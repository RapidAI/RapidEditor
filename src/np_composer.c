/* np_composer.c — M3/B1: Composer → Job → PatchSet 模型层实现.
 *
 * 无 CUI / 网络依赖；确定性本地规划器只负责把意图变成受审查的 patch_set，
 * 不直接写文档。真实模型输出后续可从同一入口替换进来。
 */
#include "np_composer.h"
#include "np_texttools.h"

#include <ctype.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NP_COMPOSER_MAX_BYTES 64

struct np_composer {
    np_registry_t *reg;
    bool reg_owned;
    np_ctx_budget_t budget;
    np_ctx_audit_t audit;

    char  path[220];
    np_track_t track;
    uint64_t revision;
    unsigned char *doc;
    size_t doc_len;

    np_composer_attach_t attaches[NP_COMPOSER_MAX_ATTACH];
    size_t nattach;

    char *ctx_text;
    size_t ctx_len;

    np_composer_state_t state;
    np_job_t *job;
    np_patch_set_t *patch;
    char patch_cite[280];
    char tool_ids[NP_COMPOSER_MAX_TOOLS][80];
    size_t ntools;
    char why[320];
};

static uint64_t s_next_job_id = 1;

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static void set_why(np_composer_t *c, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (c) vsnprintf(c->why, sizeof(c->why), fmt, ap);
    va_end(ap);
}

static void clear_result(np_composer_t *c) {
    if (!c) return;
    if (c->job) { np_job_destroy(c->job); c->job = NULL; }
    if (c->patch) { np_patch_set_destroy(c->patch); c->patch = NULL; }
    free(c->ctx_text); c->ctx_text = NULL; c->ctx_len = 0;
    memset(&c->audit, 0, sizeof(c->audit));
    memset(c->patch_cite, 0, sizeof(c->patch_cite));
    memset(c->tool_ids, 0, sizeof(c->tool_ids));
    c->ntools = 0;
    memset(c->why, 0, sizeof(c->why));
    c->state = NP_COMPOSER_IDLE;
}

static void free_attaches(np_composer_t *c) {
    for (size_t i = 0; i < c->nattach; ++i) {
        free(c->attaches[i].text);
        memset(&c->attaches[i], 0, sizeof(c->attaches[i]));
    }
    c->nattach = 0;
}

static np_patch_buffer_t doc_buffer(np_composer_t *c) {
    np_patch_buffer_t b;
    np_patch_buffer_init(&b);
    b.data = c->doc;
    b.len = c->doc_len;
    b.cap = c->doc_len;
    b.revision = c->revision;
    return b;
}

static const char *skip_ws(const char *p) {
    while (p && *p && isspace((unsigned char)*p)) ++p;
    return p;
}

static const char *find_ci(const char *hay, const char *needle) {
    if (!hay || !needle || !*needle) return NULL;
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; ++p) {
        size_t i = 0;
        for (; i < nl; ++i) {
            unsigned char a = (unsigned char)p[i], b = (unsigned char)needle[i];
            if (tolower(a) != tolower(b)) break;
        }
        if (i == nl) return p;
    }
    return NULL;
}

/* 从若干分隔词中取最早出现的一个，返回其后的非空内容。 */
static const char *value_after(const char *s, const char *const *keys,
                               size_t nkeys) {
    const char *best_kw = NULL;
    const char *best_val = NULL;
    for (size_t k = 0; k < nkeys; ++k) {
        const char *p = (keys[k][0] & 0x80) ? strstr(s, keys[k]) : find_ci(s, keys[k]);
        if (!p) continue;
        const char *v = skip_ws(p + strlen(keys[k]));
        size_t len = strlen(v);
        while (len && isspace((unsigned char)v[len - 1])) --len;
        if (len && (!best_kw || p < best_kw)) { best_kw = p; best_val = v; }
    }
    return best_val;
}

static bool parse_line_number(const char *s, size_t *line) {
    const char *p = strstr(s, "第");
    const char *q = NULL;
    if (p) {
        q = skip_ws(p + strlen("第"));
        if (!isdigit((unsigned char)*q)) q = NULL;
    }
    if (!q) {
        p = find_ci(s, "line");
        if (!p) return false;
        q = skip_ws(p + 4);
        if (!isdigit((unsigned char)*q)) return false;
    }
    char *end = NULL;
    unsigned long long n = strtoull(q, &end, 10);
    if (end == q || n == 0 || n > ULONG_MAX) return false;
    *line = (size_t)n;
    return true;
}

