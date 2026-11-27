/* np_sqlite.c — M1/F3: SQLite schema 只读提取（B-tree + sqlite_master）. */
#include "np_sqlite.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* 小工具                                                               */
/* ------------------------------------------------------------------ */

const char *np_db_status_name(np_db_status_t s) {
    switch (s) {
    case NP_DB_OK: return "ok";
    case NP_DB_ERR_ARG: return "arg";
    case NP_DB_ERR_NOT_SQLITE: return "not-sqlite";
    case NP_DB_ERR_TOO_SMALL: return "too-small";
    default: return "corrupt";
    }
}

static uint16_t rd16be(const unsigned char *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}
static uint32_t rd32be(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | p[3];
}

/* SQLite 变长整数：最多 9 字节，前 8 字节各 7 位，第 9 字节 8 位 */
static uint64_t get_varint(const unsigned char *p, size_t avail, size_t *used) {
    uint64_t v = 0;
    size_t i = 0;
    for (; i < 8 && i < avail; ++i) {
        unsigned char b = p[i];
        v = (v << 7) | (uint64_t)(b & 0x7F);
        if (!(b & 0x80)) { i++; *used = i; return v; }
    }
    if (i < avail) { v = (v << 8) | p[i]; i++; }
    *used = i;
    return v;
}

static bool page_offset(size_t len, uint16_t page_size,
                        uint32_t pgno, size_t *off) {
    if (pgno == 0) return false;
    uint64_t o = (uint64_t)(pgno - 1) * page_size;
    if (o + page_size > len) return false;
    *off = (size_t)o;
    return true;
}

/* ------------------------------------------------------------------ */
/* payload 读取（含溢出页链）                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    unsigned char *data;   /* 组合后的完整 payload（调用方 free） */
    size_t len;
} np_db_payload_t;

/* 表叶子页 cell 的本地 payload 计算（SQLite 文件格式 1.x） */
static size_t table_leaf_local(size_t usable, uint64_t payload_len) {
    size_t max_local = usable - 35;
    if (payload_len <= (uint64_t)max_local) return (size_t)payload_len;
    size_t min_local = ((usable - 12) * 32 / 255) - 23;
    size_t k = min_local + ((size_t)payload_len - min_local) % (usable - 4);
    return k <= max_local ? k : min_local;
}

/* 读 cell payload：local 部分 + 溢出链。成功返回 true。 */
static bool read_payload(const unsigned char *d, size_t len, uint16_t page_size,
                         uint8_t reserved, size_t cell_payload_off,
                         uint64_t payload_len, size_t local_len,
                         np_db_payload_t *out) {
    size_t usable = page_size - reserved;
    if (payload_len > 16u * 1024 * 1024) return false; /* 单条 schema 记录上限 16MB */
    out->data = (unsigned char *)malloc((size_t)payload_len + 1);
    if (!out->data) return false;
    out->len = (size_t)payload_len;

    size_t copied = 0;
    if (local_len > payload_len) local_len = (size_t)payload_len;
    if (cell_payload_off + local_len > len) { free(out->data); out->data = NULL; return false; }
    memcpy(out->data, d + cell_payload_off, local_len);
    copied = local_len;

    uint32_t ovf_pg = 0;
    if (copied < out->len) {
        size_t ovf_off = cell_payload_off + local_len;
        if (ovf_off + 4 > len) { free(out->data); out->data = NULL; return false; }
        ovf_pg = rd32be(d + ovf_off);
    }
    int chain = 0;
    while (copied < out->len && ovf_pg != 0 && chain++ < 4096) {
        size_t po;
        if (!page_offset(len, page_size, ovf_pg, &po)) break;
        uint32_t next = rd32be(d + po);
        size_t seg = usable - 4;
        if (seg > out->len - copied) seg = out->len - copied;
        if (po + 4 + seg > len) break;
        memcpy(out->data + copied, d + po + 4, seg);
        copied += seg;
        ovf_pg = next;
    }
    out->data[out->len] = '\0';
    if (copied < out->len) { /* 链断 */
        free(out->data);
        out->data = NULL;
        return false;
    }
    return true;
}

