/* np_extlife.c — X12: 扩展生命周期审计、回滚与清理（实现）.
 * 纯 C99、UI-free；只记元数据 + 注入式清理事务，不直接删文件。
 */
#include "np_extlife.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- 工具 ---------- */
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
    if (n < 0 || (size_t)n >= cap - *used) return false;
    *used += (size_t)n;
    return true;
}

const char *np_xl_status_name(np_xl_status_t s)
{
    switch (s) {
    case NP_XL_OK: return "OK";
    case NP_XL_ERR_ARG: return "ARG";
    case NP_XL_ERR_NOT_FOUND: return "NOT_FOUND";
    case NP_XL_ERR_FULL: return "FULL";
    case NP_XL_ERR_DUP: return "DUP";
    case NP_XL_ERR_TXN_FAILED: return "TXN_FAILED";
    case NP_XL_ERR_IO: return "IO";
    }
    return "?";
}

const char *np_xl_op_name(np_xl_op_t op)
{
    switch (op) {
    case NP_XL_INSTALL: return "安装";
    case NP_XL_UPDATE: return "更新";
    case NP_XL_REMOVE: return "删除";
    }
    return "?";
}

const char *np_xl_state_name(np_xl_state_t s)
{
    switch (s) {
    case NP_XL_ACTIVE: return "ACTIVE";
    case NP_XL_CLEANING: return "CLEANING";
    case NP_XL_REMOVED: return "REMOVED";
    case NP_XL_ROLLED_BACK: return "ROLLED_BACK";
    }
    return "?";
}

void np_xl_init(np_xl_ledger_t *l, const char *source)
{
    if (!l) return;
    memset(l, 0, sizeof(*l));
    fscpy(l->source, sizeof(l->source), source ? source : "extension-ledger");
}

/* 找 active 条目（最新一个未删除的） */
static np_xl_entry_t *find_active(np_xl_ledger_t *l, const char *id)
{
    int i;
    if (!l || !id || !*id) return NULL;
    for (i = l->n - 1; i >= 0; i--) {
        np_xl_entry_t *e = &l->entries[i];
        if (strcmp(e->id, id) == 0 &&
            (e->state == NP_XL_ACTIVE || e->state == NP_XL_ROLLED_BACK))
            return e;
    }
    return NULL;
}

const np_xl_entry_t *np_xl_latest(const np_xl_ledger_t *l, const char *id)
{
    int i;
    if (!l || !id || !*id) return NULL;
    for (i = l->n - 1; i >= 0; i--)
        if (strcmp(l->entries[i].id, id) == 0) return &l->entries[i];
    return NULL;
}

np_xl_status_t np_xl_record(np_xl_ledger_t *l, np_xl_op_t op, uint64_t job_id,
                            const np_xl_entry_t *detail,
                            np_xl_clock_fn clock, void *user)
{
    np_xl_entry_t *e;
    int i;
    if (!l || !detail || !detail->id[0]) return NP_XL_ERR_ARG;
    if (op < NP_XL_INSTALL || op > NP_XL_REMOVE) return NP_XL_ERR_ARG;
    /* 去重：同 id + op + job_id */
    for (i = 0; i < l->n; i++) {
        if (strcmp(l->entries[i].id, detail->id) == 0 &&
            l->entries[i].op == op && l->entries[i].job_id == job_id)
            return NP_XL_ERR_DUP;
    }
    if (l->n >= NP_XL_MAX_ENTRIES) return NP_XL_ERR_FULL;
    e = &l->entries[l->n++];
    *e = *detail;                 /* 深拷贝（全值结构） */
    e->op = op;
    e->job_id = job_id;
    e->state = NP_XL_ACTIVE;
    e->tombstone = false;
    if (clock) clock(e->timestamp, sizeof(e->timestamp), user);
    else fscpy(e->timestamp, sizeof(e->timestamp), "unknown");
    return NP_XL_OK;
}

/* 事务卸载：收集本次实际清理的资源类型（按逆序回滚） */
np_xl_status_t np_xl_uninstall(np_xl_ledger_t *l, const char *id,
                               np_xl_clear_fn clear, np_xl_restore_fn restore,
                               void *user, bool *rolled_back)
{
    np_xl_entry_t *e;
    np_xl_clear_kind_t done[NP_XL_MAX_SNAPSHOT];
    const char *done_val[NP_XL_MAX_SNAPSHOT];
    int ndone = 0, i;
    if (rolled_back) *rolled_back = false;
    if (!l || !id) return NP_XL_ERR_ARG;
    e = find_active(l, id);
    if (!e) return NP_XL_ERR_NOT_FOUND;
    e->state = NP_XL_CLEANING;
    /* 依次清理三类资源（只对非空引用调用） */
#define CLEAR_ONE(kind, field) do { \
        if (e->field[0] && clear) { \
            if (!clear((kind), e->field, user)) goto rollback; \
            if (ndone < NP_XL_MAX_SNAPSHOT) { \
                done[ndone] = (kind); done_val[ndone] = e->field; ndone++; \
            } \
        } \
    } while (0)
    CLEAR_ONE(NP_XL_CLEAR_CREDENTIAL, auth_ref);
    CLEAR_ONE(NP_XL_CLEAR_CACHE, cache_path);
    CLEAR_ONE(NP_XL_CLEAR_TOOLREG, tool_ref);
