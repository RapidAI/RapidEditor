/* np_byteintel.c — H3 字节解释 + H4 结构化字节搜索（只读）. 见 np_byteintel.h. */
#include "np_byteintel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <inttypes.h>

/* ---- H3 类型元数据 --------------------------------------------------- */

const char *np_bi_type_name(np_bi_type_t t) {
    switch (t) {
        case NP_BI_U8: return "u8";
        case NP_BI_I8: return "i8";
        case NP_BI_U16LE: return "u16le"; case NP_BI_U16BE: return "u16be";
        case NP_BI_I16LE: return "i16le"; case NP_BI_I16BE: return "i16be";
        case NP_BI_U32LE: return "u32le"; case NP_BI_U32BE: return "u32be";
        case NP_BI_I32LE: return "i32le"; case NP_BI_I32BE: return "i32be";
        case NP_BI_U64LE: return "u64le"; case NP_BI_U64BE: return "u64be";
        case NP_BI_F32LE: return "f32le"; case NP_BI_F64LE: return "f64le";
        case NP_BI_ASCII: return "ascii";
        case NP_BI_IPV4: return "ipv4";
        case NP_BI_MAC: return "mac";
        case NP_BI_GUID: return "guid";
        default: return "?";
    }
}

const char *np_bi_type_endian(np_bi_type_t t) {
    switch (t) {
        case NP_BI_U16LE: case NP_BI_I16LE:
        case NP_BI_U32LE: case NP_BI_I32LE:
        case NP_BI_U64LE: case NP_BI_F32LE: case NP_BI_F64LE: return "little";
        case NP_BI_U16BE: case NP_BI_I16BE:
        case NP_BI_U32BE: case NP_BI_I32BE:
        case NP_BI_U64BE: return "big";
        default: return "";
    }
}