static void payload_free(np_db_payload_t *p) {
    free(p->data);
    p->data = NULL;
    p->len = 0;
}

/* ------------------------------------------------------------------ */
/* 记录（record）解析                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t serial;
    const unsigned char *ptr;  /* 指向 payload 内部 */
    size_t len;
    int64_t ival;
    double  fval;              /* serial 7 的浮点值 */
} np_db_value_t;

/* 解析 record：values 输出指针指向 rec 内部，不拥有内存。
 * 支持表叶子常用 serial type：0,1-6,7,8,9,>=12。 */
static bool parse_record(const unsigned char *rec, size_t rec_len,
                         np_db_value_t *vals, size_t max_vals, size_t *n_vals) {
    size_t used = 0;
    uint64_t hdr_size = get_varint(rec, rec_len, &used);
    if (hdr_size < used || hdr_size > rec_len) return false;
    size_t hdr_off = used;
    size_t body_off = (size_t)hdr_size;
    size_t n = 0;
    while (hdr_off < hdr_size && n < max_vals) {
        uint64_t st = get_varint(rec + hdr_off, hdr_size - hdr_off, &used);
        hdr_off += used;
        np_db_value_t *v = &vals[n++];
        v->serial = st;
        v->ptr = NULL; v->len = 0; v->ival = 0; v->fval = 0.0;
        switch (st) {
        case 0: break;                          /* NULL */
        case 1: case 2: case 3: case 4: case 5: case 6: {
            static const int widths[7] = {0,1,2,3,4,6,8};
            size_t w = (size_t)widths[st];
            if (body_off + w > rec_len) return false;
            int64_t iv = (rec[body_off] & 0x80) ? -1 : 0;
            for (size_t i = 0; i < w; ++i) iv = (iv << 8) | rec[body_off + i];
            v->ival = iv;
            body_off += w;
            break;
        }
        case 7: { /* float：8 字节 IEEE-754 big-endian */
            if (body_off + 8 > rec_len) return false;
            uint64_t bits = 0;
            for (size_t i = 0; i < 8; ++i) bits = (bits << 8) | rec[body_off + i];
            /* bits 是按大端读入的数值；在 little-endian 主机上其内存表示
             * 恰好与 IEEE-754 double 一致（项目全平台均小端）。 */
            memcpy(&v->fval, &bits, 8);
            body_off += 8;
            break;
        }
        case 8: v->ival = 0; break;
        case 9: v->ival = 1; break;
        case 10: case 11: return false;         /* 保留值 */
        default: {
            uint64_t l = (st - 12) / 2;
            if (body_off + l > rec_len) return false;
            v->ptr = rec + body_off;
            v->len = (size_t)l;
            body_off += l;
            break;
        }
        }
    }
    *n_vals = n;
    return hdr_off == hdr_size;
}

/* ------------------------------------------------------------------ */
/* CREATE TABLE 列解析（启发式，UTF-8）                                 */
/* ------------------------------------------------------------------ */

static void trim_ws(const char **s, size_t *n) {
    while (*n && (**s == ' ' || **s == '\t' || **s == '\r' || **s == '\n')) { (*s)++; (*n)--; }
    while (*n && ((*s)[*n-1] == ' ' || (*s)[*n-1] == '\t' || (*s)[*n-1] == '\r' || (*s)[*n-1] == '\n')) (*n)--;
}

static void copy_ident(char *out, size_t cap, const char *s, size_t n) {
    /* 去引号：`x` "x" [x] 'x' 或裸标识符 */
    if (n >= 2 && ((s[0] == '"' && s[n-1] == '"') || (s[0] == '`' && s[n-1] == '`') ||
                   (s[0] == '\'' && s[n-1] == '\'') || (s[0] == '[' && s[n-1] == ']'))) {
        s++; n -= 2;
    }
    if (n >= cap) n = cap - 1;
    memcpy(out, s, n);
    out[n] = '\0';
}

