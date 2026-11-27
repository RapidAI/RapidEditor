/* np_sqlquery.c — F6: 结构化查询与采样分析（实现）.
 * 纯 C99、UI-free、只读；自研受限 SELECT 子集 + 采样统计。
 */
#include "np_sqlquery.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "np_sqlite.h"

/* 可移植的大小写不敏感比较（避免 strcasecmp / _stricmp 差异） */
static int ci_cmp(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

/* 可移植的分词（替代 MSVC 的 strtok_s，避免全局 strtok 状态） */
static char *next_tok(char **cursor)
{
    char *start, *p;
    if (!cursor || !*cursor) return NULL;
    p = *cursor;
    while (*p && isspace((unsigned char)*p)) p++;
    if (!*p) { *cursor = p; return NULL; }
    start = p;
    while (*p && !isspace((unsigned char)*p)) p++;
    if (*p) { *p = '\0'; p++; }
    *cursor = p;
    return start;
}

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

const char *np_sq_status_name(np_sq_status_t s)
{
    switch (s) {
    case NP_SQ_OK: return "OK";
    case NP_SQ_ERR_ARG: return "ARG";
    case NP_SQ_ERR_PARSE: return "PARSE";
    case NP_SQ_ERR_DENIED: return "DENIED";
    case NP_SQ_ERR_UNCONFIRMED: return "UNCONFIRMED";
    case NP_SQ_ERR_NOFILE: return "NOFILE";
    case NP_SQ_ERR_NOTSQLITE: return "NOTSQLITE";
    case NP_SQ_ERR_TRUNCATED: return "TRUNCATED";
    }
    return "?";
}

const char *np_sq_op_name(np_sq_op_t op)
{
    switch (op) {
    case NP_SQ_EQ: return "=";
    case NP_SQ_NE: return "!=";
    case NP_SQ_LT: return "<";
    case NP_SQ_LE: return "<=";
    case NP_SQ_GT: return ">";
    case NP_SQ_GE: return ">=";
    }
    return "?";
}

/* 危险写操作关键字（任意位置出现即拒绝） */
static const char *const DENY_WORDS[] = {
    "insert", "update", "delete", "drop", "alter", "create", "replace",
    "truncate", "pragma", "attach", "detach", "vacuum", "reindex",
    "begin", "commit", "rollback", "grant", "revoke", "trigger",
    "into", "values", "set", "exec", "execute", "--", "/*", ";"
};

static bool contains_denied(const char *s)
{
    size_t i;
    char low[NP_SQ_MAX_SQL + 64];
    /* 中文写操作词（UTF-8 字节序列，直接子串匹配，无需大小写转换） */
    static const char *const DENY_CJK[] = {
        "\xe5\x88\xa0\xe9\x99\xa4",       /* 删除 */
        "\xe4\xbf\xae\xe6\x94\xb9",       /* 修改 */
        "\xe6\x8f\x92\xe5\x85\xa5",       /* 插入 */
        "\xe6\x9b\xb4\xe6\x96\xb0",       /* 更新 */
        "\xe6\xb8\x85\xe7\xa9\xba",       /* 清空 */
        "\xe9\x94\x80\xe6\xaf\x81"        /* 销毁 */
    };
    fscpy(low, sizeof(low), s);
    for (i = 0; low[i]; i++) low[i] = (char)tolower((unsigned char)low[i]);
    for (i = 0; i < sizeof(DENY_WORDS) / sizeof(DENY_WORDS[0]); i++) {
        if (strstr(low, DENY_WORDS[i]) != NULL) return true;
    }
    for (i = 0; i < sizeof(DENY_CJK) / sizeof(DENY_CJK[0]); i++) {
        if (strstr(s, DENY_CJK[i]) != NULL) return true;
    }
    return false;
}

/* 大小写不敏感的表查找（SQLite 标识符不区分大小写） */
static const np_db_table_t *find_table_ci(const np_db_schema_t *s, const char *name)
{
    const np_db_table_t *t = np_db_find_table(s, name);
    int i;
    if (t) return t;
    for (i = 0; i < s->ntables; i++) {
        if (ci_cmp(s->tables[i].name, name) == 0) return &s->tables[i];
    }
    return NULL;
}

static bool ident_valid(const char *s)
{
    size_t i;
    if (!s || !*s) return false;
    for (i = 0; s[i]; i++) {
        if (!(isalnum((unsigned char)s[i]) || s[i] == '_')) return false;
    }
    return true;
}

static bool col_in_schema(const np_db_schema_t *sch, const char *table, const char *col)
{
    const np_db_table_t *t = np_db_find_table(sch, table);
    int i;
    if (!t) {
        for (i = 0; i < sch->ntables; i++)
            if (ci_cmp(sch->tables[i].name, table) == 0) { t = &sch->tables[i]; break; }
    }
    if (!t) return false;
    for (i = 0; i < t->ncols; i++)
        if (ci_cmp(t->cols[i].name, col) == 0) return true;
    return false;
}

static bool is_number(const char *s)
{
    const char *p = s;
    if (!p || !*p) return false;
    if (*p == '-' || *p == '+') p++;
    if (!*p) return false;
    for (; *p; p++) {
        if (!isdigit((unsigned char)*p) && *p != '.') return false;
    }
    return true;
}

/* 生成规范化只读 SQL（供确认预览） */
static void build_sql(np_sq_plan_t *p)
{
    char buf[NP_SQ_MAX_SQL];
    size_t u = 0;
    int i;
    u += (size_t)snprintf(buf + u, sizeof(buf) - u, "SELECT ");
    if (p->nsel == 0) {
        u += (size_t)snprintf(buf + u, sizeof(buf) - u, "*");
    } else {
        for (i = 0; i < p->nsel; i++) {
            if (i) u += (size_t)snprintf(buf + u, sizeof(buf) - u, ", ");
            u += (size_t)snprintf(buf + u, sizeof(buf) - u, "%s", p->cols[i]);
        }
    }
    u += (size_t)snprintf(buf + u, sizeof(buf) - u, " FROM %s", p->table);
    if (p->has_where) {
        if (p->where_lit_is_num) {
            u += (size_t)snprintf(buf + u, sizeof(buf) - u, " WHERE %s %s %s",
                                  p->where_col, np_sq_op_name(p->where_op),
                                  p->where_lit);
        } else {
            u += (size_t)snprintf(buf + u, sizeof(buf) - u, " WHERE %s %s '%s'",
                                  p->where_col, np_sq_op_name(p->where_op),
                                  p->where_lit);
        }
    }
    if (p->limit)
        u += (size_t)snprintf(buf + u, sizeof(buf) - u, " LIMIT %lu",
                              (unsigned long)p->limit);
    fscpy(p->sql, sizeof(p->sql), buf);
}

/* ---------- 显式 SQL 解析（白名单子集） ---------- */
np_sq_status_t np_sq_parse_sql(const char *sql, const np_db_schema_t *sch,
                               np_sq_plan_t *out)
{
    char work[NP_SQ_MAX_SQL];
    char *tok, *cursor;
    size_t n;
    int i;
    if (!sql || !sch || !out) return NP_SQ_ERR_ARG;
    memset(out, 0, sizeof(*out));
    fscpy(work, sizeof(work), sql);
    if (contains_denied(sql)) return NP_SQ_ERR_DENIED;
    cursor = work;
    /* 必须以 select 开头 */
    tok = next_tok(&cursor);
    if (!tok || ci_cmp(tok, "select") != 0) return NP_SQ_ERR_PARSE;
    /* 列列表直到 from */
    i = 0;
    while ((tok = next_tok(&cursor)) != NULL) {
        if (ci_cmp(tok, "from") == 0) break;
        /* 去尾部逗号 */
        n = strlen(tok);
        while (n && tok[n - 1] == ',') tok[--n] = '\0';
        if (!*tok) continue;
        if (strcmp(tok, "*") != 0 && !ident_valid(tok)) return NP_SQ_ERR_PARSE;
        if (i < NP_DB_MAX_COLS) fscpy(out->cols[i++], NP_SQ_MAX_IDENT, tok);
    }
    out->nsel = i;
    if (out->nsel == 1 && strcmp(out->cols[0], "*") == 0) out->nsel = 0;
    /* 表名 */
    tok = next_tok(&cursor);
    if (!tok || !ident_valid(tok) || !find_table_ci(sch, tok))
        return NP_SQ_ERR_DENIED;
    fscpy(out->table, sizeof(out->table), tok);
    /* 校验选择的列都在 schema 里 */
    for (i = 0; i < out->nsel; i++) {
        if (!col_in_schema(sch, out->table, out->cols[i])) return NP_SQ_ERR_DENIED;
    }
    /* where / limit 子句（受限） */
    tok = next_tok(&cursor);
    if (tok && ci_cmp(tok, "where") == 0) {
        char *col = next_tok(&cursor);
        char *op = next_tok(&cursor);
        char *lit = next_tok(&cursor);
        if (!col || !op || !lit || !ident_valid(col) ||
            !col_in_schema(sch, out->table, col)) return NP_SQ_ERR_DENIED;
        fscpy(out->where_col, sizeof(out->where_col), col);
        if (strcmp(op, "=") == 0) out->where_op = NP_SQ_EQ;
        else if (strcmp(op, "!=") == 0 || strcmp(op, "<>") == 0) out->where_op = NP_SQ_NE;
        else if (strcmp(op, "<") == 0) out->where_op = NP_SQ_LT;
        else if (strcmp(op, "<=") == 0) out->where_op = NP_SQ_LE;
        else if (strcmp(op, ">") == 0) out->where_op = NP_SQ_GT;
        else if (strcmp(op, ">=") == 0) out->where_op = NP_SQ_GE;
        else return NP_SQ_ERR_PARSE;
        /* 去引号 */
        {
            size_t ln = strlen(lit);
            if (ln >= 2 && ((lit[0] == '\'' && lit[ln - 1] == '\'') ||
                            (lit[0] == '"' && lit[ln - 1] == '"'))) {
                lit[ln - 1] = '\0'; lit++;
            }
        }
        fscpy(out->where_lit, sizeof(out->where_lit), lit);
        out->where_lit_is_num = is_number(lit);
        out->has_where = true;
        tok = next_tok(&cursor);
    }
    if (tok && ci_cmp(tok, "limit") == 0) {
        char *num = next_tok(&cursor);
        if (!num || !is_number(num)) return NP_SQ_ERR_PARSE;
        {
            long lv = strtol(num, NULL, 10);
            if (lv < 0) return NP_SQ_ERR_PARSE;
            out->limit = (size_t)lv;
            if (out->limit > NP_SQ_MAX_ROWS) out->limit = NP_SQ_MAX_ROWS;
        }
    }
    build_sql(out);
    return NP_SQ_OK;
}

/* ---------- 自然语言解析（简单中文/英文模板） ---------- */
np_sq_status_t np_sq_parse_nl(const char *text, const np_db_schema_t *sch,
                              np_sq_plan_t *out)
{
    char low[NP_SQ_MAX_SQL + 64];
    size_t i;
    if (!text || !sch || !out) return NP_SQ_ERR_ARG;
    if (contains_denied(text)) return NP_SQ_ERR_DENIED;
    fscpy(low, sizeof(low), text);
    for (i = 0; low[i]; i++) low[i] = (char)tolower((unsigned char)low[i]);
    /* 找表名（schema 中的任一表名出现在文本里） */
    for (i = 0; i < (size_t)sch->ntables; i++) {
        const char *tbl = sch->tables[i].name;
        char needle[NP_SQ_MAX_IDENT + 8];
        char cols_buf[NP_SQ_MAX_SQL];
        size_t u = 0;
        const np_db_table_t *t = &sch->tables[i];
        int c;
        if (!*tbl) continue;
        (void)snprintf(needle, sizeof(needle), "%s", tbl);
        if (strstr(low, needle) == NULL) continue;
        /* 构造 SELECT：检测 "name" / 列名关键词 */
        u += (size_t)snprintf(cols_buf + u, sizeof(cols_buf) - u, "SELECT ");
        {
            bool any = false;
            for (c = 0; c < t->ncols; c++) {
                char cneedle[NP_SQ_MAX_IDENT + 8];
                (void)snprintf(cneedle, sizeof(cneedle), "%s", t->cols[c].name);
                if (strstr(low, cneedle) != NULL) {
                    if (any) u += (size_t)snprintf(cols_buf + u, sizeof(cols_buf) - u, ", ");
                    u += (size_t)snprintf(cols_buf + u, sizeof(cols_buf) - u,
                                          "%s", t->cols[c].name);
                    any = true;
                }
            }
            if (!any) u += (size_t)snprintf(cols_buf + u, sizeof(cols_buf) - u, "*");
        }
        (void)snprintf(cols_buf + u, sizeof(cols_buf) - u, " FROM %s LIMIT %d",
                       tbl, NP_SQ_MAX_ROWS);
        return np_sq_parse_sql(cols_buf, sch, out);
    }
    return NP_SQ_ERR_PARSE;
}

int np_sq_preview(const np_sq_plan_t *plan, char *out, size_t cap)
{
    size_t u = 0;
    if (!plan || !out || cap < 64) return -1;
    if (!appendf(out, cap, &u, "只读查询计划（需确认后执行）：\n\n")) return -1;
    if (!appendf(out, cap, &u, "```sql\n%s\n```\n", plan->sql)) return -1;
    if (!appendf(out, cap, &u,
        "\n- 目标表：%s\n- 选择列：%s\n", plan->table,
        plan->nsel == 0 ? "全部（*）" : "指定列")) return -1;
    if (plan->nsel) {
        int i;
        for (i = 0; i < plan->nsel; i++) {
            if (!appendf(out, cap, &u, "  - %s\n", plan->cols[i])) return -1;
        }
    }
    if (plan->has_where && !appendf(out, cap, &u, "- 条件：%s %s %s\n",
        plan->where_col, np_sq_op_name(plan->where_op), plan->where_lit)) return -1;
    if (!appendf(out, cap, &u, "- 采样上限：%lu 行\n",
                 (unsigned long)(plan->limit ? plan->limit : NP_SQ_MAX_ROWS))) return -1;
    if (!appendf(out, cap, &u,
        "\n安全：仅 SELECT 白名单子集，禁止一切写操作与未声明标识符；"
        "确认后由调用方显式执行。\n")) return -1;
    return (int)u;
}

/* ---------- where 条件求值 ---------- */
static bool eval_where(const np_sq_plan_t *p, const np_db_row_t *row,
                       const np_db_table_t *t)
{
    int idx = -1, i;
    const np_db_cell_t *cell;
    double a, b;
    int cmp;
    if (!p->has_where) return true;
    for (i = 0; i < t->ncols; i++) {
        if (strcmp(t->cols[i].name, p->where_col) == 0) { idx = i; break; }
    }
    if (idx < 0 || idx >= row->ncells) return false;
    cell = &row->cells[idx];
    if (cell->is_null) return false;
    if (p->where_lit_is_num && cell->is_int) {
        a = (double)cell->ival;
        b = atof(p->where_lit);
        cmp = (a < b) ? -1 : (a > b) ? 1 : 0;
    } else {
        cmp = strcmp(cell->text, p->where_lit);
    }
    switch (p->where_op) {
    case NP_SQ_EQ: return cmp == 0;
    case NP_SQ_NE: return cmp != 0;
    case NP_SQ_LT: return cmp < 0;
    case NP_SQ_LE: return cmp <= 0;
    case NP_SQ_GT: return cmp > 0;
    case NP_SQ_GE: return cmp >= 0;
    }
    return false;
}

/* ---------- 采样回调 ---------- */
typedef struct {
    const np_sq_plan_t *plan;
    np_sq_result_t *res;
    const np_db_table_t *table;
    size_t sel_idx[NP_DB_MAX_COLS];
    int nsel_idx;
    size_t max_rows;
    bool reached_limit;
} run_ctx_t;

typedef struct {
    run_ctx_t *ctx;
} sample_cb_user_t;

static bool collect_row(const np_db_row_t *row, void *user)
{
    sample_cb_user_t *su = (sample_cb_user_t *)user;
    run_ctx_t *ctx = su->ctx;
    np_sq_result_t *res = ctx->res;
    const np_sq_plan_t *p = ctx->plan;
    size_t r = res->rows_returned;
    int i;
    res->rows_scanned++;
    if (!eval_where(p, row, ctx->table)) return true;
    if (r >= ctx->max_rows) { ctx->reached_limit = true; return false; }
    for (i = 0; i < ctx->nsel_idx; i++) {
        int ci = ctx->sel_idx[i];
        const np_db_cell_t *cell = (ci < row->ncells) ? &row->cells[ci] : NULL;
        /* 统计 */
        if (!cell || cell->is_null) {
            res->stats[i].nulls++;
            fscpy(res->rows[r][i], NP_SQ_MAX_LITERAL, "NULL");
            continue;
        }
        res->stats[i].nonnull++;
        fscpy(res->rows[r][i], NP_SQ_MAX_LITERAL, cell->text);
        /* 数值统计 */
        if (cell->is_int) {
            double v = (double)cell->ival;
            if (res->stats[i].num_count == 0) {
                res->stats[i].num_min = v; res->stats[i].num_max = v;
            } else {
                if (v < res->stats[i].num_min) res->stats[i].num_min = v;
                if (v > res->stats[i].num_max) res->stats[i].num_max = v;
            }
            res->stats[i].num_sum += v;
            res->stats[i].num_count++;
        }
        /* top 值 */
        {
            size_t k;
            bool found = false;
            for (k = 0; k < NP_SQ_MAX_BUCKETS; k++) { (void)k; }
            if (strcmp(res->stats[i].top, cell->text) == 0) {
                res->stats[i].top_count++;
                found = true;
            }
            if (!found && res->stats[i].top_count == 0) {
                fscpy(res->stats[i].top, sizeof(res->stats[i].top), cell->text);
                res->stats[i].top_count = 1;
            }
        }
    }
    res->rows_returned++;
    return true;
}

np_sq_status_t np_sq_run(const char *path, const np_sq_plan_t *plan,
                         size_t max_rows, np_sq_result_t *out)
{
    FILE *f;
    long sz;
    unsigned char *data;
    size_t got, len;
    np_db_schema_t schema;
    const np_db_table_t *t;
    run_ctx_t ctx;
    sample_cb_user_t su;
    bool partial = false;
    size_t i;
    if (!path || !plan || !out) return NP_SQ_ERR_ARG;
    if (!plan->confirmed) return NP_SQ_ERR_UNCONFIRMED;
    memset(out, 0, sizeof(*out));
    if (max_rows == 0 || max_rows > NP_SQ_MAX_ROWS) max_rows = NP_SQ_MAX_ROWS;
    f = fopen(path, "rb");
    if (!f) return NP_SQ_ERR_NOFILE;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NP_SQ_ERR_NOFILE; }
    sz = ftell(f);
    if (sz < 16 || sz > 64 * 1024 * 1024) { fclose(f); return NP_SQ_ERR_NOTSQLITE; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NP_SQ_ERR_NOFILE; }
    data = (unsigned char *)malloc((size_t)sz + 1);
    if (!data) { fclose(f); return NP_SQ_ERR_ARG; }
    got = fread(data, 1, (size_t)sz, f);
    fclose(f);
    data[got] = '\0';
    len = got;
    if (np_db_parse(data, len, &schema) != NP_DB_OK) {
        free(data); return NP_SQ_ERR_NOTSQLITE;
    }
    t = find_table_ci(&schema, plan->table);
    if (!t) { free(data); return NP_SQ_ERR_DENIED; }
    memset(&ctx, 0, sizeof(ctx));
    ctx.plan = plan; ctx.res = out; ctx.table = t;
    /* 选择列 → 表列下标映射 */
    if (plan->nsel == 0) {
        for (i = 0; i < (size_t)t->ncols && i < NP_DB_MAX_COLS; i++) {
            ctx.sel_idx[ctx.nsel_idx++] = (int)i;
            fscpy(out->cols[i], sizeof(out->cols[i]), t->cols[i].name);
        }
    } else {
        int c;
        for (i = 0; i < (size_t)plan->nsel; i++) {
            for (c = 0; c < t->ncols; c++) {
                if (strcmp(t->cols[c].name, plan->cols[i]) == 0) {
                    ctx.sel_idx[ctx.nsel_idx++] = c;
                    fscpy(out->cols[i], sizeof(out->cols[i]), t->cols[c].name);
                    break;
                }
            }
        }
    }
    out->ncols = ctx.nsel_idx;
    ctx.max_rows = max_rows;
    ctx.reached_limit = false;
    su.ctx = &ctx;
    /* 多取 1 行用于判定是否触达采样上限（collect_row 在第 max_rows+1 行
     * 检测到越界并标记 sampled），但只回调给调用方 max_rows 行。 */
    (void)np_db_sample_rows(data, len, &schema, plan->table, max_rows + 1,
                            collect_row, &su, &partial);
    out->sampled = ctx.reached_limit;
    out->partial = partial;
    free(data);
    return NP_SQ_OK;
}

