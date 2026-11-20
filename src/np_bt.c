/* np_bt.c - practical 010 Editor Binary Template interpreter.
 *
 * The engine is intentionally self-contained: a lexer turns the template
 * into tokens, a recursive-descent interpreter executes statements directly
 * from the token stream (no AST) and every typed declaration appends a field
 * to the result. Direct interpretation keeps the code small and lets control
 * flow (if/while/for) re-run a token range without rebuilding nodes.
 *
 * Safety limits guard against malformed or hostile templates: max source
 * size, max fields, max execution steps and hard bounds on every read. */

#include "np_bt.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BT_MAX_SOURCE   (4u * 1024 * 1024)
#define BT_MAX_FIELDS   200000
#define BT_MAX_STEPS    5000000
#define BT_MAX_TOKENS   200000
#define BT_MAX_SYMBOLS  20000
#define BT_MAX_STRUCTS  512
#define BT_NAME_CAP     128

/* ---------------- tokens ---------------- */

enum {
    TK_EOF = 0, TK_NUM, TK_FLOATNUM, TK_STR, TK_CHAR, TK_IDENT, TK_PUNCT
};

typedef struct {
    int     kind;
    char    text[96];
    int64_t num;
    double  dbl;
} bt_tok;

typedef struct {
    bt_tok *t;
    int     n, cap;
} bt_tokens;

static int bt_tokens_push(bt_tokens *b, int kind, const char *text, int len,
                          int64_t num, double dbl) {
    if (b->n >= b->cap) {
        int nc = b->cap ? b->cap * 2 : 256;
        bt_tok *nt = (bt_tok *)realloc(b->t, (size_t)nc * sizeof *nt);
        if (!nt) return 0;
        b->t = nt; b->cap = nc;
    }
    bt_tok *tk = &b->t[b->n++];
    memset(tk, 0, sizeof *tk);
    tk->kind = kind;
    tk->num = num; tk->dbl = dbl;
    if (text) {
        int l = len > 0 ? len : (int)strlen(text);
        if (l >= (int)sizeof tk->text) l = (int)sizeof tk->text - 1;
        memcpy(tk->text, text, (size_t)l);
        tk->text[l] = 0;
    }
    return 1;
}

/* Escape processing shared by string and character literals. */
static int bt_unescape(const char *s, int n, char *out, int cap) {
    int j = 0;
    for (int i = 0; i < n && j < cap - 1; i++) {
        char c = s[i];
        if (c == '\\' && i + 1 < n) {
            i++;
            char e = s[i];
            switch (e) {
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case '0': c = '\0'; break;
                case '\\': c = '\\'; break;
                case '\'': c = '\''; break;
                case '"': c = '"'; break;
                case 'x': {
                    int v = 0, k = 0;
                    while (i + 1 < n && k < 2 && isxdigit((unsigned char)s[i + 1])) {
                        char h = s[++i];
                        v = v * 16 + (isdigit((unsigned char)h) ? h - '0'
                            : (tolower((unsigned char)h) - 'a' + 10));
                        k++;
                    }
                    c = (char)v;
                    break;
                }
                default: c = e; break;
            }
        }
        out[j++] = c;
    }
    out[j] = 0;
    return j;
}

static int bt_is_ident_start(int c) { return isalpha(c) || c == '_'; }
static int bt_is_ident(int c) { return isalnum(c) || c == '_'; }

static bool bt_lex(const char *src, bt_tokens *b, char *err, size_t err_cap) {
    const char *p = src;
    while (*p) {
        if (isspace((unsigned char)*p)) { p++; continue; }
        if (p[0] == '/' && p[1] == '/') {
            p += 2; while (*p && *p != '\n') p++; continue;
        }
        if (p[0] == '/' && p[1] == '*') {
            p += 2;
            while (*p && !(p[0] == '*' && p[1] == '/')) p++;
            if (!*p) { snprintf(err, err_cap, "未闭合的块注释"); return false; }
            p += 2; continue;
        }
        if (bt_is_ident_start((unsigned char)*p)) {
            const char *s = p;
            while (bt_is_ident((unsigned char)*p)) p++;
            if (!bt_tokens_push(b, TK_IDENT, s, (int)(p - s), 0, 0)) goto oom;
            continue;
        }
        if (isdigit((unsigned char)*p) ||
            (p[0] == '.' && isdigit((unsigned char)p[1]))) {
            int64_t v = 0;
            double d = 0;
            bool isflt = false;
            if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
                p += 2;
                if (!isxdigit((unsigned char)*p)) {
                    snprintf(err, err_cap, "0x 后缺少十六进制数字"); return false;
                }
                while (isxdigit((unsigned char)*p)) {
                    int h = isdigit((unsigned char)*p) ? *p - '0'
                        : tolower((unsigned char)*p) - 'a' + 10;
                    v = (int64_t)((uint64_t)v * 16u + (uint64_t)h);
                    p++;
                }
            } else {
                while (isdigit((unsigned char)*p)) {
                    v = v * 10 + (*p - '0'); p++;
                }
                if (*p == '.') {
                    isflt = true;
                    p++;
                    double frac = 0, scale = 0.1;
                    while (isdigit((unsigned char)*p)) {
                        frac += (*p - '0') * scale; scale *= 0.1; p++;
                    }
                    d = (double)v + frac;
                }
                if (*p == 'e' || *p == 'E') {
                    isflt = true; p++;
                    int neg = 0;
                    if (*p == '+' || *p == '-') { neg = (*p == '-'); p++; }
                    int e = 0;
                    while (isdigit((unsigned char)*p)) { e = e * 10 + (*p - '0'); p++; }
                    double mul = 1;
                    for (int i = 0; i < e; i++) mul *= 10;
                    d = d * (neg ? 1.0 / mul : mul);
                    if (!isflt) { d = (double)v * (neg ? 1.0 / mul : mul); }
                }
            }
            if (!bt_tokens_push(b, isflt ? TK_FLOATNUM : TK_NUM, NULL, 0,
                                isflt ? 0 : v, isflt ? (d ? d : (double)v) : 0)) goto oom;
            continue;
        }
        if (*p == '"' || *p == '\'') {
            char quote = *p++;
            const char *s = p;
            while (*p && *p != quote) {
                if (*p == '\\' && p[1]) p += 2; else p++;
            }
            if (!*p) {
                snprintf(err, err_cap, "未闭合的字符串/字符字面量"); return false;
            }
            int n = (int)(p - s); p++; /* skip closing quote */
            char buf[96];
            int outn = bt_unescape(s, n, buf, (int)sizeof buf);
            if (quote == '"') {
                if (!bt_tokens_push(b, TK_STR, buf, outn, 0, 0)) goto oom;
            } else {
                int64_t cv = outn > 0 ? (unsigned char)buf[0] : 0;
                if (!bt_tokens_push(b, TK_CHAR, NULL, 0, cv, 0)) goto oom;
            }
            continue;
        }
        /* multi-character punctuation first */
        static const char *ops[] = {
            "<<=", ">>=", "++", "--", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=",
            "&&", "||", "==", "!=", "<=", ">=", "<<", ">>", NULL
        };
        int matched = 0;
        for (int i = 0; ops[i]; i++) {
            size_t l = strlen(ops[i]);
            if (strncmp(p, ops[i], l) == 0) {
                if (!bt_tokens_push(b, TK_PUNCT, ops[i], (int)l, 0, 0)) goto oom;
                p += l; matched = 1; break;
            }
        }
        if (matched) continue;
        if (strchr("+-*/%&|^~!<>=?:;,.(){}[]", *p)) {
            char q[2] = { *p++, 0 };
            if (!bt_tokens_push(b, TK_PUNCT, q, 1, 0, 0)) goto oom;
            continue;
        }
        snprintf(err, err_cap, "无法识别的字符 '%c'", *p);
        return false;
    oom:
        snprintf(err, err_cap, "模板过大或内存不足"); return false;
    }
    if (b->n >= BT_MAX_TOKENS) {
        snprintf(err, err_cap, "模板 token 数超过 %d", BT_MAX_TOKENS); return false;
    }
    bt_tokens_push(b, TK_EOF, NULL, 0, 0, 0);
    return true;
}