static bool token_eq_ci(const char *s, size_t n, const char *kw) {
    if (strlen(kw) != n) return false;
    for (size_t i = 0; i < n; ++i) {
        char a = s[i], b = kw[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (a != b) return false;
    }
    return true;
}

/* 找匹配的 ')'，处理引号。返回列内偏移或 (size_t)-1。 */
static size_t find_close_paren(const char *s, size_t n, size_t open) {
    int depth = 1;
    for (size_t i = open + 1; i < n; ++i) {
        char c = s[i];
        if (c == '\'' || c == '"' || c == '`') {
            char q = c;
            i++;
            while (i < n && s[i] != q) i++;
            continue;
        }
        if (c == '[') { while (i < n && s[i] != ']') i++; continue; }
        if (c == '(') depth++;
        else if (c == ')') { depth--; if (depth == 0) return i; }
    }
    return (size_t)-1;
}

/* 表级子句判定：PRIMARY KEY / FOREIGN KEY 需双词；UNIQUE/CHECK/CONSTRAINT
 * 单词即可（它们是 SQLite 关键字，不能作列名；而 KEY 可以）。 */
static bool is_table_def_keyword(const char *s, size_t n, const char *rest, size_t rest_len) {
    if (token_eq_ci(s, n, "PRIMARY") || token_eq_ci(s, n, "FOREIGN")) {
        /* 下一个词须是 KEY */
        size_t i = 0;
        while (i < rest_len && (rest[i] == ' ' || rest[i] == '\t')) i++;
        return token_eq_ci(rest + i, (rest_len - i > 3) ? 3 : rest_len - i, "KEY");
    }
    return token_eq_ci(s, n, "UNIQUE") || token_eq_ci(s, n, "CHECK") ||
           token_eq_ci(s, n, "CONSTRAINT");
}

/* 解析 CREATE TABLE 列清单。成功填充 t->cols 并返回列数。 */
static int parse_create_columns(const char *sql, np_db_table_t *t) {
    const char *s = sql;
    size_t n = strlen(sql);
    size_t open = 0;
    for (; open < n && s[open] != '('; ++open) {}
    if (open >= n) return 0;
    size_t close = find_close_paren(s, n, open);
    if (close == (size_t)-1) { t->truncated = true; return 0; }

    int count = 0;
    size_t i = open + 1;
    int pk_col = -1;
    bool rowid_alias = false;
    while (i < close) {
        /* 取一个顶层逗号分隔的定义 */
        size_t start = i;
        int depth = 0;
        bool in_q = false; char q = 0;
        while (i < close) {
            char c = s[i];
            if (in_q) { if (c == q) in_q = false; }
            else if (c == '\'' || c == '"' || c == '`') { in_q = true; q = c; }
            else if (c == '[') { while (i < close && s[i] != ']') i++; }
            else if (c == '(') depth++;
            else if (c == ')') depth--;
            else if (c == ',' && depth == 0) break;
            i++;
        }
        size_t dlen = i - start;
        i++; /* 跳过逗号 */
        const char *d = s + start;
        trim_ws(&d, &dlen);
        if (!dlen) continue;

        /* 第一个 token */
        size_t tok1 = 0;
        {
            char c0 = d[0];
            if (c0 == '"' || c0 == '`' || c0 == '\'') {
                tok1 = 1;
                while (tok1 < dlen && d[tok1] != c0) tok1++;
                if (tok1 < dlen) tok1++; /* 含闭引号 */
            } else if (c0 == '[') {
                tok1 = 1;
                while (tok1 < dlen && d[tok1] != ']') tok1++;
                if (tok1 < dlen) tok1++;
            } else {
                while (tok1 < dlen && d[tok1] != ' ' && d[tok1] != '\t') tok1++;
            }
        }
        size_t rest = dlen - tok1;
        const char *r = d + tok1;
        trim_ws(&r, &rest);

        /* 表级子句跳过 */
        if (!(d[0] == '"' || d[0] == '`' || d[0] == '\'' || d[0] == '[') &&
            is_table_def_keyword(d, tok1, r, rest))
            continue;

        if (count >= NP_DB_MAX_COLS) { t->truncated = true; break; }
        np_db_col_t *c = &t->cols[count];
        memset(c, 0, sizeof(*c));
        copy_ident(c->name, sizeof(c->name), d, tok1);

        /* 第二个 token：声明类型（可选） */
        size_t tok2 = 0;
        while (tok2 < rest && (r[tok2] == ' ' || r[tok2] == '\t')) tok2++;
        size_t tstart = tok2;
        while (tok2 < rest && r[tok2] != ' ' && r[tok2] != '\t') tok2++;
        if (tok2 > tstart && rest - tstart >= tok2 - tstart)
            copy_ident(c->decl_type, sizeof(c->decl_type), r + tstart, tok2 - tstart);

    /* 约束扫描（当前定义片段内） */
    for (size_t k = 0; k < rest; ++k) {
        if (k + 8 <= rest && token_eq_ci(r + k, 8, "NOT NULL")) c->notnull = true;
        if (k + 11 <= rest && token_eq_ci(r + k, 11, "PRIMARY KEY")) { c->pk = true; pk_col = count; }
    }
        count++;
    }
    t->ncols = count;
    if (pk_col >= 0) {
        np_db_col_t *c = &t->cols[pk_col];
        if (token_eq_ci(c->decl_type, strlen(c->decl_type), "INTEGER")) {
            t->rowid_alias = true;
        }
    }
    (void)rowid_alias;
    return count;
}

/* ------------------------------------------------------------------ */
/* b-tree 遍历                                                          */
/* ------------------------------------------------------------------ */

/* rowid 随行传给回调：INTEGER PRIMARY KEY 别名列在 record 中存 NULL，
 * 真实值即 rowid（SQLite 文件格式约定）。 */
typedef void (*row_cb)(const np_db_value_t *vals, size_t n, uint64_t rowid,
                       void *user);

typedef struct {
    const unsigned char *d;
    size_t len;
    uint16_t page_size;
    uint8_t reserved;
    bool partial;
} walk_ctx_t;

static bool walk_table_btree(walk_ctx_t *w, uint32_t root, row_cb cb, void *user,
                             uint64_t *cells_out);

#define WALK_MAX_PAGES 4096

static bool visit_page(walk_ctx_t *w, uint32_t pgno, row_cb cb, void *user,
                       uint64_t *cells, int depth) {
    if (depth > 20 || *cells > WALK_MAX_PAGES * 16) { w->partial = true; return false; }
    size_t po;
    if (!page_offset(w->len, w->page_size, pgno, &po)) { w->partial = true; return false; }
    const unsigned char *pg = w->d + po;
    size_t hdr = (pgno == 1) ? 100 : 0;
    unsigned char type = pg[hdr];
    uint16_t ncells = rd16be(pg + hdr + 3);
    size_t usable = w->page_size - w->reserved;

    if (type == 5) { /* 内部表页：4B child + varint key */
        size_t cellptr = hdr + 12;
        for (uint16_t i = 0; i < ncells && !w->partial; ++i) {
            if (cellptr + 2 > w->page_size) { w->partial = true; break; }
            uint16_t coff = rd16be(pg + cellptr + i * 2);
            if (coff + 4 > w->page_size) { w->partial = true; break; }
            uint32_t child = rd32be(pg + coff);
            if (child) visit_page(w, child, cb, user, cells, depth + 1);
        }
        uint32_t right = rd32be(pg + hdr + 8);
        if (right && !w->partial) visit_page(w, right, cb, user, cells, depth + 1);
        return !w->partial;
    }
    if (type != 13) { /* 叶子表页；其他类型（索引页/坏页）→ 部分解析 */
        w->partial = true;
        return false;
    }
    size_t cellptr = hdr + 8;
    for (uint16_t i = 0; i < ncells; ++i) {
        if (cellptr + 2 > w->page_size) { w->partial = true; break; }
        uint16_t coff = rd16be(pg + cellptr + i * 2);
        if (coff + 4 > w->page_size) { w->partial = true; break; }
        size_t used = 0;
        uint64_t payload_len = get_varint(pg + coff, usable - coff, &used);
        size_t used2 = 0;
        uint64_t rowid = get_varint(pg + coff + used, usable - coff - used,
                                    &used2); /* rowid */
        size_t poff = coff + used + used2;
        if (poff > usable) { w->partial = true; break; }
        size_t local = table_leaf_local(usable, payload_len);
        np_db_payload_t pl;
        if (!read_payload(w->d, w->len, w->page_size, w->reserved,
                          po + poff, payload_len, local, &pl)) {
            w->partial = true;
            break;
        }
        np_db_value_t vals[8];
        size_t nv = 0;
        if (parse_record(pl.data, pl.len, vals, 8, &nv) && cb)
            cb(vals, nv, rowid, user);
        payload_free(&pl);
        (*cells)++;
    }
    return !w->partial;
}

static bool walk_table_btree(walk_ctx_t *w, uint32_t root, row_cb cb, void *user,
                             uint64_t *cells_out) {
    uint64_t cells = 0;
    bool ok = visit_page(w, root, cb, user, &cells, 0);
    if (cells_out) *cells_out = cells;
    return ok;
}

/* ------------------------------------------------------------------ */
/* sqlite_master 收集                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    np_db_schema_t *out;
    size_t max_hdr;              /* record 数上限守卫 */
} master_user_t;