static uint16_t rd16(const unsigned char *p, bool le) {
    return le ? (uint16_t)(p[0] | ((uint16_t)p[1] << 8))
              : (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}
static uint32_t rd32(const unsigned char *p, bool le) {
    return le ? ((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                 ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24))
              : (((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                 ((uint32_t)p[2] << 8) | (uint32_t)p[3]);
}
static uint64_t rd64(const unsigned char *p, bool le) {
    uint64_t v = 0;
    if (le) for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    else    for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
    return v;
}

/* 把一个字段写入结果（带上限保护）。 */
static np_bi_field_t *add_field(np_bi_result_t *out, np_bi_type_t t,
                                size_t off, size_t size, bool plausible) {
    if (!out || out->nfields >= NP_BI_MAX_FIELDS) return NULL;
    np_bi_field_t *f = &out->fields[out->nfields++];
    memset(f, 0, sizeof *f);
    f->type = t; f->offset = off; f->size = size; f->plausible = plausible;
    return f;
}

void np_bi_interpret(const unsigned char *data, size_t len,
                     size_t offset, size_t length, np_bi_result_t *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!data || offset >= len) return;
    if (length == 0 || length > len - offset) length = len - offset;
    /* 只解释开头若干字节，避免超长选区产生无意义候选 */
    size_t avail = length > 16 ? 16 : length;
    const unsigned char *p = data + offset;

    /* 整数 / 浮点候选 */
    if (avail >= 1) {
        np_bi_field_t *f = add_field(out, NP_BI_U8, offset, 1, true);
        if (f) snprintf(f->value, sizeof f->value, "%u", (unsigned)p[0]);
        f = add_field(out, NP_BI_I8, offset, 1, true);
        if (f) snprintf(f->value, sizeof f->value, "%d", (int)(int8_t)p[0]);
    }
    if (avail >= 2) {
        static const np_bi_type_t ts[4] = { NP_BI_U16LE, NP_BI_U16BE, NP_BI_I16LE, NP_BI_I16BE };
        for (int k = 0; k < 4; k++) {
            bool le = (ts[k] == NP_BI_U16LE || ts[k] == NP_BI_I16LE);
            bool sg = (ts[k] == NP_BI_I16LE || ts[k] == NP_BI_I16BE);
            uint16_t u = rd16(p, le);
            np_bi_field_t *f = add_field(out, ts[k], offset, 2, true);
            if (f) {
                if (sg) snprintf(f->value, sizeof f->value, "%d", (int)(int16_t)u);
                else    snprintf(f->value, sizeof f->value, "%u", (unsigned)u);
            }
        }
    }
    if (avail >= 4) {
        static const np_bi_type_t ts[4] = { NP_BI_U32LE, NP_BI_U32BE, NP_BI_I32LE, NP_BI_I32BE };
        for (int k = 0; k < 4; k++) {
            bool le = (ts[k] == NP_BI_U32LE || ts[k] == NP_BI_I32LE);
            bool sg = (ts[k] == NP_BI_I32LE || ts[k] == NP_BI_I32BE);
            uint32_t u = rd32(p, le);
            np_bi_field_t *f = add_field(out, ts[k], offset, 4, true);
            if (f) {
                if (sg) snprintf(f->value, sizeof f->value, "%" PRId32, (int32_t)u);
                else    snprintf(f->value, sizeof f->value, "%" PRIu32, u);
            }
        }
        /* f32le：用 memcpy 规避严格别名，有限数才算 plausible */
        uint32_t ub = rd32(p, true);
        float fl;
        memcpy(&fl, &ub, sizeof fl);
        np_bi_field_t *ff = add_field(out, NP_BI_F32LE, offset, 4, isfinite(fl));
        if (ff) snprintf(ff->value, sizeof ff->value, "%.6g", (double)fl);
    }
    if (avail >= 8) {
        uint64_t u = rd64(p, true);
        np_bi_field_t *f = add_field(out, NP_BI_U64LE, offset, 8, true);
        if (f) snprintf(f->value, sizeof f->value, "%" PRIu64, u);
        f = add_field(out, NP_BI_U64BE, offset, 8, true);
        if (f) { uint64_t ub = rd64(p, false); snprintf(f->value, sizeof f->value, "%" PRIu64, ub); }
        double dv;
        memcpy(&dv, p, sizeof dv);   /* LE 主机假设：文件已是 LE 字节序 */
        f = add_field(out, NP_BI_F64LE, offset, 8, isfinite(dv));
        if (f) snprintf(f->value, sizeof f->value, "%.10g", dv);
    }
    /* ipv4 / mac / guid：固定宽度，标签值 */
    if (avail >= 4) {
        np_bi_field_t *f = add_field(out, NP_BI_IPV4, offset, 4, true);
        if (f) snprintf(f->value, sizeof f->value, "%u.%u.%u.%u",
                        p[0], p[1], p[2], p[3]);
    }
    if (avail >= 6) {
        np_bi_field_t *f = add_field(out, NP_BI_MAC, offset, 6, true);
        if (f) snprintf(f->value, sizeof f->value,
                        "%02X:%02X:%02X:%02X:%02X:%02X",
                        p[0], p[1], p[2], p[3], p[4], p[5]);
    }
    if (avail >= 16) {
        np_bi_field_t *f = add_field(out, NP_BI_GUID, offset, 16, true);
        if (f) snprintf(f->value, sizeof f->value,
                        "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                        p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7],
                        p[8],p[9],p[10],p[11],p[12],p[13],p[14],p[15]);
    }
    /* ascii：整段可打印才 plausible */
    {
        size_t m = length < NP_BI_VALUE_CAP - 1 ? length : NP_BI_VALUE_CAP - 1;
        bool printable = m > 0;
        for (size_t i = 0; i < m; i++) {
            unsigned char c = p[i];
            if (!((c >= 0x20 && c <= 0x7E) || c == '\t' || c == '\n' || c == '\r')) {
                printable = false; break;
            }
        }
        np_bi_field_t *f = add_field(out, NP_BI_ASCII, offset, m, printable);
        if (f) {
            size_t j = 0;
            for (; j < m && j + 1 < sizeof f->value; j++)
                f->value[j] = (p[j] >= 0x20 && p[j] <= 0x7E) ? (char)p[j] : '.';
            f->value[j] = 0;
        }
    }
}

/* ---- H4：查询解析 ---------------------------------------------------- */

static void qerr(char *err, size_t cap, const char *msg) {
    if (err && cap) snprintf(err, cap, "%s", msg);
}

static bool ci_eq(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    return true;
}

/* 解析无符号/有符号整数字面量（十进制或 0x 十六进制，可带负号）。 */
static bool parse_int(const char *s, size_t n, bool *neg, uint64_t *mag) {
    if (!n) return false;
    *neg = false;
    const char *p = s;
    if (*p == '-' || *p == '+') { *neg = (*p == '-'); p++; n--; }
    if (!n) return false;
    int base = 10;
    if (n >= 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; n -= 2; }
    if (!n) return false;
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        int d;
        unsigned char c = (unsigned char)p[i];
        if (c >= '0' && c <= '9') d = c - '0';
        else if (base == 16 && c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (base == 16 && c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        uint64_t nv = v * (uint64_t)base + (uint64_t)d;
        if (nv < v) return false;   /* 溢出 */
        v = nv;
    }
    *mag = v;
    return true;
}

/* 解析整数查询：前缀如 u32le / i16be，后接比较符 + 值。 */
static bool parse_uint_query(const char *text, size_t n, np_bi_query_t *q,
                             char *err, size_t err_cap) {
    if (n < 3) return false;
    bool signed_val = (text[0] == 'i' || text[0] == 'I');
    if (text[0] != 'u' && text[0] != 'U' && !signed_val) return false;
    /* 找到第一个比较符位置；其前的串是类型（u32le 等），其后是比较值。 */
    size_t tpos = 1;
    while (tpos < n && text[tpos] != '=' && text[tpos] != '>' && text[tpos] != '<') tpos++;
    if (tpos >= n) { qerr(err, err_cap, "缺少比较符（= > >= < <=）"); return false; }
    char tail[8];
    size_t tn = tpos - 1;   /* text[1..tpos) */
    if (tn == 0 || tn >= sizeof tail) { qerr(err, err_cap, "未知整数类型"); return false; }
    for (size_t i = 0; i < tn; i++) tail[i] = (char)tolower((unsigned char)text[1+i]);
    tail[tn] = 0;
    bool le = false;
    int width = 0;
    if (strcmp(tail, "8") == 0) { width = 1; }
    else if (strcmp(tail, "16le") == 0) { width = 2; le = true; }
    else if (strcmp(tail, "16be") == 0) { width = 2; le = false; }
    else if (strcmp(tail, "32le") == 0) { width = 4; le = true; }
    else if (strcmp(tail, "32be") == 0) { width = 4; le = false; }
    else if (strcmp(tail, "64le") == 0) { width = 8; le = true; }
    else if (strcmp(tail, "64be") == 0) { width = 8; le = false; }
    else { qerr(err, err_cap, "未知整数类型（支持 u8/u16le/u16be/u32le/u32be/u64le/u64be 及 i 变体）"); return false; }

    /* 比较符：支持 >= <= > < = */
    size_t pos = tpos;
    np_bi_cmp_t cmp = NP_BI_CMP_EQ;
    if (text[pos] == '>') { cmp = NP_BI_CMP_GT; pos++; if (pos < n && text[pos] == '=') { cmp = NP_BI_CMP_GE; pos++; } }
    else if (text[pos] == '<') { cmp = NP_BI_CMP_LT; pos++; if (pos < n && text[pos] == '=') { cmp = NP_BI_CMP_LE; pos++; } }
    else if (text[pos] == '=') { cmp = NP_BI_CMP_EQ; pos++; }
    if (pos >= n) { qerr(err, err_cap, "缺少比较值"); return false; }

    bool neg = false; uint64_t mag = 0;
    if (!parse_int(text + pos, n - pos, &neg, &mag)) { qerr(err, err_cap, "数值格式不对"); return false; }
    uint64_t mask = width == 8 ? UINT64_MAX : ((uint64_t)1 << (width * 8)) - 1;
    uint64_t val = mag & mask;
    if (neg) val = (~mag + 1) & mask;   /* 补码 */

    q->kind = (cmp == NP_BI_CMP_EQ) ? NP_BIQ_UINT_EQ :
              (cmp == NP_BI_CMP_GT) ? NP_BIQ_UINT_GT :
              (cmp == NP_BI_CMP_GE) ? NP_BIQ_UINT_GE :
              (cmp == NP_BI_CMP_LT) ? NP_BIQ_UINT_LT : NP_BIQ_UINT_LE;
    q->width = width; q->big_endian = !le; q->signed_val = signed_val;
    q->cmp = cmp; q->value = val;
    {
        const char *op = cmp == NP_BI_CMP_EQ ? "=" : cmp == NP_BI_CMP_GT ? ">" :
                         cmp == NP_BI_CMP_GE ? ">=" : cmp == NP_BI_CMP_LT ? "<" : "<=";
        snprintf(q->desc, sizeof q->desc, "%c%d%s %s %s%" PRIu64,
                 signed_val ? 'i' : 'u', width * 8, le ? "le" : "be",
                 op, neg ? "-" : "", mag);
    }
    return true;
}

/* 已知签名表（字节 + 长度）。 */
typedef struct { const char *name; const char *bytes; size_t n; } sig_def_t;
static const sig_def_t SIGS[] = {
    { "elf", "\x7F\x45\x4C\x46", 4 },
    { "pe",  "MZ", 2 },
    { "jpg", "\xFF\xD8\xFF", 3 },
    { "png", "\x89PNG\r\n\x1A\n", 8 },
    { "gif", "GIF8", 4 },
    { "zip", "PK\x03\x04", 4 },
    { "bmp", "BM", 2 }
};

static bool parse_hex_byte(const char *s, size_t n, unsigned char *out) {
    if (n == 0 || n > 2) return false;
    unsigned v = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i]; int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        v = v * 16 + (unsigned)d;
    }
    *out = (unsigned char)v;
    return true;
}

bool np_bi_query_parse(const char *text, np_bi_query_t *out,
                       char *err, size_t err_cap) {
    if (err && err_cap) err[0] = 0;
    if (!text || !out || !*text) { qerr(err, err_cap, "查询为空"); return false; }
    memset(out, 0, sizeof *out);
    size_t n = strlen(text);
    if (n > NP_BI_QUERY_CAP) { qerr(err, err_cap, "查询过长"); return false; }

    /* 整数查询：以 u/i 开头且含比较符（u8 等也 ≥2 字符）。 */
    if ((text[0] == 'u' || text[0] == 'U' || text[0] == 'i' || text[0] == 'I') &&
        strpbrk(text, "=<>"))
        return parse_uint_query(text, n, out, err, err_cap);

    /* SIG：elf/pe/jpg/png/gif/zip/bmp 精确匹配 */
    for (size_t i = 0; i < sizeof SIGS / sizeof *SIGS; i++) {
        size_t sl = strlen(SIGS[i].name);
        if (n == sl && ci_eq(text, SIGS[i].name, sl)) {
            out->kind = NP_BIQ_SIG;
            memset(out->sig, 0, sizeof out->sig);
            memcpy(out->sig, SIGS[i].bytes, SIGS[i].n);
            out->sig_len = SIGS[i].n;
            snprintf(out->desc, sizeof out->desc, "签名 %s", SIGS[i].name);
            return true;
        }
    }

    /* 带冒号的扩展：png:CHUNK / fill:0xNN:minrun / str:text */
    const char *colon = strchr(text, ':');
    if (colon) {
        size_t head = (size_t)(colon - text);
        const char *arg = colon + 1;
        size_t argn = strlen(arg);
        if (head == 3 && ci_eq(text, "png", 3)) {
            if (argn == 0 || argn > 4) { qerr(err, err_cap, "PNG 块类型应为 1-4 个 ASCII 字符（如 IHDR）"); return false; }
            for (size_t i = 0; i < argn; i++)
                if (!((arg[i] >= 'A' && arg[i] <= 'Z') || (arg[i] >= 'a' && arg[i] <= 'z')))
                    { qerr(err, err_cap, "PNG 块类型只能含字母"); return false; }
            out->kind = NP_BIQ_CHUNK;
            memset(out->sig, 0, sizeof out->sig);
            memcpy(out->sig, arg, argn);
            out->sig_len = argn;
            snprintf(out->desc, sizeof out->desc, "PNG 块 %s", arg);
            return true;
        }
        if (head == 4 && ci_eq(text, "fill", 4)) {
            const char *c2 = strchr(arg, ':');
            if (!c2) { qerr(err, err_cap, "格式 fill:0xNN:minrun"); return false; }
            size_t bn = (size_t)(c2 - arg);
            const char *bstr = arg;
            if (bn >= 2 && arg[0] == '0' && (arg[1] == 'x' || arg[1] == 'X')) {
                bstr += 2; bn -= 2;   /* 支持 0x 前缀 */
            }
            unsigned char fb = 0;
            if (!parse_hex_byte(bstr, bn, &fb)) { qerr(err, err_cap, "填充字节格式不对（如 00 或 FF）"); return false; }
            const char *rn = c2 + 1;
            if (!*rn) { qerr(err, err_cap, "缺少最小连续长度"); return false; }
            bool neg; uint64_t mag;
            if (!parse_int(rn, strlen(rn), &neg, &mag) || neg || mag == 0 || mag > 0xFFFFFFFFu) {
                qerr(err, err_cap, "最小连续长度应为正整数"); return false;
            }
            out->kind = NP_BIQ_FILL; out->fill_byte = fb; out->min_run = (size_t)mag;
            snprintf(out->desc, sizeof out->desc, "填充 0x%02X × %llu", fb,
                     (unsigned long long)mag);
            return true;
        }
        if (head == 3 && ci_eq(text, "str", 3)) {
            if (argn == 0) { qerr(err, err_cap, "str: 后需文本"); return false; }
            if (argn >= NP_BI_STR_CAP) { qerr(err, err_cap, "文本过长"); return false; }
            out->kind = NP_BIQ_STR;
            memcpy(out->str, arg, argn); out->str[argn] = 0;
            snprintf(out->desc, sizeof out->desc, "文本 \"%s\"", out->str);
            return true;
        }
        qerr(err, err_cap, "未知查询前缀（支持 png:/fill:/str:）");
        return false;
    }

    qerr(err, err_cap, "无法识别的查询");
    return false;
}

/* ---- H4：本地扫描 ---------------------------------------------------- */

static bool cmp_ok(uint64_t v, uint64_t target, np_bi_cmp_t cmp,
                   bool signed_val, int width) {
    /* 有符号比较：按位宽符号扩展到 int64_t 再比较。 */
    if (signed_val) {
        int shift = 64 - width * 8;
        int64_t sv = (int64_t)(v << shift) >> shift;
        int64_t tv = (int64_t)(target << shift) >> shift;
        switch (cmp) {
            case NP_BI_CMP_EQ: return sv == tv;
            case NP_BI_CMP_GT: return sv > tv;
            case NP_BI_CMP_GE: return sv >= tv;
            case NP_BI_CMP_LT: return sv < tv;
            case NP_BI_CMP_LE: return sv <= tv;
        }
    }
    switch (cmp) {
        case NP_BI_CMP_EQ: return v == target;
        case NP_BI_CMP_GT: return v > target;
        case NP_BI_CMP_GE: return v >= target;
        case NP_BI_CMP_LT: return v < target;
        case NP_BI_CMP_LE: return v <= target;
    }
    return false;
}

static void push_hit(np_bi_hit_t *hits, int cap, int *kept, int *total,
                     size_t off, size_t len) {
    if (total) (*total)++;
    if (hits && *kept < cap) {
        hits[*kept].offset = off;
        hits[*kept].length = len;
        (*kept)++;
    }
}

int np_bi_search(const unsigned char *data, size_t len,
                 const np_bi_query_t *q,
                 np_bi_hit_t *hits, int cap, int *total) {
    int kept = 0;
    if (total) *total = 0;
    if (!data || !q || len == 0) return 0;
    if (cap < 0) cap = 0;

    switch (q->kind) {
        case NP_BIQ_UINT_EQ: case NP_BIQ_UINT_GT: case NP_BIQ_UINT_GE:
        case NP_BIQ_UINT_LT: case NP_BIQ_UINT_LE: {
            int w = q->width;
            if (w <= 0 || w > 8 || len < (size_t)w) break;
            for (size_t i = 0; i + (size_t)w <= len; i++) {
                uint64_t v = w == 1 ? data[i] : w == 2 ? rd16(data + i, !q->big_endian)
                           : w == 4 ? rd32(data + i, !q->big_endian)
                                    : rd64(data + i, !q->big_endian);
                if (cmp_ok(v, q->value, q->cmp, q->signed_val, w))
                    push_hit(hits, cap, &kept, total, i, (size_t)w);
                if (total && *total >= 100000) break;   /* 防御：超密命中截断扫描 */
            }
            break;
        }
        case NP_BIQ_SIG: case NP_BIQ_CHUNK: {
            size_t sl = q->sig_len;
            if (sl == 0 || sl > 8 || len < sl) break;
            for (size_t i = 0; i + sl <= len; i++) {
                if (memcmp(data + i, q->sig, sl) == 0)
                    push_hit(hits, cap, &kept, total, i, sl);
            }
            break;
        }
        case NP_BIQ_FILL: {
            size_t run = q->min_run;
            if (run == 0 || len < run) break;
            size_t i = 0;
            while (i + run <= len) {
                if (data[i] != q->fill_byte) { i++; continue; }
                size_t j = i + 1;
                while (j < len && data[j] == q->fill_byte) j++;
                size_t seglen = j - i;
                if (seglen >= run)
                    push_hit(hits, cap, &kept, total, i, seglen);
                i = j;
            }
            break;
        }
        case NP_BIQ_STR: {
            size_t sl = strlen(q->str);
            if (sl == 0 || sl > NP_BI_STR_CAP || len < sl) break;
            for (size_t i = 0; i + sl <= len; i++) {
                if (memcmp(data + i, q->str, sl) == 0)
                    push_hit(hits, cap, &kept, total, i, sl);
            }
            break;
        }
    }
    return kept;
}