#undef CLEAR_ONE
    /* 全部成功 → 标记 REMOVED + 仅留墓碑 */
    e->state = NP_XL_REMOVED;
    e->tombstone = true;
    return NP_XL_OK;

rollback:
    /* 逆序恢复已清理的资源 */
    for (i = ndone - 1; i >= 0; i--) {
        if (restore) (void)restore(done[i], done_val[i], user);
    }
    e->state = NP_XL_ROLLED_BACK;
    e->tombstone = false;
    if (rolled_back) *rolled_back = true;
    return NP_XL_ERR_TXN_FAILED;
}

np_xl_status_t np_xl_rollback(np_xl_ledger_t *l, const char *id,
                              np_xl_restore_fn restore, void *user)
{
    np_xl_entry_t *e;
    if (!l || !id || !restore) return NP_XL_ERR_ARG;
    e = (np_xl_entry_t *)np_xl_latest(l, id);
    if (!e) return NP_XL_ERR_NOT_FOUND;
    if (e->state != NP_XL_REMOVED && e->state != NP_XL_ROLLED_BACK)
        return NP_XL_ERR_ARG;
    /* 恢复三类资源（对非空引用） */
    if (e->auth_ref[0])   (void)restore(NP_XL_CLEAR_CREDENTIAL, e->auth_ref, user);
    if (e->cache_path[0]) (void)restore(NP_XL_CLEAR_CACHE, e->cache_path, user);
    if (e->tool_ref[0])   (void)restore(NP_XL_CLEAR_TOOLREG, e->tool_ref, user);
    e->state = NP_XL_ACTIVE;
    e->tombstone = true;   /* 保留可追溯记录 */
    return NP_XL_OK;
}

/* ---------- INI 持久化（只存元数据） ---------- */
static void write_line(FILE *f, const char *key, const char *val)
{
    (void)fprintf(f, "%s=%s\n", key, val ? val : "");
}

np_xl_status_t np_xl_save(const np_xl_ledger_t *l, const char *path)
{
    FILE *f;
    int i, k;
    if (!l || !path) return NP_XL_ERR_ARG;
#if defined(_MSC_VER)
    if (fopen_s(&f, path, "w") != 0) return NP_XL_ERR_IO;
#else
    f = fopen(path, "w");
    if (!f) return NP_XL_ERR_IO;
#endif
    write_line(f, "source", l->source);
    (void)fprintf(f, "count=%d\n", l->n);
    for (i = 0; i < l->n; i++) {
        const np_xl_entry_t *e = &l->entries[i];
        (void)fprintf(f, "\n[entry%d]\n", i);
        (void)fprintf(f, "op=%d\nstate=%d\njob=%llu\n",
                      (int)e->op, (int)e->state, (unsigned long long)e->job_id);
        write_line(f, "id", e->id);
        write_line(f, "source_url", e->source);
        write_line(f, "sha256", e->sha256);
        write_line(f, "license", e->license);
        {
            char perms[NP_XL_MAX_PERMS * NP_XL_MAX_TEXT];
            size_t u = 0;
            for (k = 0; k < e->nperms; k++) {
                size_t ln = strlen(e->perms[k]);
                if (u + ln + 2 >= sizeof(perms)) break;
                if (u) perms[u++] = '|';
                memcpy(perms + u, e->perms[k], ln);
                u += ln;
            }
            perms[u] = '\0';
            write_line(f, "perms", perms);
        }
        write_line(f, "auth_ref", e->auth_ref);
        write_line(f, "cache_path", e->cache_path);
        write_line(f, "tool_ref", e->tool_ref);
        write_line(f, "call_summary", e->call_summary);
        write_line(f, "timestamp", e->timestamp);
        (void)fprintf(f, "tombstone=%d\n", e->tombstone ? 1 : 0);
    }
    fclose(f);
    return NP_XL_OK;
}

/* 简易 INI 读：逐行解析 key=value / [entryN] */
static void parse_perms(const char *s, np_xl_entry_t *e)
{
    const char *p = s;
    while (*p && e->nperms < NP_XL_MAX_PERMS) {
        const char *q = strchr(p, '|');
        size_t n = q ? (size_t)(q - p) : strlen(p);
        if (n >= sizeof(e->perms[0])) n = sizeof(e->perms[0]) - 1;
        memcpy(e->perms[e->nperms], p, n);
        e->perms[e->nperms][n] = '\0';
        e->nperms++;
        if (!q) break;
        p = q + 1;
    }
}