/* 从 value 中取文本（UTF-8 假定）；len 截断到 cap-1 */
static void val_text(const np_db_value_t *v, char *out, size_t cap) {
    if (!out || !cap) return;
    out[0] = '\0';
    if (!v || !v->ptr) return;
    size_t n = v->len < cap - 1 ? v->len : cap - 1;
    memcpy(out, v->ptr, n);
    out[n] = '\0';
}

static bool be_utf8_ci(const char *s, size_t n, const char *kw) {
    return token_eq_ci(s, n, kw);
}

static void master_row_cb(const np_db_value_t *vals, size_t n, uint64_t rowid,
                          void *user) {
    (void)rowid;
    master_user_t *mu = (master_user_t *)user;
    np_db_schema_t *out = mu->out;
    if (n < 5) return;

    char type[12], name[NP_DB_MAX_NAME], tbl[NP_DB_MAX_NAME];
    char sql[NP_DB_MAX_SQL];
    val_text(&vals[0], type, sizeof(type));
    val_text(&vals[1], name, sizeof(name));
    val_text(&vals[2], tbl, sizeof(tbl));
    val_text(&vals[4], sql, sizeof(sql));
    uint32_t rootpage = (uint32_t)vals[3].ival;
    if (!name[0]) return;

    if (be_utf8_ci(type, strlen(type), "table")) {
        if (out->ntables >= NP_DB_MAX_TABLES) { out->partially_parsed = true; return; }
        np_db_table_t *t = &out->tables[out->ntables++];
        memset(t, 0, sizeof(*t));
        snprintf(t->name, sizeof(t->name), "%s", name);
        t->rootpage = rootpage;
        snprintf(t->sql, sizeof(t->sql), "%s", sql);
        /* sql 截断检测：源文本长于缓冲 */
        if (vals[4].ptr && vals[4].len >= sizeof(t->sql)) t->truncated = true;
        if (sql[0] && !t->truncated) {
            parse_create_columns(sql, t);
        }
    } else { /* index / view / trigger */
        if (out->nobjects >= NP_DB_MAX_OBJECTS) { out->partially_parsed = true; return; }
        np_db_object_t *o = &out->objects[out->nobjects++];
        memset(o, 0, sizeof(*o));
        snprintf(o->type, sizeof(o->type), "%s", type);
        snprintf(o->name, sizeof(o->name), "%s", name);
        snprintf(o->tbl, sizeof(o->tbl), "%s", tbl);
        o->rootpage = rootpage;
        snprintf(o->sql, sizeof(o->sql), "%s", sql);
    }
}