int np_sq_render(const np_sq_plan_t *plan, const np_sq_result_t *res,
                 const char *source, char *out, size_t cap)
{
    size_t u = 0;
    size_t r, c;
    if (!plan || !res || !out || cap < 128) return -1;
    if (!appendf(out, cap, &u, "# 结构化查询结果 · %s\n\n",
                 source ? source : "(unknown)")) return -1;
    if (!appendf(out, cap, &u, "```sql\n%s\n```\n\n", plan->sql)) return -1;
    if (!appendf(out, cap, &u,
        "- 返回 %lu 行 / 扫描 %lu 行%s%s\n\n",
        (unsigned long)res->rows_returned, (unsigned long)res->rows_scanned,
        res->sampled ? " · 已采样截断" : "",
        res->partial ? " · B-tree 遍历不完整" : "")) return -1;
    /* 表格头 */
    if (!appendf(out, cap, &u, "|")) return -1;
    for (c = 0; c < (size_t)res->ncols; c++)
        if (!appendf(out, cap, &u, " %s |", res->cols[c])) return -1;
    if (!appendf(out, cap, &u, "\n|")) return -1;
    for (c = 0; c < (size_t)res->ncols; c++)
        if (!appendf(out, cap, &u, "--- |")) return -1;
    if (!appendf(out, cap, &u, "\n")) return -1;
    for (r = 0; r < res->rows_returned; r++) {
        if (!appendf(out, cap, &u, "|")) return -1;
        for (c = 0; c < (size_t)res->ncols; c++) {
            char cell[NP_SQ_MAX_LITERAL];
            size_t k, o = 0;
            for (k = 0; res->rows[r][c][k] && o + 1 < sizeof(cell); k++) {
                cell[o++] = (res->rows[r][c][k] == '|' || res->rows[r][c][k] == '\n')
                            ? ' ' : res->rows[r][c][k];
            }
            cell[o] = '\0';
            if (!appendf(out, cap, &u, " %s |", cell)) return -1;
        }
        if (!appendf(out, cap, &u, "\n")) return -1;
    }
    return (int)u;
}