np_xl_status_t np_xl_load(np_xl_ledger_t *l, const char *path)
{
    FILE *f;
    char line[1024];
    np_xl_entry_t cur;
    int have = 0;
    if (!l || !path) return NP_XL_ERR_ARG;
#if defined(_MSC_VER)
    if (fopen_s(&f, path, "r") != 0) return NP_XL_ERR_IO;
#else
    f = fopen(path, "r");
    if (!f) return NP_XL_ERR_IO;
#endif
    np_xl_init(l, "loaded");
    memset(&cur, 0, sizeof(cur));
    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n');
        char *eq;
        if (nl) *nl = '\0';
        if (line[0] == '[') {
            if (have && l->n < NP_XL_MAX_ENTRIES) l->entries[l->n++] = cur;
            memset(&cur, 0, sizeof(cur));
            have = 1;
            continue;
        }
        eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        {
            const char *key = line, *val = eq + 1;
            if (!strcmp(key, "source")) fscpy(l->source, sizeof(l->source), val);
            else if (!strcmp(key, "op")) cur.op = (np_xl_op_t)atoi(val);
            else if (!strcmp(key, "state")) cur.state = (np_xl_state_t)atoi(val);
            else if (!strcmp(key, "job")) cur.job_id = strtoull(val, NULL, 10);
            else if (!strcmp(key, "id")) fscpy(cur.id, sizeof(cur.id), val);
            else if (!strcmp(key, "source_url")) fscpy(cur.source, sizeof(cur.source), val);
            else if (!strcmp(key, "sha256")) fscpy(cur.sha256, sizeof(cur.sha256), val);
            else if (!strcmp(key, "license")) fscpy(cur.license, sizeof(cur.license), val);
            else if (!strcmp(key, "perms")) parse_perms(val, &cur);
            else if (!strcmp(key, "auth_ref")) fscpy(cur.auth_ref, sizeof(cur.auth_ref), val);
            else if (!strcmp(key, "cache_path")) fscpy(cur.cache_path, sizeof(cur.cache_path), val);
            else if (!strcmp(key, "tool_ref")) fscpy(cur.tool_ref, sizeof(cur.tool_ref), val);
            else if (!strcmp(key, "call_summary")) fscpy(cur.call_summary, sizeof(cur.call_summary), val);
            else if (!strcmp(key, "timestamp")) fscpy(cur.timestamp, sizeof(cur.timestamp), val);
            else if (!strcmp(key, "tombstone")) cur.tombstone = atoi(val) != 0;
        }
    }
    if (have && l->n < NP_XL_MAX_ENTRIES) l->entries[l->n++] = cur;
    fclose(f);
    return NP_XL_OK;
}

/* ---------- 渲染 ---------- */
int np_xl_render(const np_xl_ledger_t *l, char *out, size_t cap)
{
    size_t u = 0;
    int i;
    if (!l || !out || cap < 128) return -1;
    if (!appendf(out, cap, &u, "# 扩展生命周期审计报告\n\n")) return -1;
    if (!appendf(out, cap, &u, "- 账本来源：%s\n- 条目数：%d\n\n",
                 l->source, l->n)) return -1;
    for (i = 0; i < l->n; i++) {
        const np_xl_entry_t *e = &l->entries[i];
        int k;
        if (!appendf(out, cap, &u,
            "## %s · %s · Job %llu\n\n"
            "- 状态：%s%s\n- 来源：%s\n- SHA256：%s\n- 许可证：%s\n"
            "- 凭据引用：%s（非明文）\n- 缓存：%s\n- 工具注册：%s\n"
            "- 调用摘要：%s\n- 时间：%s\n",
            np_xl_op_name(e->op), e->id, (unsigned long long)e->job_id,
            np_xl_state_name(e->state), e->tombstone ? " · 墓碑" : "",
            e->source[0] ? e->source : "(未记录)",
            e->sha256[0] ? e->sha256 : "(未记录)",
            e->license[0] ? e->license : "(未记录)",
            e->auth_ref[0] ? e->auth_ref : "(无)",
            e->cache_path[0] ? e->cache_path : "(无)",
            e->tool_ref[0] ? e->tool_ref : "(无)",
            e->call_summary[0] ? e->call_summary : "(无)",
            e->timestamp)) return -1;
        if (e->nperms) {
            if (!appendf(out, cap, &u, "- 权限：")) return -1;
            for (k = 0; k < e->nperms; k++) {
                if (!appendf(out, cap, &u, "%s%s", k ? " | " : "",
                             e->perms[k])) return -1;
            }
            if (!appendf(out, cap, &u, "\n")) return -1;
        }
        if (!appendf(out, cap, &u, "\n")) return -1;
    }
    if (!appendf(out, cap, &u,
        "> 只记录元数据（凭据仅存引用，无明文）；删除事务任一清理失败即回滚，"
        "成功后仅留审计墓碑。回滚范围取决于注入的清理/恢复回调。\n")) return -1;
    return (int)u;
}