/* ------------------------------------------------------------------ */
/* 主入口                                                               */
/* ------------------------------------------------------------------ */

np_db_status_t np_db_parse(const unsigned char *d, size_t len,
                           np_db_schema_t *out) {
    if (!out) return NP_DB_ERR_ARG;
    memset(out, 0, sizeof(*out));
    if (!d) return NP_DB_ERR_ARG;
    if (len < 16) return NP_DB_ERR_TOO_SMALL;
    if (memcmp(d, "SQLite format 3\0", 16) != 0)
        return NP_DB_ERR_NOT_SQLITE;
    if (len < 100) return NP_DB_ERR_CORRUPT;

    /* 页大小：头中值 1 表示 65536 */
    uint32_t ps = rd16be(d + 16);
    if (ps == 1) ps = 65536;
    if (ps < 512 || ps > 65536 || (ps & (ps - 1)) != 0)
        return NP_DB_ERR_CORRUPT;

    uint8_t reserved = d[20];
    if (ps - reserved < 480) return NP_DB_ERR_CORRUPT; /* usable 太小 */

    out->valid      = true;
    out->page_size  = ps;
    out->wal        = (d[18] == 2 || d[19] == 2);
    out->n_pages    = rd32be(d + 28);
    out->freelist_pages = rd32be(d + 36);
    uint32_t enc = rd32be(d + 56);
    out->text_encoding = (enc >= 1 && enc <= 3) ? (uint8_t)enc : 1;

    walk_ctx_t w = { d, len, ps, reserved, false };
    master_user_t mu = { out, 0 };
    uint64_t cells = 0;
    walk_table_btree(&w, 1, master_row_cb, &mu, &cells);
    out->partially_parsed = out->partially_parsed || w.partial;
    if (out->text_encoding != 1) out->partially_parsed = true; /* 列解析仅 UTF-8 */

    /* 行数：对每张表根页独立遍历计 cell */
    for (int i = 0; i < out->ntables; ++i) {
        walk_ctx_t w2 = { d, len, ps, reserved, false };
        uint64_t c2 = 0;
        walk_table_btree(&w2, out->tables[i].rootpage, NULL, NULL, &c2);
        out->tables[i].est_rows = c2;
        if (w2.partial) {
            out->partially_parsed = true;
            /* 该表 rootpage 无效（如视图/vtab rootpage=0），est_rows 置 0 */
            if (out->tables[i].rootpage == 0) out->tables[i].est_rows = 0;
        }
    }
    return NP_DB_OK;
}