/* ---------------- values ---------------- */

typedef struct {
    int64_t i;
    double  d;
    bool    real;
} bt_val;

static double bt_vd(bt_val v) { return v.real ? v.d : (double)v.i; }
static int64_t bt_vi(bt_val v) { return v.real ? (int64_t)v.d : v.i; }
static bt_val bt_iv(int64_t x) { bt_val v = { x, 0, false }; return v; }
static bt_val bt_dv(double x) { bt_val v = { 0, x, true }; return v; }

/* ---------------- structs, symbols, vm ---------------- */

typedef struct bt_struct {
    char name[64];
    int  begin, end;          /* token range inside { } */
    struct bt_struct *next;
} bt_struct;

typedef struct {
    char    name[BT_NAME_CAP];
    int64_t value;
} bt_sym;

typedef struct {
    char name[64];
    char base[64];            /* underlying scalar type for typedef aliases */
    bt_struct *st;            /* non-null when alias names a struct */
} bt_alias;

typedef struct {
    bt_tok     *tok;
    int         n, pos;
    const uint8_t *data;
    size_t      size, off;
    bool        big_endian;
    int         steps;
    bool        failed;
    char        error[256];
    np_bt_result *out;
    bt_struct  *structs;
    bt_alias    aliases[128];
    int         naliases;
    bt_sym     *syms;
    int         nsyms, symcap;
    int         parent;       /* current field-tree parent */
    int         depth;
    char        scope[BT_NAME_CAP]; /* current struct scope for relative names */
} bt_vm;

static void bt_fail(bt_vm *v, const char *fmt, ...) {
    if (v->failed) return;
    va_list ap; va_start(ap, fmt);
    vsnprintf(v->error, sizeof v->error, fmt, ap);
    va_end(ap);
    v->failed = true;
}

static void bt_tick(bt_vm *v) {
    if (++v->steps > BT_MAX_STEPS)
        bt_fail(v, "执行步数超限（模板可能含死循环）");
}

static bt_sym *bt_sym_find(bt_vm *v, const char *name) {
    for (int i = 0; i < v->nsyms; i++)
        if (strcmp(v->syms[i].name, name) == 0) return &v->syms[i];
    return NULL;
}

static void bt_sym_set(bt_vm *v, const char *name, int64_t value) {
    bt_sym *s = bt_sym_find(v, name);
    if (s) { s->value = value; return; }
    if (v->nsyms >= BT_MAX_SYMBOLS) {
        bt_fail(v, "变量数超过 %d", BT_MAX_SYMBOLS); return;
    }
    if (v->nsyms >= v->symcap) {
        int nc = v->symcap ? v->symcap * 2 : 128;
        bt_sym *ns = (bt_sym *)realloc(v->syms, (size_t)nc * sizeof *ns);
        if (!ns) { bt_fail(v, "内存不足"); return; }
        v->syms = ns; v->symcap = nc;
    }
    bt_sym *d = &v->syms[v->nsyms++];
    snprintf(d->name, sizeof d->name, "%s", name);
    d->value = value;
}

/* ---------------- fields ---------------- */

static int bt_field_add(bt_vm *v, const char *name, const char *type,
                        const char *value, size_t off, size_t size, int color) {
    if (v->failed) return -1;
    np_bt_result *r = v->out;
    if (r->count >= BT_MAX_FIELDS || r->count >= r->cap) {
        if (r->cap >= BT_MAX_FIELDS) { bt_fail(v, "字段数超过 %d", BT_MAX_FIELDS); return -1; }
        int nc = r->cap ? r->cap * 2 : 256;
        if (nc > BT_MAX_FIELDS) nc = BT_MAX_FIELDS;
        np_bt_field *nf = (np_bt_field *)realloc(r->fields, (size_t)nc * sizeof *nf);
        if (!nf) { bt_fail(v, "内存不足"); return -1; }
        r->fields = nf; r->cap = nc;
    }
    np_bt_field *f = &r->fields[r->count];
    memset(f, 0, sizeof *f);
    snprintf(f->name, sizeof f->name, "%s", name);
    snprintf(f->type, sizeof f->type, "%s", type);
    snprintf(f->value, sizeof f->value, "%s", value);
    f->offset = off; f->size = size;
    f->depth = v->depth; f->parent = v->parent; f->color = color;
    return r->count++;
}

/* ---------------- primitive types ---------------- */

typedef struct {
    const char *name;
    int   size;               /* 0 = variable / special */
    bool  sign;
    bool  is_float;
    int   kind;               /* 1 = scalar, 2 = string, 3 = wstring */
} bt_prim;

static const bt_prim bt_prims[] = {
    { "char",    1, true,  false, 1 },
    { "byte",    1, false, false, 1 },
    { "ubyte",   1, false, false, 1 },
    { "int8",    1, true,  false, 1 },
    { "uint8",   1, false, false, 1 },
    { "short",   2, true,  false, 1 },
    { "ushort",  2, false, false, 1 },
    { "word",    2, false, false, 1 },
    { "int16",   2, true,  false, 1 },
    { "uint16",  2, false, false, 1 },
    { "int",     4, true,  false, 1 },
    { "int32",   4, true,  false, 1 },
    { "uint",    4, false, false, 1 },
    { "uint32",  4, false, false, 1 },
    { "dword",   4, false, false, 1 },
    { "float",   4, false, true,  1 },
    { "float32", 4, false, true,  1 },
    { "int64",   8, true,  false, 1 },
    { "uint64",  8, false, false, 1 },
    { "double",  8, false, true,  1 },
    { "float64", 8, false, true,  1 },
    { "qword",   8, false, false, 1 },
    { "string",  0, false, false, 2 },
    { "wstring", 0, false, false, 3 },
};