/* 返回 0=替换 1=插入 2=删除，-1=不支持。value 指向原始意图内的一段。 */
static int parse_line_intent(const char *s, size_t *line,
                             const char **value, size_t *vlen) {
    static const char *const del_keys[] = {"删除", "删掉", "delete", "remove"};
    static const char *const ins_keys[] = {"插入", "insert "};
    static const char *const rep_keys[] = {"改成", "改为", "替换成", "设为", "=", " to "};
    if (!s || !line || !value || !vlen) return -1;
    if (!parse_line_number(s, line)) return -1;

    int op = 0;
    for (size_t i = 0; i < sizeof(del_keys) / sizeof(del_keys[0]); ++i) {
        const char *p = (del_keys[i][0] & 0x80) ? strstr(s, del_keys[i]) : find_ci(s, del_keys[i]);
        if (p) { op = 2; break; }
    }
    if (op != 2) {
        for (size_t i = 0; i < sizeof(ins_keys) / sizeof(ins_keys[0]); ++i) {
            const char *p = (ins_keys[i][0] & 0x80) ? strstr(s, ins_keys[i]) : find_ci(s, ins_keys[i]);
            if (p) { op = 1; break; }
        }
    }

    if (op == 2) { *value = NULL; *vlen = 0; return 2; }

    const char *v = value_after(s, op == 1 ? ins_keys : rep_keys,
                                op == 1 ? sizeof(ins_keys) / sizeof(ins_keys[0])
                                        : sizeof(rep_keys) / sizeof(rep_keys[0]));
    if (!v) return -1;
    *value = skip_ws(v);
    *vlen = strlen(*value);
    while (*vlen && isspace((unsigned char)(*value)[*vlen - 1])) --*vlen;
    if (*vlen == 0) return -1;
    return op;
}