const np_db_table_t *np_db_find_table(const np_db_schema_t *s, const char *name) {
    if (!s || !name) return NULL;
    for (int i = 0; i < s->ntables; ++i)
        if (strcmp(s->tables[i].name, name) == 0) return &s->tables[i];
    return NULL;
}

/* ------------------------------------------------------------------ */
/* F6: 只读行采样（复用 B-tree 遍历，绝不修改字节）                     */
/* ------------------------------------------------------------------ */

typedef struct {
    np_db_row_cb cb;
    void *user;
    size_t rows;
    size_t max_rows;
    bool stop;
    const np_db_table_t *table;   /* 用于 INTEGER PRIMARY KEY 别名回填 */
} sample_user_t;

/* 把 np_db_value_t 文本化到 np_db_cell_t。
 * 文本/整数/ blob 均转成可读文本；NULL 用 is_null 标记。 */
static void value_to_cell(const np_db_value_t *v, np_db_cell_t *cell) {
    memset(cell, 0, sizeof(*cell));
    if (!v) { cell->is_null = true; return; }
    if (v->serial == 0) { cell->is_null = true; return; }
    if (v->serial >= 1 && v->serial <= 6) {
        cell->is_int = true;
        cell->ival = v->ival;
        snprintf(cell->text, sizeof(cell->text), "%lld", (long long)v->ival);
        return;
    }
    if (v->serial == 7) { /* float */
        snprintf(cell->text, sizeof(cell->text), "%.6g", v->fval);
        return;
    }
    if (v->serial == 8) { cell->is_int = true; cell->ival = 0;
        strcpy(cell->text, "0"); return; }
    if (v->serial == 9) { cell->is_int = true; cell->ival = 1;
        strcpy(cell->text, "1"); return; }
    if (v->ptr) {
        size_t n = v->len < sizeof(cell->text) - 1 ? v->len : sizeof(cell->text) - 1;
        if (v->len > n) cell->truncated = true;
        memcpy(cell->text, v->ptr, n);
        cell->text[n] = '\0';
        cell->is_blob = ((v->serial - 12) % 2 == 1);
    }
}