static const bt_prim *bt_prim_find(const char *name) {
    for (size_t i = 0; i < sizeof bt_prims / sizeof *bt_prims; i++)
        if (strcmp(bt_prims[i].name, name) == 0) return &bt_prims[i];
    return NULL;
}

/* Resolve a type name to either a primitive, an alias or a struct. */
typedef struct {
    const bt_prim *prim;
    bt_struct *st;
    char name[64];
} bt_type;

static bool bt_resolve_type(bt_vm *v, const char *name, bt_type *out) {
    memset(out, 0, sizeof *out);
    snprintf(out->name, sizeof out->name, "%s", name);
    const bt_prim *p = bt_prim_find(name);
    if (p) { out->prim = p; return true; }
    for (int i = 0; i < v->naliases; i++) {
        if (strcmp(v->aliases[i].name, name) == 0) {
            if (v->aliases[i].st) { out->st = v->aliases[i].st; return true; }
            const bt_prim *ap = bt_prim_find(v->aliases[i].base);
            if (ap) { out->prim = ap; return true; }
        }
    }
    for (bt_struct *s = v->structs; s; s = s->next)
        if (strcmp(s->name, name) == 0) { out->st = s; return true; }
    return false;
}

/* ---------------- byte reads ---------------- */

static uint64_t bt_read_bits(bt_vm *v, size_t off, int bytes, bool *ok) {
    *ok = true;
    if (off + (size_t)bytes > v->size) { *ok = false; return 0; }
    uint64_t r = 0;
    if (v->big_endian) {
        for (int i = 0; i < bytes; i++) r = (r << 8) | v->data[off + i];
    } else {
        for (int i = bytes - 1; i >= 0; i--) r = (r << 8) | v->data[off + i];
    }
    return r;
}

static int64_t bt_read_signed(uint64_t raw, int bytes) {
    if (bytes >= 8) return (int64_t)raw;
    uint64_t sign = 1ull << (bytes * 8 - 1);
    if (raw & sign) return (int64_t)(raw | ~((1ull << (bytes * 8)) - 1));
    return (int64_t)raw;
}

static double bt_read_float(uint64_t raw, int bytes) {
    if (bytes == 4) {
        float f;
        uint32_t u = (uint32_t)raw;
        memcpy(&f, &u, 4);
        return (double)f;
    }
    double d;
    memcpy(&d, &raw, 8);
    return d;
}

static void bt_format_value(char *buf, size_t cap, const bt_prim *p,
                            uint64_t raw, int bytes) {
    if (!p) { if (cap > 0) buf[0] = 0; return; }
    if (p->is_float) {
        snprintf(buf, cap, "%.6g", bt_read_float(raw, bytes));
    } else if (p->kind == 1 && p->sign && bytes == 1 && p->size == 1 && strcmp(p->name, "char") == 0) {
        int c = (int)(int64_t)bt_read_signed(raw, 1);
        if (c >= 32 && c < 127) snprintf(buf, cap, "'%c' (0x%02X)", c, (unsigned)(raw & 0xff));
        else snprintf(buf, cap, "0x%02X", (unsigned)(raw & 0xff));
    } else if (p->sign) {
        snprintf(buf, cap, "%lld", (long long)bt_read_signed(raw, bytes));
    } else {
        if (bytes == 1) snprintf(buf, cap, "0x%02X (%u)", (unsigned)(raw & 0xff), (unsigned)(raw & 0xff));
        else if (bytes == 2) snprintf(buf, cap, "%u (0x%04X)", (unsigned)(raw & 0xffff), (unsigned)(raw & 0xffff));
        else snprintf(buf, cap, "%llu (0x%llX)", (unsigned long long)raw, (unsigned long long)raw);
    }
}

/* ---------------- forward decls ---------------- */

static int bt_exec_block(bt_vm *v, int end, const char *prefix);
static bt_val bt_expr(bt_vm *v, int min_prec);

/* ---------------- statement helpers ---------------- */

static bool bt_is_punct(bt_vm *v, int i, const char *s) {
    return i < v->n && v->tok[i].kind == TK_PUNCT && strcmp(v->tok[i].text, s) == 0;
}
static bool bt_is_ident_tok(bt_vm *v, int i, const char *s) {
    return i < v->n && v->tok[i].kind == TK_IDENT && strcmp(v->tok[i].text, s) == 0;
}
static void bt_expect(bt_vm *v, const char *s) {
    if (!bt_is_punct(v, v->pos, s))
        bt_fail(v, "期望 '%s'，实际 '%s'", s,
                v->pos < v->n ? v->tok[v->pos].text : "EOF");
    else v->pos++;
}

/* Compose "prefix.name" or "prefix.name[i]" for symbols/field names. */
static void bt_compose(char *out, size_t cap, const char *prefix,
                       const char *name, int index, bool has_index) {
    if (has_index)
        snprintf(out, cap, "%s%s[%d]", prefix, name, index);
    else
        snprintf(out, cap, "%s%s", prefix, name);
}

/* Parse [expr] after a variable name. Returns -1 when absent. */
static int64_t bt_parse_array_len(bt_vm *v) {
    if (!bt_is_punct(v, v->pos, "[")) return -1;
    v->pos++;
    bt_val n = bt_expr(v, 0);
    bt_expect(v, "]");
    int64_t len = bt_vi(n);
    if (len < 0) len = 0;
    if (len > 1000000) { bt_fail(v, "数组长度过大"); len = 0; }
    return len;
}

/* Read a string field (ASCII or UTF-16) up to NUL. */
static size_t bt_read_string_field(bt_vm *v, const char *full, const char *type,
                                   bool wide, int color, char *display, size_t dcap) {
    size_t start = v->off, n = 0;
    char text[160];
    size_t ti = 0;
    if (!wide) {
        while (v->off < v->size && v->data[v->off] != 0 && ti + 1 < sizeof text) {
            unsigned char c = v->data[v->off];
            text[ti++] = (c >= 32 && c < 127) ? (char)c : '.';
            v->off++;
        }
        if (v->off < v->size) v->off++; /* consume NUL */
        n = v->off - start;
    } else {
        while (v->off + 1 < v->size && ti + 1 < sizeof text) {
            uint16_t u = (uint16_t)bt_read_bits(v, v->off, 2, &(bool){ true });
            if (u == 0) { v->off += 2; break; }
            text[ti++] = (u >= 32 && u < 127) ? (char)u : '.';
            v->off += 2;
        }
        n = v->off - start;
    }
    text[ti] = 0;
    snprintf(display, dcap, "\"%s\"", text);
    bt_field_add(v, full, type, display, start, n, color);
    bt_sym_set(v, full, (int64_t)start);
    return n;
}

