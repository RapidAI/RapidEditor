/* np_featcard.c — F4: AI 特征抽取与特征卡（实现）.
 * 纯 C99、UI-free、只读；复用 F2/F3 的只读解析结果，不碰源字节。
 */
#include "np_featcard.h"
#include "np_ooxml.h"
#include "np_sqlite.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- 小工具（有界、无 C4996） ---------- */
static void fscpy(char *dst, size_t cap, const char *src)
{
    size_t i = 0;
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    while (i + 1 < cap && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static bool appendf(char *out, size_t cap, size_t *used, const char *fmt, ...)
{
    va_list ap;
    int n;
    if (*used >= cap) return false;
    va_start(ap, fmt);
    n = vsnprintf(out + *used, cap - *used, fmt, ap);
    va_end(ap);
    if (n < 0) return false;
    if ((size_t)n >= cap - *used) return false;
    *used += (size_t)n;
    return true;
}

/* 把 '|' / 换行转义为表格安全文本 */
static void tbl_escape(const char *in, char *out, size_t cap)
{
    size_t i = 0, o = 0;
    if (!in) { if (cap) out[0] = '\0'; return; }
    while (in[i] && o + 2 < cap) {
        if (in[i] == '|' || in[i] == '\n' || in[i] == '\r' || in[i] == '\t') {
            out[o++] = ' ';
        } else {
            out[o++] = in[i];
        }
        i++;
    }
    out[o] = '\0';
}

/* ---------- 名称表 ---------- */
const char *np_fc_status_name(np_fc_status_t s)
{
    switch (s) {
    case NP_FC_OK:             return "OK";
    case NP_FC_ERR_ARG:        return "ARG";
    case NP_FC_ERR_FULL:       return "FULL";
    case NP_FC_ERR_DUP:        return "DUP";
    case NP_FC_ERR_LOW_CONF:   return "LOW_CONF";
    case NP_FC_ERR_NO_EVIDENCE:return "NO_EVIDENCE";
    case NP_FC_ERR_PARSE:      return "PARSE";
    case NP_FC_ERR_UNSUPPORTED:return "UNSUPPORTED";
    }
    return "?";
}

const char *np_fc_domain_name(np_fc_domain_t d)
{
    return d == NP_FC_DB ? "数据库" : "文档";
}

const char *np_fc_kind_name(np_fc_kind_t k)
{
    switch (k) {
    case NP_FC_FORMAT:   return "格式";
    case NP_FC_STRUCTURE:return "结构";
    case NP_FC_SEMANTIC: return "语义";
    case NP_FC_QUALITY:  return "质量";
    case NP_FC_SECURITY: return "安全";
    }
    return "?";
}

const char *np_fc_level_name(np_fc_level_t l)
{
    switch (l) {
    case NP_FC_L0: return "L0";
    case NP_FC_L1: return "L1";
    case NP_FC_L2: return "L2";
    }
    return "?";
}

/* ---------- PII 检测与脱敏 ---------- */
static bool secret_prefix(const char *p, size_t *len)
{
    static const char *const pref[] = {
        "sk-", "ghp_", "github_pat_", "AKIA", "xoxb_", "-----BEGIN"
    };
    size_t i;
    for (i = 0; i < sizeof(pref) / sizeof(pref[0]); i++) {
        size_t n = strlen(pref[i]);
        if (strncmp(p, pref[i], n) == 0) { *len = n; return true; }
    }
    *len = 0;
    return false;
}

static bool is_email_char(unsigned char c)
{
    return isalnum(c) || c == '.' || c == '_' || c == '%' || c == '+' ||
           c == '-' || c == '@';
}

static bool is_email_token(const char *s, size_t n)
{
    size_t i, at = (size_t)-1, dot_after = 0;
    if (n < 5 || n > 200) return false;
    for (i = 0; i < n; i++) {
        if (s[i] == '@') {
            if (at != (size_t)-1) return false;
            at = i;
        } else if (s[i] == '.' && at != (size_t)-1 && i > at) {
            dot_after++;
        }
    }
    return at > 0 && at + 1 < n && dot_after > 0;
}

bool np_fc_looks_like_pii(const char *s, const char **reason)
{
    const char *p;
    if (!s) return false;
    /* 密钥形态 */
    for (p = s; *p; p++) {
        size_t sl = 0;
        if (secret_prefix(p, &sl)) {
            if (reason) *reason = "凭证形态";
            return true;
        }
        /* 邮箱 */
        if (is_email_char((unsigned char)*p) && *p != '@') {
            const char *q = p;
            while (*q && is_email_char((unsigned char)*q)) q++;
            if (is_email_token(p, (size_t)(q - p))) {
                if (reason) *reason = "邮箱";
                return true;
            }
        }
        /* 连续数字：身份证 18 位 / 17 位+X / 手机号 11 位 / 长数字 */
        if (isdigit((unsigned char)*p)) {
            const char *q = p;
            size_t nd = 0;
            while (*q && isdigit((unsigned char)*q)) { q++; nd++; }
            if (nd == 18) {
                if (reason) *reason = "身份证号";
                return true;
            }
            if (nd == 17 && (*q == 'X' || *q == 'x')) {
                if (reason) *reason = "身份证号";
                return true;
            }
            if (nd == 11 && p[0] == '1' && p[1] >= '3' && p[1] <= '9') {
                if (reason) *reason = "手机号";
                return true;
            }
            if (nd >= 12) {
                if (reason) *reason = "长数字";
                return true;
            }
            p = q - 1;
        }
    }
    return false;
}

void np_fc_redact(const char *in, char *out, size_t cap)
{
    const char *p;
    size_t o = 0;
    if (!out || cap == 0) return;
    if (!in) { out[0] = '\0'; return; }
    p = in;
    while (*p && o + 1 < cap) {
        size_t sl = 0;
        if (secret_prefix(p, &sl)) {
            const char *tag = "[SECRET]";
            while (*tag && o + 1 < cap) out[o++] = *tag++;
            p += sl;
            continue;
        }
        if (is_email_char((unsigned char)*p) && *p != '@') {
            const char *q = p;
            while (*q && is_email_char((unsigned char)*q)) q++;
            if (is_email_token(p, (size_t)(q - p))) {
                const char *tag = "[EMAIL]";
                while (*tag && o + 1 < cap) out[o++] = *tag++;
                p = q;
                continue;
            }
        }
        if (isdigit((unsigned char)*p)) {
            const char *q = p;
            size_t nd = 0;
            while (*q && isdigit((unsigned char)*q)) { q++; nd++; }
            if (nd == 18 || (nd == 17 && (*q == 'X' || *q == 'x'))) {
                const char *tag = "[ID]";
                while (*tag && o + 1 < cap) out[o++] = *tag++;
                if (nd == 17) q++;
                p = q;
                continue;
            }
            if (nd == 11 && p[0] == '1' && p[1] >= '3' && p[1] <= '9') {
                const char *tag = "[PHONE]";
                while (*tag && o + 1 < cap) out[o++] = *tag++;
                p = q;
                continue;
            }
            if (nd >= 12) {
                const char *tag = "[NUM]";
                while (*tag && o + 1 < cap) out[o++] = *tag++;
                p = q;
                continue;
            }
            while (p < q && o + 1 < cap) out[o++] = *p++;
            continue;
        }
        out[o++] = *p++;
    }
    out[o] = '\0';
}

/* ---------- 卡核心 ---------- */
void np_fc_init(np_fc_card_t *c, np_fc_domain_t domain, const char *source)
{
    if (!c) return;
    memset(c, 0, sizeof(*c));
    c->domain = domain;
    fscpy(c->source, sizeof(c->source), source ? source : "");
}

static bool has_evidence(const char *e)
{
    return e && (strchr(e, '#') != NULL || strstr(e, ":0x") != NULL);
}

const np_fc_feature_t *np_fc_find(const np_fc_card_t *c, const char *id)
{
    int i;
    if (!c || !id || !*id) return NULL;
    for (i = 0; i < c->n; i++) {
        if (strcmp(c->items[i].id, id) == 0) return &c->items[i];
    }
    return NULL;
}

np_fc_status_t np_fc_add(np_fc_card_t *c, np_fc_kind_t kind, np_fc_level_t level,
                         const char *id, const char *name, const char *value,
                         const char *evidence, int conf)
{
    np_fc_feature_t *f;
    if (!c || !id || !*id || !name || !*name || !value || !evidence)
        return NP_FC_ERR_ARG;
    if (kind < NP_FC_FORMAT || kind > NP_FC_SECURITY ||
        level < NP_FC_L0 || level > NP_FC_L2 || conf < 0 || conf > 100)
        return NP_FC_ERR_ARG;
    if (!has_evidence(evidence)) {
        c->rejected_no_evidence++;
        return NP_FC_ERR_NO_EVIDENCE;
    }
    if (conf < NP_FC_MIN_CONF) {
        c->rejected_low_conf++;
        return NP_FC_ERR_LOW_CONF;
    }
    if (np_fc_find(c, id)) {
        c->rejected_dup++;
        return NP_FC_ERR_DUP;
    }
    if (c->n >= NP_FC_MAX_FEATURES) { c->truncated = true; return NP_FC_ERR_FULL; }
    f = &c->items[c->n];
    memset(f, 0, sizeof(*f));
    f->kind = kind;
    f->level = level;
    f->conf = conf;
    fscpy(f->id, sizeof(f->id), id);
    fscpy(f->name, sizeof(f->name), name);
    np_fc_redact(value, f->value, sizeof(f->value));
    if (strcmp(f->value, value) != 0) c->redacted++;
    fscpy(f->evidence, sizeof(f->evidence), evidence);
    c->n++;
    return NP_FC_OK;
}

int np_fc_render(const np_fc_card_t *c, char *out, size_t cap)
{
    size_t u = 0;
    int i, n0 = 0, n1 = 0, n2 = 0;
    if (!c || !out || cap < 128) return -1;
    for (i = 0; i < c->n; i++) {
        if (c->items[i].level == NP_FC_L0) n0++;
        else if (c->items[i].level == NP_FC_L1) n1++;
        else n2++;
    }
    if (!appendf(out, cap, &u, "# 特征卡 · %s\n\n", c->source)) return -1;
    if (!appendf(out, cap, &u, "- 领域：%s\n", np_fc_domain_name(c->domain))) return -1;
    if (!appendf(out, cap, &u, "- 特征数：%d（L0 %d / L1 %d / L2 %d）\n",
                 c->n, n0, n1, n2)) return -1;
    if (!appendf(out, cap, &u,
        "- 门禁：低置信度丢弃 %d / 无证据丢弃 %d / 重复 id %d / PII 脱敏 %d%s\n",
        c->rejected_low_conf, c->rejected_no_evidence, c->rejected_dup,
        c->redacted, c->truncated ? " / 已达上限" : "")) return -1;
    if (!appendf(out, cap, &u,
        "- 证据等级：L0=解析器确定性事实 · L1=工具推导（命名/结构约定） · "
        "L2=AI 候选启发式（需人工确认）\n\n")) return -1;
    if (!appendf(out, cap, &u,
        "| id | 类别 | 等级 | 置信度 | 值 | 证据 |\n"
        "|---|---|---|---|---|---|\n")) return -1;
    for (i = 0; i < c->n; i++) {
        const np_fc_feature_t *f = &c->items[i];
        char v[NP_FC_MAX_VALUE + 8], e[NP_FC_MAX_EVIDENCE + 8];
        tbl_escape(f->value, v, sizeof(v));
        tbl_escape(f->evidence, e, sizeof(e));
        if (!appendf(out, cap, &u, "| %s | %s | %s | %d | %s | %s |\n",
                     f->id, np_fc_kind_name(f->kind), np_fc_level_name(f->level),
                     f->conf, v, e)) return -1;
    }
    return (int)u;
}

/* ---------- F2 OOXML → 特征 ---------- */
static bool stopword(const char *w, size_t n)
{
    static const char *const sw[] = {
        "the","and","for","with","this","that","from","are","was","were",
        "has","have","had","but","not","you","your","its","into","than",
        "then","they","them","their","which","will","would","about","been",
        "over","under","also","can","may","all","any","one","two","more"
    };
    size_t i;
    for (i = 0; i < sizeof(sw) / sizeof(sw[0]); i++) {
        if (strlen(sw[i]) == n && strncmp(w, sw[i], n) == 0) return true;
    }
    return false;
}

/* 从文本抽取 ASCII 词频（长度 4..63），返回按频次降序的前 top 个 */
static int word_topics(const char *text,
                       char words[3][32], int freq[3])
{
    char pool[64][32];
    int pf[64];
    int npool = 0, i, j;
    memset(pool, 0, sizeof(pool));
    memset(pf, 0, sizeof(pf));
    memset(words, 0, 3 * 32);
    memset(freq, 0, 3 * sizeof(int));
    i = 0;
    while (text[i] && i < NP_FC_MAX_TEXT) {
        size_t n = 0;
        char buf[64];
        if (!isalpha((unsigned char)text[i])) { i++; continue; }
        while (text[i + n] && isalpha((unsigned char)text[i + n]) && n < 63) n++;
        if (n >= 4) {
            size_t k;
            for (k = 0; k < n; k++) buf[k] = (char)tolower((unsigned char)text[i + k]);
            buf[n] = '\0';
            if (!stopword(buf, n)) {
                int found = -1;
                for (j = 0; j < npool; j++) {
                    if (strcmp(pool[j], buf) == 0) { found = j; break; }
                }
                if (found >= 0) pf[found]++;
                else if (npool < 64) {
                    fscpy(pool[npool], sizeof(pool[npool]), buf);
                    pf[npool] = 1;
                    npool++;
                }
            }
        }
        i += (int)n;
    }
    for (i = 0; i < 3; i++) {
        int best = -1;
        for (j = 0; j < npool; j++) {
            if (pf[j] > 0 && (best < 0 || pf[j] > pf[best])) best = j;
        }
        if (best < 0) break;
        fscpy(words[i], sizeof(words[i]), pool[best]);
        freq[i] = pf[best];
        pf[best] = 0;
    }
    return i;
}

np_fc_status_t np_fc_extract_ooxml(const char *path, const char *source,
                                   np_fc_card_t *out)
{
    np_ox_doc_t doc;
    np_ox_status_t st;
    int i, npara = 0, ntbl = 0, ncell = 0, nstyle = 0;
    int nformula = 0, ncomment = 0, nrevision = 0, nsheet = 0;
    char ev[NP_FC_MAX_EVIDENCE];
    char text[NP_FC_MAX_TEXT + 1];
    size_t tlen = 0;
    if (!path || !source || !out) return NP_FC_ERR_ARG;
    st = np_ox_parse_file(path, &doc);
    if (st != NP_OX_OK) return NP_FC_ERR_PARSE;
    np_fc_init(out, NP_FC_DOC, source);

    (void)snprintf(ev, sizeof(ev), "%s#/[Content_Types].xml", source);
    (void)np_fc_add(out, NP_FC_FORMAT, NP_FC_L0, "doc.type", "文档类型",
                    np_ox_kind_name(doc.kind), ev, 100);
    for (i = 0; i < doc.nnodes; i++) {
        const np_ox_node_t *nd = &doc.nodes[i];
        switch (nd->type) {
        case NP_OX_NODE_PARAGRAPH: npara++; break;
        case NP_OX_NODE_TABLE:     ntbl++;  break;
        case NP_OX_NODE_CELL:      ncell++; break;
        case NP_OX_NODE_STYLE:     nstyle++;break;
        case NP_OX_NODE_FORMULA:   nformula++;break;
        case NP_OX_NODE_COMMENT:   ncomment++;break;
        case NP_OX_NODE_REVISION:  nrevision++;break;
        case NP_OX_NODE_SHEET:     nsheet++;break;
        default: break;
        }
        if ((nd->type == NP_OX_NODE_PARAGRAPH || nd->type == NP_OX_NODE_CELL) &&
            tlen + strlen(nd->label) + 1 < sizeof(text)) {
            size_t k = 0;
            while (nd->label[k] && tlen + 1 < sizeof(text)) {
                text[tlen++] = nd->label[k++];
            }
            text[tlen++] = ' ';
        }
    }
    text[tlen] = '\0';

    (void)snprintf(ev, sizeof(ev), "%s#/word/document.xml", source);
    {
        char v[32];
        (void)snprintf(v, sizeof(v), "%d", doc.nparts);
        (void)np_fc_add(out, NP_FC_STRUCTURE, NP_FC_L0, "doc.parts", "部件数",
                        v, ev, 100);
    }
    if (npara) {
        char v[32];
        (void)snprintf(v, sizeof(v), "%d", npara);
        (void)snprintf(ev, sizeof(ev), "%s#/word/document.xml#paragraph", source);
        (void)np_fc_add(out, NP_FC_STRUCTURE, NP_FC_L0, "doc.paragraphs", "段落数",
                        v, ev, 100);
    }
    if (ntbl || ncell) {
        char v[48];
        (void)snprintf(v, sizeof(v), "%d 表 / %d 单元格", ntbl, ncell);
        (void)snprintf(ev, sizeof(ev), "%s#/word/document.xml#/w:tbl[0]", source);
        (void)np_fc_add(out, NP_FC_STRUCTURE, NP_FC_L0, "doc.tables", "表格结构",
                        v, ev, 100);
    }
    if (nsheet) {
        char v[32];
        (void)snprintf(v, sizeof(v), "%d", nsheet);
        (void)snprintf(ev, sizeof(ev), "%s#/xl/workbook.xml#sheet", source);
        (void)np_fc_add(out, NP_FC_STRUCTURE, NP_FC_L0, "doc.sheets", "工作表数",
                        v, ev, 100);
    }
    if (nstyle) {
        char v[32];
        (void)snprintf(v, sizeof(v), "%d", nstyle);
        (void)snprintf(ev, sizeof(ev), "%s#/word/styles.xml#style", source);
        (void)np_fc_add(out, NP_FC_STRUCTURE, NP_FC_L0, "doc.styles", "样式数",
                        v, ev, 100);
    }
    if (nformula || ncomment || nrevision) {
        char v[80];
        (void)snprintf(v, sizeof(v), "公式 %d / 批注 %d / 修订 %d",
                       nformula, ncomment, nrevision);
        (void)snprintf(ev, sizeof(ev), "%s#/word/document.xml", source);
        (void)np_fc_add(out, NP_FC_STRUCTURE, NP_FC_L0, "doc.annotations",
                        "注释与修订", v, ev, 100);
    }
    /* 主题词频（L2，需人工确认） */
    {
        char words[3][32];
        int freq[3];
        int nw = word_topics(text, words, freq);
        int k;
        for (k = 0; k < nw; k++) {
            char id[NP_FC_MAX_ID], name[NP_FC_MAX_NAME], v[96];
            int conf = 40 + freq[k] * 10;
            if (conf > 90) conf = 90;
            (void)snprintf(id, sizeof(id), "doc.topic.%d", k + 1);
            (void)snprintf(name, sizeof(name), "主题候选 %d", k + 1);
            (void)snprintf(v, sizeof(v), "%s（出现 %d 次）", words[k], freq[k]);
            (void)snprintf(ev, sizeof(ev), "%s#/word/document.xml#topic", source);
            (void)np_fc_add(out, NP_FC_SEMANTIC, NP_FC_L2, id, name, v, ev, conf);
        }
    }
    /* 异常段落（L1）：过短或过长 */
    for (i = 0; i < doc.nnodes && i < 32; i++) {
        const np_ox_node_t *nd = &doc.nodes[i];
        size_t nl;
        if (nd->type != NP_OX_NODE_PARAGRAPH) continue;
        nl = strlen(nd->label);
        if (nl >= 3 && nl <= 180) continue;
        {
            char id[NP_FC_MAX_ID], v[96];
            (void)snprintf(id, sizeof(id), "doc.anomaly.para%d", i);
            (void)snprintf(v, sizeof(v), "长度 %lu", (unsigned long)nl);
            (void)fscpy(ev, sizeof(ev), nd->path);
            if (!has_evidence(ev)) {
                (void)snprintf(ev, sizeof(ev), "%s#paragraph[%d]", source, i);
            }
            (void)np_fc_add(out, NP_FC_QUALITY, NP_FC_L1, id, "异常段落",
                            v, ev, 70);
        }
        break; /* 只报首个，避免噪声 */
    }
    /* PII 形态（安全，L0 形态检测） */
    if (np_fc_looks_like_pii(text, NULL)) {
        (void)snprintf(ev, sizeof(ev), "%s#/word/document.xml#pii", source);
        (void)np_fc_add(out, NP_FC_SECURITY, NP_FC_L0, "doc.pii", "敏感信息形态",
                        "检测到邮箱/证件/手机号/凭证形态（已脱敏）", ev, 100);
    }
    return NP_FC_OK;
}

/* ---------- F3 SQLite → 特征 ---------- */
static unsigned char *read_whole_file(const char *path, size_t *len)
{
    FILE *f;
    long sz;
    unsigned char *buf;
    size_t got;
    if (!path || !len) return NULL;
#if defined(_MSC_VER)
    if (fopen_s(&f, path, "rb") != 0) return NULL;
#else
    f = fopen(path, "rb");
    if (!f) return NULL;
#endif
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    sz = ftell(f);
    if (sz < 16 || sz > 64 * 1024 * 1024) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    buf = (unsigned char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = '\0';
    *len = got;
    return buf;
}

static bool is_pii_col(const char *n)
{
    static const char *const kw[] = {
        "email","phone","mobile","tel","idcard","id_card","身份证","手机",
        "passport","address","password","passwd","secret","token","apikey","api_key"
    };
    size_t i;
    for (i = 0; i < sizeof(kw) / sizeof(kw[0]); i++) {
        const char *p = n;
        while ((p = strstr(p, kw[i])) != NULL) {
            return true;
        }
    }
    return false;
}

/* 列名关键词 → 语义标签；未命中返回 NULL */
static const char *col_semantic(const char *n)
{
    static const struct { const char *kw; const char *label; } rules[] = {
        {"email", "email 地址"}, {"phone", "电话号码"}, {"mobile", "手机号"},
        {"name", "名称/姓名"}, {"title", "标题"}, {"date", "日期"},
        {"time", "时间"}, {"created", "创建时间"}, {"updated", "更新时间"},
        {"amount", "金额"}, {"price", "价格"}, {"total", "合计"},
        {"salary", "薪酬"}, {"count", "计数"}, {"num", "数量"},
        {"qty", "数量"}, {"status", "状态"}, {"state", "状态"},
        {"type", "类型"}, {"addr", "地址"}, {"url", "链接"},
        {"id", "标识符"}
    };
    size_t i;
    for (i = 0; i < sizeof(rules) / sizeof(rules[0]); i++) {
        if (strstr(n, rules[i].kw) != NULL) return rules[i].label;
    }
    return NULL;
}

np_fc_status_t np_fc_extract_sqlite(const char *path, const char *source,
                                    np_fc_card_t *out)
{
    size_t len = 0;
    unsigned char *data;
    np_db_schema_t db;
    int i, j;
    char ev[NP_FC_MAX_EVIDENCE];
    if (!path || !source || !out) return NP_FC_ERR_ARG;
    data = read_whole_file(path, &len);
    if (!data) return NP_FC_ERR_PARSE;
    if (np_db_parse(data, len, &db) != NP_DB_OK) {
        free(data);
        return NP_FC_ERR_PARSE;
    }
    np_fc_init(out, NP_FC_DB, source);
    free(data);

    (void)snprintf(ev, sizeof(ev), "%s#/sqlite_master", source);
    (void)np_fc_add(out, NP_FC_FORMAT, NP_FC_L0, "db.format", "数据库格式",
                    "SQLite", ev, 100);
    {
        char v[64];
        (void)snprintf(v, sizeof(v), "%lu 字节", (unsigned long)db.page_size);
        (void)np_fc_add(out, NP_FC_STRUCTURE, NP_FC_L0, "db.page_size", "页大小",
                        v, ev, 100);
        (void)snprintf(v, sizeof(v), "%lu 页", (unsigned long)db.n_pages);
        (void)np_fc_add(out, NP_FC_STRUCTURE, NP_FC_L0, "db.pages", "页数",
                        v, ev, 100);
        (void)np_fc_add(out, NP_FC_STRUCTURE, NP_FC_L0, "db.wal", "WAL 模式",
                        db.wal ? "是" : "否", ev, 100);
    }
    for (i = 0; i < db.ntables; i++) {
        const np_db_table_t *t = &db.tables[i];
        char tid[NP_FC_MAX_ID], v[96];
        (void)snprintf(tid, sizeof(tid), "db.table.%s", t->name);
        (void)snprintf(ev, sizeof(ev), "%s#table.%s", source, t->name);
        (void)snprintf(v, sizeof(v), "%d 列 / %lu 行", t->ncols,
                       (unsigned long)t->est_rows);
        (void)np_fc_add(out, NP_FC_STRUCTURE, NP_FC_L0, tid, "表结构",
                        v, ev, 100);
        {
            char pid[NP_FC_MAX_ID];
            (void)snprintf(pid, sizeof(pid), "db.table.%s.pk", t->name);
            (void)np_fc_add(out, NP_FC_STRUCTURE, NP_FC_L0, pid, "主键",
                            t->rowid_alias ? "INTEGER 主键别名" :
                            (strstr(t->sql, "PRIMARY KEY") ? "显式主键" : "无"),
                            ev, 100);
        }
        for (j = 0; j < t->ncols; j++) {
            const np_db_col_t *col = &t->cols[j];
            char cid[NP_FC_MAX_ID], cev[NP_FC_MAX_EVIDENCE];
            const char *sem = col_semantic(col->name);
            (void)snprintf(cev, sizeof(cev), "%s#table.%s.col.%s",
                           source, t->name, col->name);
            if (sem && *sem) {
                (void)snprintf(cid, sizeof(cid), "db.table.%s.col.%s.semantic",
                               t->name, col->name);
                (void)np_fc_add(out, NP_FC_SEMANTIC, NP_FC_L2, cid, "列语义推断",
                                sem, cev, 60);
            }
            if (is_pii_col(col->name)) {
                (void)snprintf(cid, sizeof(cid), "db.table.%s.col.%s.pii",
                               t->name, col->name);
                (void)np_fc_add(out, NP_FC_SECURITY, NP_FC_L1, cid, "PII 列标记",
                                "列名命中敏感字段形态", cev, 85);
            }
            /* 外键推断：xxx_id 且存在表 xxx（含去复数 s） */
            {
                size_t nl = strlen(col->name);
                if (nl > 3 && strcmp(col->name + nl - 3, "_id") == 0) {
                    char base[NP_DB_MAX_NAME];
                    size_t bl;
                    fscpy(base, sizeof(base), col->name);
                    base[nl - 3] = '\0';                 /* "user_id" → "user" */
                    bl = strlen(base);
                    /* 列名单数 / 表名复数：先试原名，再加 s（user→users） */
                    if (!np_db_find_table(&db, base) && bl + 1 < sizeof(base)) {
                        base[bl] = 's';
                        base[bl + 1] = '\0';
                    }
                    if (np_db_find_table(&db, base)) {
                        char fid[NP_FC_MAX_ID], fv[NP_FC_MAX_VALUE];
                        (void)snprintf(fid, sizeof(fid),
                                       "db.table.%s.col.%s.fk", t->name, col->name);
                        (void)snprintf(fv, sizeof(fv), "推断指向表 %s（仅命名约定）",
                                       base);
                        (void)np_fc_add(out, NP_FC_SEMANTIC, NP_FC_L1, fid,
                                        "外键推断", fv, cev, 70);
                    }
                }
            }
        }
    }
    /* 数据质量：无主键表 / 解析不完整 */
    for (i = 0; i < db.ntables; i++) {
        const np_db_table_t *t = &db.tables[i];
        if (!t->rowid_alias && !strstr(t->sql, "PRIMARY KEY")) {
            char qid[NP_FC_MAX_ID];
            (void)snprintf(qid, sizeof(qid), "db.table.%s.nopk", t->name);
            (void)snprintf(ev, sizeof(ev), "%s#table.%s", source, t->name);
            (void)np_fc_add(out, NP_FC_QUALITY, NP_FC_L1, qid, "无主键表",
                            "未发现主键或 rowid 别名", ev, 80);
        }
    }
    if (db.partially_parsed) {
        (void)snprintf(ev, sizeof(ev), "%s#/sqlite_master", source);
        (void)np_fc_add(out, NP_FC_QUALITY, NP_FC_L0, "db.partial", "解析受限",
                        "schema 未完全解析（UTF-16/溢出/超限）", ev, 100);
    }
    out->truncated = out->truncated || db.partially_parsed;
    return NP_FC_OK;
}