static int hex_digit(int ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

/* "0x10 改成 DE AD BE EF" / "replace 0x10 = DEADBEEF" */
static bool parse_byte_intent(const char *s, size_t *off,
                              unsigned char *bytes, size_t *nbytes) {
    if (!s || !off || !bytes || !nbytes) return false;
    const char *p = strstr(s, "0x");
    if (!p) p = strstr(s, "0X");
    if (!p) return false;
    const char *q = p + 2;
    if (hex_digit((unsigned char)*q) < 0) return false;
    char *end = NULL;
    unsigned long long v = strtoull(q, &end, 16);
    if (end == q) return false;
    *off = (size_t)v;

    static const char *const seps[] = {"改成", "改为", "=", ":", " to "};
    const char *sep = value_after(s, seps, sizeof(seps) / sizeof(seps[0]));
    if (!sep) return false;
    q = skip_ws(sep);

    size_t n = 0;
    while (*q && n < NP_COMPOSER_MAX_BYTES) {
        int hi = hex_digit((unsigned char)*q);
        if (hi < 0) { ++q; continue; }
        if (hex_digit((unsigned char)q[1]) < 0) break;
        int lo = hex_digit((unsigned char)q[1]);
        bytes[n++] = (unsigned char)((hi << 4) | lo);
        q += 2;
        if (*q == ',' || *q == ' ') ++q;
    }
    *nbytes = n;
    return n > 0;
}

/* ------------------------------------------------------------------ */
/* 生命周期                                                            */
/* ------------------------------------------------------------------ */

const char *np_composer_state_name(np_composer_state_t s) {
    switch (s) {
        case NP_COMPOSER_IDLE:    return "idle";
        case NP_COMPOSER_READY:   return "ready";
        case NP_COMPOSER_RUNNING: return "running";
        case NP_COMPOSER_DONE:    return "succeeded";
        case NP_COMPOSER_FAILED:  return "failed";
        default:                  return "unknown";
    }
}

const char *np_submit_result_name(np_submit_result_t r) {
    switch (r) {
        case NP_SUBMIT_OK:             return "ok";
        case NP_SUBMIT_ERR_ARG:        return "bad argument";
        case NP_SUBMIT_ERR_NO_DOC:     return "no document bound";
        case NP_SUBMIT_ERR_UNSUPPORTED: return "intent not supported";
        case NP_SUBMIT_ERR_STATE:      return "invalid state";
        case NP_SUBMIT_ERR_PATCH:      return "patch rejected";
        default:                       return "unknown";
    }
}

np_composer_t *np_composer_create(np_registry_t *reg,
                                  const np_ctx_budget_t *budget) {
    np_composer_t *c = (np_composer_t *)calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->reg = reg;
    if (!c->reg) {
        c->reg = np_registry_create();
        c->reg_owned = true;
        if (c->reg) np_texttools_register(c->reg);
    }
    c->budget = budget ? *budget : np_ctx_default_budget();
    c->track = NP_TRACK_TEXT;
    c->state = NP_COMPOSER_IDLE;
    return c;
}

void np_composer_destroy(np_composer_t *c) {
    if (!c) return;
    clear_result(c);
    free_attaches(c);
    free(c->doc);
    if (c->reg_owned && c->reg) np_registry_destroy(c->reg);
    free(c);
}

void np_composer_bind(np_composer_t *c, const char *path, np_track_t track,
                      uint64_t revision, const unsigned char *data, size_t len) {
    if (!c) return;
    clear_result(c);
    free_attaches(c);
    free(c->doc);
    c->doc = NULL;
    c->doc_len = 0;
    if (path) snprintf(c->path, sizeof(c->path), "%s", path);
    else c->path[0] = '\0';
    c->track = track;
    c->revision = revision;
    if (data && len) {
        c->doc = (unsigned char *)malloc(len);
        if (c->doc) { memcpy(c->doc, data, len); c->doc_len = len; }
    }
}

/* ------------------------------------------------------------------ */
/* 附件芯片                                                            */
/* ------------------------------------------------------------------ */

static bool push_attach(np_composer_t *c, np_attach_kind_t kind,
                        const char *label, const char *citation,
                        const char *text, size_t len) {
    if (!c || c->nattach >= NP_COMPOSER_MAX_ATTACH) return false;
    np_composer_attach_t *a = &c->attaches[c->nattach];
    memset(a, 0, sizeof(*a));
    a->kind = kind;
    snprintf(a->label, sizeof(a->label), "%s", label ? label : "");
    snprintf(a->citation, sizeof(a->citation), "%s", citation ? citation : "");
    a->text = (char *)malloc(len + 1);
    if (!a->text) return false;
    if (len) memcpy(a->text, text, len);
    a->text[len] = '\0';
    a->len = len;
    ++c->nattach;
    return true;
}

bool np_composer_add_text(np_composer_t *c, size_t line_first, size_t line_last) {
    if (!c || !c->doc || line_first == 0 || line_last < line_first) return false;
    np_patch_buffer_t b = doc_buffer(c);
    size_t nlines = np_patch_line_count(&b);
    if (line_last > nlines) return false;
    size_t s = np_patch_line_byte_start(&b, line_first - 1);
    size_t e = np_patch_line_byte_end(&b, line_last - 1);
    if (e < s || e > c->doc_len) return false;

    np_citation_t cite;
    memset(&cite, 0, sizeof(cite));
    cite.kind = NP_CITE_LINE;
    snprintf(cite.file, sizeof(cite.file), "%s", c->path);
    cite.start = line_first;
    cite.end = line_last;
    cite.revision = c->revision;
    char cite_text[280];
    np_citation_format(&cite, cite_text, sizeof(cite_text));

    char label[128];
    snprintf(label, sizeof(label), "文本选区 L%zu-L%zu", line_first, line_last);
    return push_attach(c, NP_ATTACH_TEXT, label, cite_text,
                       (const char *)(c->doc + s), e - s);
}

bool np_composer_add_byte(np_composer_t *c, size_t offset, size_t len) {
    if (!c || !c->doc || len == 0 || offset > c->doc_len ||
        len > c->doc_len - offset) return false;

    char *hex = (char *)malloc(len * 3 + 1);
    if (!hex) return false;
    size_t at = 0;
    for (size_t i = 0; i < len; ++i)
        at += (size_t)snprintf(hex + at, len * 3 + 1 - at, "%02X ",
                               c->doc[offset + i]);
    if (at) --at; /* 去掉末尾空格 */
    hex[at] = '\0';

    np_citation_t cite;
    memset(&cite, 0, sizeof(cite));
    cite.kind = NP_CITE_OFFSET;
    snprintf(cite.file, sizeof(cite.file), "%s", c->path);
    cite.start = offset;
    cite.end = offset + len;
    cite.revision = c->revision;
    char cite_text[280];
    np_citation_format(&cite, cite_text, sizeof(cite_text));

    char label[128];
    snprintf(label, sizeof(label), "字节选区 0x%zX + %zuB", offset, len);
    bool ok = push_attach(c, NP_ATTACH_BYTE, label, cite_text, hex, at);
    free(hex);
    return ok;
}

bool np_composer_add_format(np_composer_t *c, const char *snippet,
                            const char *citation) {
    if (!c || !snippet || !citation || !*snippet || !*citation) return false;
    np_citation_t parsed;
    if (!np_citation_parse(citation, &parsed)) return false;
    char label[128];
    snprintf(label, sizeof(label), "格式片段 %.100s",
             parsed.path[0] ? parsed.path : citation);
    return push_attach(c, NP_ATTACH_FORMAT, label, citation, snippet, strlen(snippet));
}

size_t np_composer_attach_count(const np_composer_t *c) {
    return c ? c->nattach : 0;
}

const np_composer_attach_t *np_composer_attach(const np_composer_t *c, size_t i) {
    if (!c || i >= c->nattach) return NULL;
    return &c->attaches[i];
}

void np_composer_remove_attach(np_composer_t *c, size_t i) {
    if (!c || i >= c->nattach) return;
    free(c->attaches[i].text);
    for (size_t j = i + 1; j < c->nattach; ++j)
        c->attaches[j - 1] = c->attaches[j];
    memset(&c->attaches[c->nattach - 1], 0, sizeof(c->attaches[0]));
    --c->nattach;
}

/* ------------------------------------------------------------------ */
/* 审计摘要                                                            */
/* ------------------------------------------------------------------ */

bool np_composer_audit(np_composer_t *c, char *out, size_t cap) {
    if (!c || !c->doc) return false;
    np_ctx_fragment_t frags[NP_COMPOSER_MAX_ATTACH];
    for (size_t i = 0; i < c->nattach; ++i) {
        const np_composer_attach_t *a = &c->attaches[i];
        memset(&frags[i], 0, sizeof(frags[i]));
        frags[i].track = (a->kind == NP_ATTACH_BYTE) ? NP_TRACK_BINARY
                       : (a->kind == NP_ATTACH_FORMAT) ? NP_TRACK_FORMAT
                                                       : NP_TRACK_TEXT;
        snprintf(frags[i].source, sizeof(frags[i].source), "%.259s", a->citation);
        frags[i].text = a->text;
        frags[i].len = a->len;
    }
    free(c->ctx_text);
    c->ctx_text = NULL;
    c->ctx_len = 0;
    if (!np_ctx_build(frags, c->nattach, &c->budget, NULL,
                      &c->ctx_text, &c->ctx_len, &c->audit)) {
        set_why(c, "上下文打包失败");
        return false;
    }
    if (out && cap) snprintf(out, cap, "%s", c->audit.audit);
    if (c->state == NP_COMPOSER_IDLE) c->state = NP_COMPOSER_READY;
    return true;
}

const char *np_composer_context(const np_composer_t *c, size_t *len) {
    if (len) *len = c ? c->ctx_len : 0;
    return c ? c->ctx_text : NULL;
}

/* ------------------------------------------------------------------ */
/* 提交                                                                */
/* ------------------------------------------------------------------ */

np_submit_result_t np_composer_submit(np_composer_t *c, const char *intent) {
    if (!c || !intent || !*intent) {
        if (c) set_why(c, "意图为空");
        return NP_SUBMIT_ERR_ARG;
    }
    if (!c->doc) { set_why(c, "没有绑定文档"); return NP_SUBMIT_ERR_NO_DOC; }
    if (c->state == NP_COMPOSER_RUNNING) {
        set_why(c, "已有任务在运行");
        return NP_SUBMIT_ERR_STATE;
    }
    clear_result(c);
    if (!np_composer_audit(c, NULL, 0)) return NP_SUBMIT_ERR_STATE;

    /* X2: 工具清单按意图路由，核心工具常驻 */
    c->ntools = np_registry_route(c->reg, intent, 16,
                                  NP_COMPOSER_MAX_TOOLS,
                                  c->tool_ids, NP_COMPOSER_MAX_TOOLS);

    unsigned char bytes[NP_COMPOSER_MAX_BYTES];
    size_t nbytes = 0, off = 0;
    size_t line = 0, vlen = 0;
    const char *value = NULL;
    bool is_byte = parse_byte_intent(intent, &off, bytes, &nbytes);
    int line_op = is_byte ? -1 : parse_line_intent(intent, &line, &value, &vlen);
    if (!is_byte && line_op < 0) {
        set_why(c, "未识别的意图（支持：把第 N 行改成 X / 在第 N 行插入 X / 删除第 N 行 / 0xOFF 改成 XX XX）");
        c->state = NP_COMPOSER_FAILED;
        return NP_SUBMIT_ERR_UNSUPPORTED;
    }

    np_track_t track = is_byte ? NP_TRACK_BINARY : NP_TRACK_TEXT;
    np_job_kind_t kind = is_byte ? NP_JOB_KIND_BYTE_PATCH : NP_JOB_KIND_TEXT_PATCH;
    np_budget_t bud = np_job_default_budget();
    bud.max_context_bytes = c->ctx_len;
    np_provenance_t prov;
    memset(&prov, 0, sizeof(prov));
    snprintf(prov.tool, sizeof(prov.tool), "composer.local");
    snprintf(prov.source, sizeof(prov.source), "%.159s", c->path);
    prov.revision = c->revision;

    np_job_t *job = np_job_create(s_next_job_id++, track, kind, &bud, &prov);
    if (!job) {
        set_why(c, "创建 Job 失败");
        c->state = NP_COMPOSER_FAILED;
        return NP_SUBMIT_ERR_STATE;
    }
    c->job = job;
    if (!np_job_queue(job) || !np_job_start(job)) {
        set_why(c, "Job 状态机启动失败");
        np_job_fail(job);
        c->state = NP_COMPOSER_FAILED;
        return NP_SUBMIT_ERR_STATE;
    }
    np_job_set_progress(job, 35);

    np_patch_set_t *ps = np_patch_set_create(c->revision);
    if (!ps) {
        set_why(c, "创建 PatchSet 失败");
        np_job_fail(job);
        c->state = NP_COMPOSER_FAILED;
        return NP_SUBMIT_ERR_PATCH;
    }
    np_patch_result_t pr = NP_PATCH_ERR_ARG;
    if (is_byte) {
        pr = np_patch_add_byte(ps, NP_OP_REPLACE, off, nbytes, bytes, nbytes);
        snprintf(c->patch_cite, sizeof(c->patch_cite), "%s:0x%zX-0x%zX",
                 c->path, off, off + nbytes);
    } else if (line_op == 0) {
        char *text = (char *)malloc(vlen + 2);
        if (text) {
            memcpy(text, value, vlen);
            text[vlen] = '\n';
            text[vlen + 1] = '\0';
            pr = np_patch_add_text(ps, NP_OP_REPLACE, line - 1, line,
                                   text, vlen + 1);
            free(text);
        }
        snprintf(c->patch_cite, sizeof(c->patch_cite), "%s:%zu", c->path, line);
    } else if (line_op == 1) {
        char *text = (char *)malloc(vlen + 2);
        if (text) {
            memcpy(text, value, vlen);
            text[vlen] = '\n';
            text[vlen + 1] = '\0';
            pr = np_patch_add_text(ps, NP_OP_INSERT, line - 1, line - 1,
                                   text, vlen + 1);
            free(text);
        }
        snprintf(c->patch_cite, sizeof(c->patch_cite), "%s:%zu", c->path, line);
    } else {
        pr = np_patch_add_text(ps, NP_OP_DELETE, line - 1, line, NULL, 0);
        snprintf(c->patch_cite, sizeof(c->patch_cite), "%s:%zu", c->path, line);
    }
    if (pr != NP_PATCH_OK) {
        set_why(c, "构造补丁失败: %s", np_patch_result_name(pr));
        np_patch_set_destroy(ps);
        np_job_fail(job);
        c->state = NP_COMPOSER_FAILED;
        return NP_SUBMIT_ERR_PATCH;
    }

    np_patch_buffer_t b = doc_buffer(c);
    pr = np_patch_validate(ps, track, &b);
    if (pr != NP_PATCH_OK) {
        set_why(c, "补丁校验失败: %s", np_patch_result_name(pr));
        np_patch_set_destroy(ps);
        np_job_fail(job);
        c->state = NP_COMPOSER_FAILED;
        return NP_SUBMIT_ERR_PATCH;
    }

    np_job_set_progress(job, 85);
    np_job_append_output(job, c->patch_cite, strlen(c->patch_cite));
    if (!np_job_finish(job)) {
        set_why(c, "Job 未能结束");
        np_patch_set_destroy(ps);
        c->state = NP_COMPOSER_FAILED;
        return NP_SUBMIT_ERR_STATE;
    }
    c->patch = ps;
    c->state = NP_COMPOSER_DONE;
    return NP_SUBMIT_OK;
}

/* ------------------------------------------------------------------ */
/* 访问器 / 校验                                                       */
/* ------------------------------------------------------------------ */

np_job_t *np_composer_job(np_composer_t *c) { return c ? c->job : NULL; }

const np_patch_set_t *np_composer_patch(const np_composer_t *c) {
    return c ? c->patch : NULL;
}

np_composer_state_t np_composer_state(const np_composer_t *c) {
    return c ? c->state : NP_COMPOSER_IDLE;
}

size_t np_composer_routed_tools(const np_composer_t *c,
                                char ids[][80], size_t cap) {
    if (!c || !ids) return 0;
    size_t n = c->ntools < cap ? c->ntools : cap;
    for (size_t i = 0; i < n; ++i) snprintf(ids[i], 80, "%s", c->tool_ids[i]);
    return n;
}

size_t np_composer_citations(const np_composer_t *c,
                             char out[][280], size_t cap) {
    if (!c || !out) return 0;
    size_t n = 0;
    for (size_t i = 0; i < c->nattach && n < cap; ++i)
        snprintf(out[n++], 280, "%s", c->attaches[i].citation);
    if (c->patch_cite[0] && n < cap)
        snprintf(out[n++], 280, "%s", c->patch_cite);
    return n;
}

/* 字节区间 reader（np_citation_resolve 的 OFFSET 分支） */
static long local_reader(const char *file, size_t start, size_t end,
                         unsigned char *buf, size_t cap, void *user) {
    np_composer_t *c = (np_composer_t *)user;
    if (!c || !c->doc || !file) return -1;
    if (*file && strcmp(file, c->path) != 0) return -1;
    if (end < start || end > c->doc_len) return -1;
    size_t n = end - start;
    if (buf && cap) {
        if (n > cap) n = cap;
        memcpy(buf, c->doc + start, n);
    }
    return (long)n;
}

/* 行号 reader（LINE 分支：start/end 为 1-based 行号，含端点） */
static long local_line_reader(const char *file, size_t start, size_t end,
                              unsigned char *buf, size_t cap, void *user) {
    np_composer_t *c = (np_composer_t *)user;
    if (!c || !c->doc) return -1;
    np_patch_buffer_t b = doc_buffer(c);
    size_t nlines = np_patch_line_count(&b);
    if (start == 0 || end < start || end > nlines) return -1;
    size_t s = np_patch_line_byte_start(&b, start - 1);
    size_t e = np_patch_line_byte_end(&b, end - 1);
    return local_reader(file, s, e, buf, cap, c);
}

bool np_composer_verify_citations(np_composer_t *c, char *why, size_t cap) {
    if (!c || !why || cap == 0) return false;
    char cites[NP_COMPOSER_MAX_CITES][280];
    size_t n = np_composer_citations(c, cites, NP_COMPOSER_MAX_CITES);
    if (n == 0) { snprintf(why, cap, "没有 Citation 可验证"); return false; }
    for (size_t i = 0; i < n; ++i) {
        np_citation_t cite;
        if (!np_citation_parse(cites[i], &cite)) {
            snprintf(why, cap, "Citation 语法无效: %s", cites[i]);
            return false;
        }
        if (strcmp(cite.file, c->path) != 0) {
            snprintf(why, cap, "Citation 来源不匹配: %s", cites[i]);
            return false;
        }
        if (cite.kind == NP_CITE_STRUCT || cite.kind == NP_CITE_EXTERNAL) {
            snprintf(why, cap, "%s 需格式轨 reader 才能读回", cites[i]);
            return false;
        }
        /* 从本地缓冲区读回摘录（绝不信任模型给的文本），并算 CRC32 摘要。 */
        unsigned char excerpt[512];
        size_t got = 0;
        uint32_t hash = 0;
        np_cite_reader reader = (cite.kind == NP_CITE_LINE)
                              ? local_line_reader : local_reader;
        bool ok = np_citation_resolve(&cite, reader, c,
                                      excerpt, sizeof(excerpt), &got, &hash);
        if (!ok) {
            snprintf(why, cap, "Citation 本地读回失败: %s", cites[i]);
            return false;
        }
    }
    snprintf(why, cap, "已本地读回验证 %zu 条 Citation", n);
    return true;
}

const char *np_composer_why(const np_composer_t *c) {
    return c ? c->why : "";
}

void np_composer_reset(np_composer_t *c) { clear_result(c); }