/* Execute the member range of a struct for one variable. */
static void bt_exec_struct(bt_vm *v, bt_struct *st, const char *prefix, int parent) {
    int save_pos = v->pos, save_parent = v->parent, save_depth = v->depth;
    char save_scope[BT_NAME_CAP];
    snprintf(save_scope, sizeof save_scope, "%s", v->scope);
    v->pos = st->begin;
    v->parent = parent;
    v->depth++;
    snprintf(v->scope, sizeof v->scope, "%s", prefix);
    bt_exec_block(v, st->end, prefix);
    v->pos = save_pos; v->parent = save_parent; v->depth = save_depth;
    snprintf(v->scope, sizeof v->scope, "%s", save_scope);
}

/* A typed declaration statement (type name [len]; ). */
static void bt_exec_decl(bt_vm *v, bt_type *ty, const char *prefix) {
    if (v->pos >= v->n || v->tok[v->pos].kind != TK_IDENT) {
        bt_fail(v, "类型 %s 后缺少变量名", ty->name); return;
    }
    char var[BT_NAME_CAP];
    snprintf(var, sizeof var, "%s", v->tok[v->pos++].text);
    int64_t alen = bt_parse_array_len(v);
    bool is_array = alen >= 0;
    if (!is_array) alen = 1;

    /* `type name = expr;` declares a pure script variable: it does not read
     * bytes or produce a field. 010 Editor uses this for loop counters. */
    if (bt_is_punct(v, v->pos, "=")) {
        v->pos++;
        bt_val init = bt_expr(v, 0);
        bt_expect(v, ";");
        if (!v->failed) {
            char full[BT_NAME_CAP];
            bt_compose(full, sizeof full, prefix, var, 0, false);
            bt_sym_set(v, full, bt_vi(init));
        }
        return;
    }
    bt_expect(v, ";");
    if (v->failed) return;

    if (ty->st) {
        char full[BT_NAME_CAP];
        bt_compose(full, sizeof full, prefix, var, 0, false);
        int parent = bt_field_add(v, full, ty->name, "", v->off, 0, 6);
        size_t start = v->off;
        for (int64_t i = 0; i < alen && !v->failed; i++) {
            char cp[BT_NAME_CAP];
            if (is_array) bt_compose(cp, sizeof cp, prefix, var, (int)i, true);
            else snprintf(cp, sizeof cp, "%s%s", prefix, var);
            strcat(cp, ".");
            bt_exec_struct(v, ty->st, cp, parent);
        }
        if (parent >= 0 && v->out && parent < v->out->count)
            v->out->fields[parent].size = v->off - start;
        return;
    }

    const bt_prim *p = ty->prim;
    if (!p) { bt_fail(v, "未知类型 %s", ty->name); return; }

    if (p->kind == 2 || p->kind == 3) {
        char full[BT_NAME_CAP], disp[192];
        bt_compose(full, sizeof full, prefix, var, 0, false);
        bt_read_string_field(v, full, ty->name, p->kind == 3, 5, disp, sizeof disp);
        return;
    }

    int bytes = p->size;
    size_t start = v->off;
    char full[BT_NAME_CAP];
    bt_compose(full, sizeof full, prefix, var, 0, false);

    int parent = -1;
    if (is_array && alen > 1 && alen <= 64) {
        char tname[80];
        snprintf(tname, sizeof tname, "%s[%lld]", ty->name, (long long)alen);
        parent = bt_field_add(v, full, tname, "", start, (size_t)(bytes * alen), 4);
        if (parent >= 0) {
            int sp = v->parent, sd = v->depth;
            v->parent = parent; v->depth++;
            for (int64_t i = 0; i < alen && !v->failed; i++) {
                bool ok;
                uint64_t raw = bt_read_bits(v, v->off, bytes, &ok);
                if (!ok) { bt_fail(v, "读取数据越界（偏移 %llu）", (unsigned long long)v->off); break; }
                char el[BT_NAME_CAP], val[160];
                bt_compose(el, sizeof el, prefix, var, (int)i, true);
                bt_format_value(val, sizeof val, p, raw, bytes);
                bt_field_add(v, el, ty->name, val, v->off, (size_t)bytes, 4);
                v->off += (size_t)bytes;
            }
            v->parent = sp; v->depth = sd;
        }
    } else {
        /* single element, or a large array represented as one summary field */
        char disp[192] = "";
        if (is_array && alen > 1) {
            snprintf(disp, sizeof disp, "%lld 个元素", (long long)alen);
        } else {
            bool ok;
            uint64_t raw = bt_read_bits(v, v->off, bytes, &ok);
            if (!ok) { bt_fail(v, "读取数据越界（偏移 %llu）", (unsigned long long)v->off); return; }
            bt_format_value(disp, sizeof disp, p, raw, bytes);
            bt_sym_set(v, full, p->is_float ? bt_vi(bt_dv(bt_read_float(raw, bytes)))
                                            : (p->sign ? bt_read_signed(raw, bytes) : (int64_t)raw));
        }
        size_t total = (size_t)bytes * (size_t)alen;
        if (v->off + total > v->size) {
            bt_fail(v, "读取数据越界（偏移 %llu，长度 %llu）",
                    (unsigned long long)v->off, (unsigned long long)total);
            return;
        }
        char tname[80];
        if (is_array) snprintf(tname, sizeof tname, "%s[%lld]", ty->name, (long long)alen);
        else snprintf(tname, sizeof tname, "%s", ty->name);
        bt_field_add(v, full, tname, disp, v->off, total, 3);
        if (is_array) bt_sym_set(v, full, (int64_t)v->off);  /* arrays: store base offset */
        v->off += total;
    }
}

/* ---------------- expression parser (precedence climbing) ---------------- */

static int bt_prec(const char *op) {
    if (!strcmp(op, "||")) return 1;
    if (!strcmp(op, "&&")) return 2;
    if (!strcmp(op, "|")) return 3;
    if (!strcmp(op, "^")) return 4;
    if (!strcmp(op, "&")) return 5;
    if (!strcmp(op, "==") || !strcmp(op, "!=")) return 6;
    if (!strcmp(op, "<") || !strcmp(op, ">") || !strcmp(op, "<=") || !strcmp(op, ">=")) return 7;
    if (!strcmp(op, "<<") || !strcmp(op, ">>")) return 8;
    if (!strcmp(op, "+") || !strcmp(op, "-")) return 9;
    if (!strcmp(op, "*") || !strcmp(op, "/") || !strcmp(op, "%")) return 10;
    return -1;
}