int np_sq_quality(const np_sq_result_t *res, char *out, size_t cap)
{
    size_t u = 0;
    int i;
    if (!res || !out || cap < 64) return -1;
    if (!appendf(out, cap, &u, "采样数据质量（基于 %lu 行）：\n",
                 (unsigned long)res->rows_scanned)) return -1;
    for (i = 0; i < res->ncols; i++) {
        const char *col = res->cols[i];
        size_t total = res->stats[i].nulls + res->stats[i].nonnull;
        double rate = total ? (double)res->stats[i].nulls * 100.0 / (double)total : 0.0;
        if (!appendf(out, cap, &u, "- %s：空值 %.1f%% (%lu/%lu)",
                     col, rate, (unsigned long)res->stats[i].nulls,
                     (unsigned long)total)) return -1;
        if (res->stats[i].num_count > 0 &&
            !appendf(out, cap, &u, " · 数值范围 [%.2f, %.2f] 均值 %.2f",
                     res->stats[i].num_min, res->stats[i].num_max,
                     res->stats[i].num_sum / (double)res->stats[i].num_count)) return -1;
        if (res->stats[i].top_count > 0 &&
            !appendf(out, cap, &u, " · 最常见 '%s'×%lu",
                     res->stats[i].top, (unsigned long)res->stats[i].top_count)) return -1;
        if (!appendf(out, cap, &u, "\n")) return -1;
    }
    if (res->sampled && !appendf(out, cap, &u,
        "\n注：结果受采样上限截断，空值率仅为采样估计。\n")) return -1;
    return (int)u;
}