static void sample_row_cb(const np_db_value_t *vals, size_t n, uint64_t rowid,
                          void *user) {
    sample_user_t *su = (sample_user_t *)user;
    np_db_row_t row;
    size_t i;
    (void)rowid;
    if (su->stop || !su->cb) return;
    memset(&row, 0, sizeof(row));
    row.rowid = rowid;
    row.ncells = n < NP_DB_MAX_SAMPLE_COLS ? (int)n : NP_DB_MAX_SAMPLE_COLS;
    for (i = 0; i < (size_t)row.ncells; ++i)
        value_to_cell(&vals[i], &row.cells[i]);
    /* INTEGER PRIMARY KEY 别名列在 record 里是 NULL（serial 0），真实值=rowid */
    if (su->table && su->table->rowid_alias) {
        for (i = 0; i < (size_t)su->table->ncols && i < (size_t)row.ncells; ++i) {
            /* np_db_col_t 无 rowid_alias 字段；用 decl_type==INTEGER && pk 判定 */
            if (strcmp(su->table->cols[i].decl_type, "INTEGER") == 0 &&
                su->table->cols[i].pk) {
                row.cells[i].is_null = false;
                row.cells[i].is_int = true;
                row.cells[i].ival = rowid;
                snprintf(row.cells[i].text, sizeof(row.cells[i].text),
                         "%lld", (long long)rowid);
                break;
            }
        }
    }
    su->rows++;
    if (su->rows >= su->max_rows) su->stop = true;   /* 采样上限：不再继续 */
    if (!su->cb(&row, su->user)) su->stop = true;
}

size_t np_db_sample_rows(const unsigned char *data, size_t len,
                         const np_db_schema_t *schema, const char *table,
                         size_t max_rows, np_db_row_cb cb, void *user,
                         bool *partial) {
    const np_db_table_t *t;
    walk_ctx_t w;
    sample_user_t su;
    uint64_t cells = 0;
    if (partial) *partial = false;
    if (!data || !schema || !table || !max_rows) return 0;
    t = np_db_find_table(schema, table);
    if (!t || t->rootpage == 0) return 0;
    w.d = data; w.len = len; w.page_size = schema->page_size;
    w.reserved = (uint8_t)0; w.partial = false;
    /* reserved 字节不可从 schema 反推，需从文件头第 20 字节读回 */
    if (len > 20) w.reserved = data[20];
    su.cb = cb; su.user = user; su.rows = 0; su.max_rows = max_rows;
    su.stop = false; su.table = t;
    (void)walk_table_btree(&w, t->rootpage, sample_row_cb, &su, &cells);
    if (partial) *partial = w.partial;
    return su.rows;
}