static bt_val bt_apply_bin(bt_vm *v, const char *op, bt_val a, bt_val b) {
    bool fl = a.real || b.real;
    if (!strcmp(op, "==")) return bt_iv(fl ? (bt_vd(a) == bt_vd(b)) : (bt_vi(a) == bt_vi(b)));
    if (!strcmp(op, "!=")) return bt_iv(fl ? (bt_vd(a) != bt_vd(b)) : (bt_vi(a) != bt_vi(b)));
    if (!strcmp(op, "<"))  return bt_iv(fl ? (bt_vd(a) <  bt_vd(b)) : (bt_vi(a) <  bt_vi(b)));
    if (!strcmp(op, ">"))  return bt_iv(fl ? (bt_vd(a) >  bt_vd(b)) : (bt_vi(a) >  bt_vi(b)));
    if (!strcmp(op, "<=")) return bt_iv(fl ? (bt_vd(a) <= bt_vd(b)) : (bt_vi(a) <= bt_vi(b)));
    if (!strcmp(op, ">=")) return bt_iv(fl ? (bt_vd(a) >= bt_vd(b)) : (bt_vi(a) >= bt_vi(b)));
    if (!strcmp(op, "&&")) return bt_iv((bt_vi(a) != 0) && (bt_vi(b) != 0));
    if (!strcmp(op, "||")) return bt_iv((bt_vi(a) != 0) || (bt_vi(b) != 0));
    if (!strcmp(op, "+")) return fl ? bt_dv(bt_vd(a) + bt_vd(b)) : bt_iv(bt_vi(a) + bt_vi(b));
    if (!strcmp(op, "-")) return fl ? bt_dv(bt_vd(a) - bt_vd(b)) : bt_iv(bt_vi(a) - bt_vi(b));
    if (!strcmp(op, "*")) return fl ? bt_dv(bt_vd(a) * bt_vd(b)) : bt_iv(bt_vi(a) * bt_vi(b));
    if (!strcmp(op, "/")) {
        if (fl) { double d = bt_vd(b); if (d == 0) { bt_fail(v, "除零"); return bt_iv(0); } return bt_dv(bt_vd(a) / d); }
        int64_t d = bt_vi(b); if (!d) { bt_fail(v, "除零"); return bt_iv(0); }
        return bt_iv(bt_vi(a) / d);
    }
    if (!strcmp(op, "%")) {
        int64_t d = bt_vi(b); if (!d) { bt_fail(v, "取模除零"); return bt_iv(0); }
        return bt_iv(bt_vi(a) % d);
    }
    int64_t a1 = bt_vi(a), b1 = bt_vi(b);
    if (!strcmp(op, "&"))  return bt_iv(a1 & b1);
    if (!strcmp(op, "|"))  return bt_iv(a1 | b1);
    if (!strcmp(op, "^"))  return bt_iv(a1 ^ b1);
    if (!strcmp(op, "<<")) return bt_iv(a1 << (b1 & 63));
    if (!strcmp(op, ">>")) return bt_iv(a1 >> (b1 & 63));
    bt_fail(v, "未知运算符 %s", op);
    return bt_iv(0);
}

static bt_val bt_call_builtin(bt_vm *v, const char *name) {
    /* Arguments are already consumed by the caller? No: parse them here. */
    bt_expect(v, "(");
    bt_val args[8]; int na = 0;
    if (!bt_is_punct(v, v->pos, ")")) {
        for (;;) {
            if (na < 8) args[na] = bt_expr(v, 0);
            else bt_expr(v, 0);
            na++;
            if (bt_is_punct(v, v->pos, ",")) { v->pos++; continue; }
            break;
        }
    }
    bt_expect(v, ")");
    int64_t a0 = na > 0 ? bt_vi(args[0]) : 0;
    int64_t a1 = na > 1 ? bt_vi(args[1]) : 0;
    if (!strcmp(name, "FTell")) return bt_iv((int64_t)v->off);
    if (!strcmp(name, "FSize")) return bt_iv((int64_t)v->size);
    if (!strcmp(name, "FEof")) return bt_iv(v->off >= v->size);
    if (!strcmp(name, "FSeek")) {
        if (a0 >= 0 && (size_t)a0 <= v->size) v->off = (size_t)a0;
        else bt_fail(v, "FSeek 偏移越界: %lld", (long long)a0);
        return bt_iv((int64_t)v->off);
    }
    if (!strcmp(name, "BigEndian")) { v->big_endian = true; return bt_iv(1); }
    if (!strcmp(name, "LittleEndian")) { v->big_endian = false; return bt_iv(1); }
    if (!strcmp(name, "Abs")) return bt_iv(a0 < 0 ? -a0 : a0);
    if (!strcmp(name, "Min")) return bt_iv(a0 < a1 ? a0 : a1);
    if (!strcmp(name, "Max")) return bt_iv(a0 > a1 ? a0 : a1);
    /* ReadXxx() helpers: read a scalar and advance the cursor. */
    struct { const char *n; int sz; bool sign; bool flt; } reads[] = {
        { "ReadByte", 1, false, false }, { "ReadUByte", 1, false, false },
        { "ReadChar", 1, true, false }, { "ReadShort", 2, true, false },
        { "ReadUShort", 2, false, false }, { "ReadWord", 2, false, false },
        { "ReadInt", 4, true, false }, { "ReadUInt", 4, false, false },
        { "ReadDWord", 4, false, false }, { "ReadFloat", 4, false, true },
        { "ReadInt64", 8, true, false }, { "ReadUInt64", 8, false, false },
        { "ReadDouble", 8, false, true },
    };
    for (size_t i = 0; i < sizeof reads / sizeof *reads; i++) {
        if (!strcmp(name, reads[i].n)) {
            bool ok; uint64_t raw = bt_read_bits(v, v->off, reads[i].sz, &ok);
            if (!ok) { bt_fail(v, "%s 读取越界", name); return bt_iv(0); }
            v->off += (size_t)reads[i].sz;
            if (reads[i].flt) return bt_dv(bt_read_float(raw, reads[i].sz));
            return bt_iv(reads[i].sign ? bt_read_signed(raw, reads[i].sz) : (int64_t)raw);
        }
    }
    bt_fail(v, "未知函数 %s()", name);
    return bt_iv(0);
}

static bt_val bt_primary(bt_vm *v) {
    if (v->pos >= v->n || v->failed) return bt_iv(0);
    bt_tok *t = &v->tok[v->pos];
    bt_val res = bt_iv(0);
    if (t->kind == TK_NUM) { res = bt_iv(t->num); v->pos++; return res; }
    if (t->kind == TK_FLOATNUM) { res = bt_dv(t->dbl); v->pos++; return res; }
    if (t->kind == TK_CHAR) { res = bt_iv(t->num); v->pos++; return res; }
    if (t->kind == TK_STR) { res = bt_iv(0); v->pos++; return res; }
    if (bt_is_punct(v, v->pos, "(")) {
        v->pos++;
        res = bt_expr(v, 0);
        bt_expect(v, ")");
        return res;
    }
    if (bt_is_punct(v, v->pos, "!") || bt_is_punct(v, v->pos, "~") ||
        bt_is_punct(v, v->pos, "+") || bt_is_punct(v, v->pos, "-") ||
        bt_is_punct(v, v->pos, "++") || bt_is_punct(v, v->pos, "--")) {
        char op[8]; snprintf(op, sizeof op, "%s", t->text); v->pos++;
        bt_val a = bt_expr(v, 11);
        if (!strcmp(op, "!")) return bt_iv(!bt_vi(a));
        if (!strcmp(op, "~")) return bt_iv(~bt_vi(a));
        if (!strcmp(op, "+")) return a;
        if (!strcmp(op, "-")) return a.real ? bt_dv(-bt_vd(a)) : bt_iv(-bt_vi(a));
        int64_t nv = bt_vi(a) + (!strcmp(op, "++") ? 1 : -1);
        /* prefix inc/dec on a plain identifier */
        if (a.real) { bt_fail(v, "++ / -- 需要整型变量"); return a; }
        return bt_iv(nv);
    }
    if (t->kind == TK_IDENT) {
        char name[BT_NAME_CAP];
        snprintf(name, sizeof name, "%s", t->text); v->pos++;
        /* member / index composition: obj.member, arr[i] */
        while (v->pos < v->n) {
            if (bt_is_punct(v, v->pos, ".")) {
                v->pos++;
                if (v->pos >= v->n || v->tok[v->pos].kind != TK_IDENT) { bt_fail(v, "'.' 后缺少成员名"); break; }
                char full[BT_NAME_CAP];
                snprintf(full, sizeof full, "%s.%s", name, v->tok[v->pos].text);
                snprintf(name, sizeof name, "%s", full);
                v->pos++;
            } else if (bt_is_punct(v, v->pos, "[")) {
                v->pos++;
                bt_val idx = bt_expr(v, 0);
                bt_expect(v, "]");
                char full[BT_NAME_CAP];
                snprintf(full, sizeof full, "%s[%lld]", name, (long long)bt_vi(idx));
                snprintf(name, sizeof name, "%s", full);
            } else break;
        }
        if (bt_is_punct(v, v->pos, "(")) return bt_call_builtin(v, name);
        if (!strcmp(name, "true")) return bt_iv(1);
        if (!strcmp(name, "false")) return bt_iv(0);
        bt_sym *s = bt_sym_find(v, name);
        if (!s && v->scope[0]) {
            char scoped[BT_NAME_CAP];
            snprintf(scoped, sizeof scoped, "%s%s", v->scope, name);
            s = bt_sym_find(v, scoped);
            if (s) snprintf(name, sizeof name, "%s", scoped);
        }
        if (!s) {
            bt_fail(v, "未定义标识符 %s", name);
            return bt_iv(0);
        }
        res = bt_iv(s->value);
        if (bt_is_punct(v, v->pos, "++") || bt_is_punct(v, v->pos, "--")) {
            int delta = bt_is_punct(v, v->pos, "++") ? 1 : -1;
            v->pos++;
            bt_sym_set(v, name, s->value + delta);
        }
        return res;
    }
    bt_fail(v, "表达式语法错误（'%s'）", t->text);
    v->pos++;
    return bt_iv(0);
}

static bt_val bt_expr(bt_vm *v, int min_prec) {
    bt_val lhs = bt_primary(v);
    while (!v->failed && v->pos < v->n) {
        if (bt_is_punct(v, v->pos, "?")) {
            if (min_prec > 0) break;
            v->pos++;
            bt_val a = bt_expr(v, 0);
            bt_expect(v, ":");
            bt_val b = bt_expr(v, 0);
            lhs = bt_vi(lhs) ? a : b;
            continue;
        }
        if (v->tok[v->pos].kind != TK_PUNCT) break;
        const char *op = v->tok[v->pos].text;
        int pr = bt_prec(op);
        if (pr < min_prec) break;
        v->pos++;
        bt_val rhs = bt_expr(v, pr + 1);
        lhs = bt_apply_bin(v, op, lhs, rhs);
    }
    return lhs;
}

/* Assignment target: consume an identifier (with . / [] composition). */
static void bt_assignment(bt_vm *v) {
    char name[BT_NAME_CAP];
    snprintf(name, sizeof name, "%s", v->tok[v->pos++].text);
    while (bt_is_punct(v, v->pos, ".") || bt_is_punct(v, v->pos, "[")) {
        if (bt_is_punct(v, v->pos, ".")) {
            v->pos++;
            if (v->pos < v->n && v->tok[v->pos].kind == TK_IDENT) {
                char f[BT_NAME_CAP];
                snprintf(f, sizeof f, "%s.%s", name, v->tok[v->pos].text);
                snprintf(name, sizeof name, "%s", f); v->pos++;
            }
        } else {
            v->pos++;
            bt_val idx = bt_expr(v, 0);
            bt_expect(v, "]");
            char f[BT_NAME_CAP];
            snprintf(f, sizeof f, "%s[%lld]", name, (long long)bt_vi(idx));
            snprintf(name, sizeof name, "%s", f);
        }
    }
    static const char *asg[] = { "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>=", NULL };
    const char *aop = NULL;
    for (int i = 0; asg[i]; i++)
        if (bt_is_punct(v, v->pos, asg[i])) { aop = asg[i]; v->pos++; break; }
    if (!aop) bt_expect(v, "=");
    bt_val rhs = bt_expr(v, 0);
    bt_expect(v, ";");
    if (v->failed) return;
    bt_sym *s = bt_sym_find(v, name);
    if (!s) { bt_fail(v, "赋值给未定义变量 %s", name); return; }
    int64_t nv = bt_vi(rhs);
    if (aop) {
        int64_t old = s->value;
        char op[4] = { aop[0], 0, 0, 0 };
        if (aop[1] == '=') {
            if (aop[0] == '<' || aop[0] == '>') { op[0] = aop[0]; op[1] = aop[0]; op[2] = 0; }
        }
        nv = bt_vi(bt_apply_bin(v, aop[0] == '<' || aop[0] == '>' ? (aop[0] == '<' ? "<<" : ">>") : op,
                                bt_iv(old), bt_iv(nv)));
    }
    s->value = nv;
}

/* ---------------- statements ---------------- */

#define EX_NORMAL   0
#define EX_BREAK    1
#define EX_CONTINUE 2
#define EX_STOP     4

/* Find the index of the '}' matching the '{' at `open`. */
static int bt_match_brace(bt_vm *v, int open) {
    int d = 0;
    for (int i = open; i < v->n; i++) {
        if (bt_is_punct(v, i, "{")) d++;
        else if (bt_is_punct(v, i, "}")) {
            if (--d == 0) return i;
        }
    }
    return v->n - 1;
}

/* Skip exactly one statement starting at v->pos (used for untaken branches). */
static void bt_skip_stmt(bt_vm *v, int end) {
    while (v->pos < end) {
        if (bt_is_punct(v, v->pos, "{")) {
            int close = bt_match_brace(v, v->pos);
            v->pos = close < end ? close + 1 : end;
            return;
        }
        if (bt_is_punct(v, v->pos, ";")) { v->pos++; return; }
        v->pos++;
    }
}

/* Execute one statement starting at v->pos; returns control-flow flags. */
static int bt_exec_stmt(bt_vm *v, int end, const char *prefix) {
    if (v->failed || v->pos >= end) return EX_NORMAL;
    bt_tick(v);
    if (v->failed) return EX_STOP;
    bt_tok *t = &v->tok[v->pos];
    if (t->kind == TK_EOF) { v->pos++; return EX_NORMAL; }

    if (bt_is_punct(v, v->pos, ";")) { v->pos++; return EX_NORMAL; }
    if (bt_is_punct(v, v->pos, "}")) { v->pos++; return EX_NORMAL; }
    if (bt_is_punct(v, v->pos, "{")) {
        int close = bt_match_brace(v, v->pos);
        if (close >= end) close = end - 1;
        v->pos++;
        int r = bt_exec_block(v, close, prefix);
        v->pos = close + 1;
        return r;
    }
    if (bt_is_ident_tok(v, v->pos, "break")) { v->pos++; bt_expect(v, ";"); return EX_BREAK; }
    if (bt_is_ident_tok(v, v->pos, "continue")) { v->pos++; bt_expect(v, ";"); return EX_CONTINUE; }

    if (bt_is_ident_tok(v, v->pos, "if")) {
        v->pos++;
        bt_expect(v, "(");
        bt_val c = bt_expr(v, 0);
        bt_expect(v, ")");
        int take = bt_vi(c) != 0;
        int r = EX_NORMAL;
        if (take) r = bt_exec_stmt(v, end, prefix);
        else { bt_skip_stmt(v, end); r = EX_NORMAL; }
        if (bt_is_ident_tok(v, v->pos, "else")) {
            v->pos++;
            if (take) { bt_skip_stmt(v, end); }
            else { r = bt_exec_stmt(v, end, prefix); }
        }
        return r;
    }
    if (bt_is_ident_tok(v, v->pos, "while")) {
        v->pos++;
        bt_expect(v, "(");
        int cond_begin = v->pos;
        bt_val c = bt_expr(v, 0);
        bt_expect(v, ")");
        int body = v->pos;
        int r = EX_NORMAL;
        for (;;) {
            bt_tick(v); if (v->failed) return EX_STOP;
            if (!bt_vi(c)) break;
            v->pos = body;
            r = bt_exec_stmt(v, end, prefix);
            if (r & EX_BREAK) break;
            if (r & EX_STOP) return EX_STOP;
            v->pos = cond_begin;
            c = bt_expr(v, 0);
            bt_expect(v, ")");      /* body starts after ')' */
            if (v->failed) return EX_STOP;
        }
        return (r & EX_BREAK) ? EX_NORMAL : r;
    }
    if (bt_is_ident_tok(v, v->pos, "for")) {
        v->pos++;
        bt_expect(v, "(");
        if (!bt_is_punct(v, v->pos, ";")) bt_exec_stmt(v, end, prefix); /* init */
        else v->pos++;
        int cond_begin = v->pos;
        bt_val c = bt_is_punct(v, v->pos, ";") ? bt_iv(1) : bt_expr(v, 0);
        bt_expect(v, ";");
        int post_begin = v->pos;
        /* find the end of the post expression token range */
        int d = 0, post_end = v->pos;
        while (post_end < end) {
            if (bt_is_punct(v, post_end, "(")) d++;
            else if (bt_is_punct(v, post_end, ")")) { if (d == 0) break; d--; }
            post_end++;
        }
        bt_expect(v, ")");
        int body = v->pos;
        int r = EX_NORMAL;
        for (;;) {
            bt_tick(v); if (v->failed) return EX_STOP;
            if (!bt_vi(c)) break;
            v->pos = body;
            r = bt_exec_stmt(v, end, prefix);
            if (r & EX_BREAK) break;
            if (r & EX_STOP) return EX_STOP;
            /* post expression */
            v->pos = post_begin;
            if (post_begin < post_end) bt_expr(v, 0);
            if (v->failed) return EX_STOP;
            v->pos = cond_begin;
            c = bt_is_punct(v, v->pos, ";") ? bt_iv(1) : bt_expr(v, 0);
            bt_expect(v, ";");      /* keep cursor before post expression */
            if (v->failed) return EX_STOP;
        }
        return (r & EX_BREAK) ? EX_NORMAL : r;
    }

    if (bt_is_ident_tok(v, v->pos, "struct") || bt_is_ident_tok(v, v->pos, "enum") ||
        bt_is_ident_tok(v, v->pos, "typedef")) {
        /* struct / enum / typedef definitions: parse and register, no bytes read */
        bool is_typedef = bt_is_ident_tok(v, v->pos, "typedef");
        if (is_typedef) v->pos++;
        bool is_enum = bt_is_ident_tok(v, v->pos, "enum");
        bool is_struct = bt_is_ident_tok(v, v->pos, "struct");
        if (is_enum || is_struct) {
            v->pos++;
            char tname[64] = "";
            if (v->pos < v->n && v->tok[v->pos].kind == TK_IDENT) {
                snprintf(tname, sizeof tname, "%s", v->tok[v->pos].text); v->pos++;
            }
            if (!bt_is_punct(v, v->pos, "{")) { bt_fail(v, "定义缺少 '{'"); return EX_STOP; }
            v->pos++;
            int begin = v->pos;
            int d = 1;
            while (v->pos < v->n && d > 0) {
                if (bt_is_punct(v, v->pos, "{")) d++;
                else if (bt_is_punct(v, v->pos, "}")) d--;
                v->pos++;
            }
            int stop = v->pos - 1;
            if (is_enum) {
                int64_t ev = 0;
                char en[BT_NAME_CAP];
                for (int i = begin; i < stop && !v->failed; ) {
                    if (v->tok[i].kind != TK_IDENT) { i++; continue; }
                    snprintf(en, sizeof en, "%s", v->tok[i].text); i++;
                    if (i < stop && bt_is_punct(v, i, "=")) {
                        int sv = v->pos; v->pos = i + 1;
                        bt_val e = bt_expr(v, 0);
                        i = v->pos; v->pos = sv;
                        ev = bt_vi(e);
                    }
                    bt_sym_set(v, en, ev++);
                    if (i < stop && bt_is_punct(v, i, ",")) i++;
                }
            } else {
                if (v->naliases < (int)(sizeof v->aliases / sizeof *v->aliases) && tname[0]) {
                    bt_struct *s = (bt_struct *)calloc(1, sizeof *s);
                    if (!s) { bt_fail(v, "内存不足"); return EX_STOP; }
                    snprintf(s->name, sizeof s->name, "%s", tname);
                    s->begin = begin; s->end = stop;
                    s->next = v->structs; v->structs = s;
                    bt_alias *al = &v->aliases[v->naliases++];
                    memset(al, 0, sizeof *al);
                    snprintf(al->name, sizeof al->name, "%s", tname);
                    al->st = s;
                }
            }
            if (bt_is_punct(v, v->pos, ";")) v->pos++;
            return EX_NORMAL;
        }
        /* typedef SCALAR Alias; */
        if (v->pos < v->n && v->tok[v->pos].kind == TK_IDENT) {
            char base[64], alias[64];
            snprintf(base, sizeof base, "%s", v->tok[v->pos++].text);
            if (v->pos < v->n && v->tok[v->pos].kind == TK_IDENT) {
                snprintf(alias, sizeof alias, "%s", v->tok[v->pos++].text);
                if (v->naliases < (int)(sizeof v->aliases / sizeof *v->aliases)) {
                    bt_alias *al = &v->aliases[v->naliases++];
                    memset(al, 0, sizeof *al);
                    snprintf(al->name, sizeof al->name, "%s", alias);
                    snprintf(al->base, sizeof al->base, "%s", base);
                }
            }
        }
        while (v->pos < end && !bt_is_punct(v, v->pos, ";")) v->pos++;
        if (bt_is_punct(v, v->pos, ";")) v->pos++;
        return EX_NORMAL;
    }

    /* typed declaration */
    if (t->kind == TK_IDENT) {
        bt_type ty;
        if (bt_resolve_type(v, t->text, &ty) && v->pos + 1 < end &&
            v->tok[v->pos + 1].kind == TK_IDENT) {
            v->pos++;
            bt_exec_decl(v, &ty, prefix);
            return v->failed ? EX_STOP : EX_NORMAL;
        }
        /* assignment or call statement */
        if (v->pos + 1 < end &&
            (bt_is_punct(v, v->pos + 1, "=") || bt_is_punct(v, v->pos + 1, "+=") ||
             bt_is_punct(v, v->pos + 1, "-=") || bt_is_punct(v, v->pos + 1, "*=") ||
             bt_is_punct(v, v->pos + 1, "/=") || bt_is_punct(v, v->pos + 1, "%=") ||
             bt_is_punct(v, v->pos + 1, "&=") || bt_is_punct(v, v->pos + 1, "|=") ||
             bt_is_punct(v, v->pos + 1, "^=") || bt_is_punct(v, v->pos + 1, "<<=") ||
             bt_is_punct(v, v->pos + 1, ">>="))) {
            bt_assignment(v);
            return v->failed ? EX_STOP : EX_NORMAL;
        }
        /* bare identifier: could be ++/-- or a function call statement */
        bt_expr(v, 0);
        if (!v->failed && bt_is_punct(v, v->pos, ";")) v->pos++;
        return v->failed ? EX_STOP : EX_NORMAL;
    }

    bt_fail(v, "无法解析的语句（'%s'）", t->text);
    v->pos++;
    return EX_STOP;
}

static int bt_exec_block(bt_vm *v, int end, const char *prefix) {
    int r = EX_NORMAL;
    while (v->pos < end && !v->failed) {
        r = bt_exec_stmt(v, end, prefix);
        if (r & (EX_BREAK | EX_CONTINUE | EX_STOP)) break;
    }
    return r;
}

/* ---------------- public API ---------------- */

bool np_bt_execute(const char *script, const uint8_t *data, size_t size,
                   np_bt_result *out) {
    if (!out) return false;
    memset(out, 0, sizeof *out);
    out->ok = false;
    if (!script || !*script) {
        snprintf(out->error, sizeof out->error, "模板脚本为空");
        return false;
    }
    if (strlen(script) > BT_MAX_SOURCE) {
        snprintf(out->error, sizeof out->error, "模板脚本过大（上限 %u 字节）", BT_MAX_SOURCE);
        return false;
    }

    bt_vm vm;
    memset(&vm, 0, sizeof vm);
    vm.data = data ? data : (const uint8_t *)"";
    vm.size = data ? size : 0;
    vm.out = out;
    vm.parent = -1;            /* top-level fields have no parent */
    bt_tokens toks;
    memset(&toks, 0, sizeof toks);
    if (!bt_lex(script, &toks, out->error, sizeof out->error)) {
        free(toks.t);
        return false;
    }
    vm.tok = toks.t; vm.n = toks.n; vm.pos = 0;

    const char *prefix = "";
    bt_exec_block(&vm, vm.n, prefix);

    if (vm.failed) snprintf(out->error, sizeof out->error, "%s", vm.error);
    else out->ok = true;

    for (bt_struct *s = vm.structs; s; ) { bt_struct *nx = s->next; free(s); s = nx; }
    free(vm.syms);
    free(toks.t);
    if (!out->ok && out->fields) {
        free(out->fields);
        out->fields = NULL; out->count = out->cap = 0;
    }
    return out->ok;
}

void np_bt_result_free(np_bt_result *out) {
    if (!out) return;
    free(out->fields);
    out->fields = NULL; out->count = out->cap = 0;
}

bool np_bt_validate(const char *script, char *err, size_t err_cap) {
    static uint8_t zeros[4096];
    np_bt_result r;
    bool ok = np_bt_execute(script, zeros, sizeof zeros, &r);
    if (!ok && err && err_cap) snprintf(err, err_cap, "%s", r.error);
    np_bt_result_free(&r);
    return ok;
}
