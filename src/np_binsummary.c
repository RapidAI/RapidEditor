/* np_binsummary.c — R4: 符号 / 字符串 / 导入表摘要（实现）.
 * 纯 C99、UI-free、只读；ELF32/64 dynsym+symtab、PE 导入目录、可打印字符串。
 */
#include "np_binsummary.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- 小端/大端读取 ---------- */
static uint16_t rd16(const unsigned char *p, bool le)
{
    if (le) return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}
static uint32_t rd32(const unsigned char *p, bool le)
{
    if (le) return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                  ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static uint64_t rd64(const unsigned char *p, bool le)
{
    if (le) return (uint64_t)rd32(p, true) | ((uint64_t)rd32(p + 4, true) << 32);
    return ((uint64_t)rd32(p, false) << 32) | (uint64_t)rd32(p + 4, false);
}

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

static bool range_ok(size_t len, uint64_t off, uint64_t size)
{
    return off < len && size <= len - off;
}

const char *np_bs_status_name(np_bs_status_t s)
{
    switch (s) {
    case NP_BS_OK: return "OK";
    case NP_BS_ERR_ARG: return "ARG";
    case NP_BS_ERR_NOTPROG: return "NOTPROG";
    case NP_BS_ERR_PARSE: return "PARSE";
    case NP_BS_ERR_FULL: return "FULL";
    }
    return "?";
}

const char *np_bs_format_name(np_bs_format_t f)
{
    return f == NP_BS_PE ? "PE" : "ELF";
}

static void make_ref(char *out, size_t cap, const char *source, uint64_t off)
{
    (void)snprintf(out, cap, "%s:0x%llx", source ? source : "(unknown)",
                   (unsigned long long)off);
}

/* ---------- 可打印字符串提取 ---------- */
static void extract_strings(const unsigned char *d, size_t len,
                            np_bs_result_t *out)
{
    size_t i = 0, run = 0, start = 0;
    while (i <= len) {
        unsigned char c = (i < len) ? d[i] : 0;
        bool printable = (c >= 0x20 && c < 0x7f);
        if (printable && run < NP_BS_MAX_STRLEN - 1) {
            if (run == 0) start = i;
            run++;
        } else {
            if (run >= NP_BS_MIN_STRLEN && out->nstrings < NP_BS_MAX_STRINGS) {
                np_bs_string_t *s = &out->strings[out->nstrings++];
                size_t k;
                memset(s, 0, sizeof(*s));
                s->offset = start;
                s->len = run;
                for (k = 0; k < run && k + 1 < sizeof(s->text); k++)
                    s->text[k] = (char)d[start + k];
                s->text[k < sizeof(s->text) ? k : sizeof(s->text) - 1] = '\0';
                s->truncated = (run >= NP_BS_MAX_STRLEN - 1);
                make_ref(s->ref, sizeof(s->ref), out->source, start);
            } else if (out->nstrings >= NP_BS_MAX_STRINGS) {
                out->strings_truncated = true;
            }
            run = 0;
        }
        i++;
    }
}

/* ---------- ELF ---------- */
typedef struct {
    bool le;
    bool is64;
    uint16_t etype;
    uint16_t machine;
    uint64_t shoff;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
} elf_hdr_t;

static np_bs_sym_bind_t elf_bind(unsigned char info)
{
    unsigned b = info >> 4;
    if (b == 1) return NP_BS_SYM_GLOBAL;
    if (b == 2) return NP_BS_SYM_WEAK;
    if (b == 0) return NP_BS_SYM_LOCAL;
    return NP_BS_SYM_OTHER;
}

static void add_elf_symbol(const unsigned char *d, size_t len,
                           const elf_hdr_t *eh, uint64_t stroff, uint64_t strsize,
                           uint64_t symoff, size_t symsize, size_t idx,
                           np_bs_result_t *out)
{
    uint64_t nameoff;
    uint64_t value;
    unsigned char info, other;
    uint16_t shndx;
    const char *nm;
    np_bs_symbol_t *s;
    if (!range_ok(len, symoff, symsize)) return;
    if (eh->is64) {
        const unsigned char *p = d + symoff;
        nameoff = rd32(p, eh->le);
        info = p[4]; other = p[5];
        shndx = rd16(p + 6, eh->le);
        value = rd64(p + 8, eh->le);
    } else {
        const unsigned char *p = d + symoff;
        nameoff = rd32(p, eh->le);
        value = rd32(p + 4, eh->le);
        info = p[12]; other = p[13];
        shndx = rd16(p + 14, eh->le);
    }
    (void)other;
    if (nameoff >= strsize || !range_ok(len, stroff, strsize)) return;
    nm = (const char *)d + stroff + nameoff;
    if (!*nm) return;  /* 空名跳过 */
    if (out->nsymbols >= NP_BS_MAX_SYMBOLS) { out->symbols_truncated = true; return; }
    s = &out->symbols[out->nsymbols++];
    memset(s, 0, sizeof(*s));
    s->bind = elf_bind(info);
    s->undefined = (shndx == 0);
    s->is_func = ((info & 0x0f) == 2);
    s->value = value;
    fscpy(s->name, sizeof(s->name), nm);
    make_ref(s->ref, sizeof(s->ref), out->source, symoff);
    (void)idx;
}

static np_bs_status_t parse_elf(const unsigned char *d, size_t len,
                                np_bs_result_t *out)
{
    elf_hdr_t eh;
    uint64_t shoff, strtab_off = 0, strtab_size = 0;
    uint64_t symtab_off = 0, symtab_size = 0;
    uint64_t dynsym_off = 0, dynsym_size = 0, dynstr_off = 0, dynstr_size = 0;
    uint16_t shentsize, shnum, shstrndx;
    size_t i;
    bool le;
    if (len < 52 || d[5] != 1) return NP_BS_ERR_NOTPROG;  /* EI_DATA: 1=LE,2=BE */
    le = (d[5] == 1);
    eh.le = le;
    eh.is64 = (d[4] == 2);
    if (!eh.is64 && d[4] != 1) return NP_BS_ERR_NOTPROG;
    eh.etype = rd16(d + 16, le);
    eh.machine = rd16(d + 18, le);
    if (eh.is64) {
        if (len < 64) return NP_BS_ERR_PARSE;
        shoff = rd64(d + 40, le);
        shentsize = rd16(d + 58, le);
        shnum = rd16(d + 60, le);
        shstrndx = rd16(d + 62, le);
    } else {
        shoff = rd32(d + 32, le);
        shentsize = rd16(d + 46, le);
        shnum = rd16(d + 48, le);
        shstrndx = rd16(d + 50, le);
    }
    if (!shoff || !shentsize || !shnum) return NP_BS_OK; /* stripped，无节头 */
    if (!range_ok(len, shoff, (uint64_t)shentsize * shnum)) return NP_BS_ERR_PARSE;
    /* 遍历节头表，按类型识别：2=SYMTAB 11=DYNSYM 3=STRTAB，关联 sh_link */
    for (i = 0; i < shnum; i++) {
        const unsigned char *sh = d + shoff + (uint64_t)i * shentsize;
        uint32_t type, link;
        uint64_t offset, size, entsize;
        if (eh.is64) {
            type = rd32(sh + 4, le);
            offset = rd64(sh + 24, le);
            size = rd64(sh + 32, le);
            link = rd32(sh + 40, le);
            entsize = rd64(sh + 56, le);
        } else {
            type = rd32(sh + 4, le);
            offset = rd32(sh + 16, le);
            size = rd32(sh + 20, le);
            link = rd32(sh + 24, le);
            entsize = rd32(sh + 36, le);
        }
        (void)shstrndx;
        if (type == 3 && link == 0) { /* 第一个独立 strtab 兜底 */
            if (!strtab_size) { strtab_off = offset; strtab_size = size; }
        }
        if (type == 11) { dynsym_off = offset; dynsym_size = size;
                          if (link < shnum) {
                              const unsigned char *ls = d + shoff + (uint64_t)link * shentsize;
                              dynstr_off = eh.is64 ? rd64(ls + 24, le) : rd32(ls + 16, le);
                              dynstr_size = eh.is64 ? rd64(ls + 32, le) : rd32(ls + 20, le);
                          }
                          if (!entsize) { /* 取默认 */
                          }
                        }
        if (type == 2) { symtab_off = offset; symtab_size = size;
                         if (!strtab_size && link < shnum) {
                             const unsigned char *ls = d + shoff + (uint64_t)link * shentsize;
                             strtab_off = eh.is64 ? rd64(ls + 24, le) : rd32(ls + 16, le);
                             strtab_size = eh.is64 ? rd64(ls + 32, le) : rd32(ls + 20, le);
                         }
                       }
    }
    /* 解析 dynsym（含 undefined=导入）+ symtab */
    {
        size_t es = eh.is64 ? 24 : 16;
        if (dynsym_off && dynsym_size && dynstr_size && es) {
            size_t cnt = (size_t)(dynsym_size / es), k;
            if (cnt > NP_BS_MAX_SYMBOLS * 2) cnt = NP_BS_MAX_SYMBOLS * 2;
            for (k = 0; k < cnt; k++)
                add_elf_symbol(d, len, &eh, dynstr_off, dynstr_size,
                               dynsym_off + (uint64_t)k * es, es, k, out);
        }
        if (symtab_off && symtab_size && strtab_size && es) {
            size_t cnt = (size_t)(symtab_size / es), k;
            if (cnt > NP_BS_MAX_SYMBOLS * 2) cnt = NP_BS_MAX_SYMBOLS * 2;
            for (k = 0; k < cnt; k++)
                add_elf_symbol(d, len, &eh, strtab_off, strtab_size,
                               symtab_off + (uint64_t)k * es, es, k, out);
        }
    }
    /* ELF 的 undefined 符号作为导入，lib 记为 ELF:<动态依赖未知> */
    for (i = 0; i < (size_t)out->nsymbols; i++) {
        const np_bs_symbol_t *s = &out->symbols[i];
        if (!s->undefined || out->nimports >= NP_BS_MAX_IMPORTS) continue;
        {
            np_bs_import_t *im = &out->imports[out->nimports++];
            memset(im, 0, sizeof(*im));
            fscpy(im->name, sizeof(im->name), s->name);
            fscpy(im->lib, sizeof(im->lib), "ELF:<dynamic>");
            fscpy(im->ref, sizeof(im->ref), s->ref);
        }
    }
    if (out->nsymbols >= NP_BS_MAX_SYMBOLS || out->nimports >= NP_BS_MAX_IMPORTS)
        out->symbols_truncated = true;
    out->format = NP_BS_ELF;
    out->bits = eh.is64 ? 64 : 32;
    /* arch from e_machine (简表) */
    switch (eh.machine) {
    case 3:  out->arch = NP_PARCH_X86; break;
    case 62: out->arch = NP_PARCH_X64; break;
    case 40: out->arch = NP_PARCH_ARM; break;
    case 183: out->arch = NP_PARCH_AARCH64; break;  /* 0xB7 EM_AARCH64 */
    case 8:  out->arch = NP_PARCH_MIPS; break;
    case 258: out->arch = NP_PARCH_LOONGARCH; break;
    case 243: out->arch = NP_PARCH_RISCV; break;
    default: out->arch = NP_PARCH_UNKNOWN; break;
    }
    return NP_BS_OK;
}

/* ---------- PE ---------- */
static np_bs_status_t parse_pe(const unsigned char *d, size_t len,
                               np_bs_result_t *out)
{
    uint32_t lfanew, optsz, import_rva, import_sz;
    uint16_t machine, magic;
    uint32_t image_base, section_cnt;
    uint64_t opt_off, sec_off;
    uint32_t ndirs;
    /* RVA→文件偏移所需节表 */
    typedef struct { uint32_t va, vsize, raw_off, raw_size; } sec_t;
    sec_t secs[96];
    uint32_t ns = 0, i;
    if (len < 64 || d[0] != 'M' || d[1] != 'Z') return NP_BS_ERR_NOTPROG;
    lfanew = rd32(d + 0x3c, true);
    if (!range_ok(len, lfanew, 24) || memcmp(d + lfanew, "PE\0\0", 4) != 0)
        return NP_BS_ERR_NOTPROG;
    machine = rd16(d + lfanew + 4, true);
    section_cnt = rd16(d + lfanew + 6, true);
    optsz = rd16(d + lfanew + 20, true);
    opt_off = (uint64_t)lfanew + 24;
    if (!range_ok(len, opt_off, optsz)) return NP_BS_ERR_PARSE;
    magic = rd16(d + opt_off, true);
    if (magic == 0x20b) { image_base = rd32(d + opt_off + 24, true); ndirs = rd32(d + opt_off + 108, true); }
    else if (magic == 0x10b) { image_base = rd32(d + opt_off + 28, true); ndirs = rd32(d + opt_off + 92, true); }
    else return NP_BS_ERR_NOTPROG;
    sec_off = opt_off + optsz;
    if (section_cnt > 96 || !range_ok(len, sec_off, (uint64_t)section_cnt * 40))
        return NP_BS_ERR_PARSE;
    for (i = 0; i < section_cnt; i++) {
        const unsigned char *sh = d + sec_off + (uint64_t)i * 40;
        secs[i].vsize = rd32(sh + 8, true);
        secs[i].va = rd32(sh + 12, true);
        secs[i].raw_size = rd32(sh + 16, true);
        secs[i].raw_off = rd32(sh + 20, true);
    }
    ns = section_cnt;
    /* 数据目录 1 = 导入表；PE32+ 偏移 112+8*1，PE32 偏移 96+8*1 */
    {
        uint64_t dd = opt_off + (magic == 0x20b ? 112 : 96) + 8;
        if (ndirs < 2 || !range_ok(len, dd, 8)) return NP_BS_OK;
        import_rva = rd32(d + dd, true);
        import_sz = rd32(d + dd + 4, true);
    }
    (void)image_base; (void)import_sz;
    if (!import_rva) return NP_BS_OK; /* 无导入表 */
    /* RVA→offset helper */
#define RVA2OFF(rva, outoff) do { \
        (outoff) = SIZE_MAX; \
        for (i = 0; i < ns; i++) { \
            uint32_t sva = secs[i].va, svs = secs[i].vsize ? secs[i].vsize : secs[i].raw_size; \
            if ((rva) >= sva && (uint64_t)(rva) - sva < svs) { \
                uint64_t o = (uint64_t)secs[i].raw_off + ((rva) - sva); \
                if (o < len) { (outoff) = (size_t)o; } break; \
            } \
        } \
    } while (0)
    {
        size_t doff;
        RVA2OFF(import_rva, doff);
        if (doff == SIZE_MAX) return NP_BS_OK;
        /* IMAGE_IMPORT_DESCRIPTOR: 20 字节，全 0 结束；OriginalFirstThunk@0, Name@12, FirstThunk@16 */
        for (i = 0; i < 256; i++, doff += 20) {
            uint32_t oft, name_rva, ft;
            uint64_t noff;
            char lib[NP_BS_MAX_NAME];
            if (!range_ok(len, doff, 20)) break;
            oft = rd32(d + doff, true);
            name_rva = rd32(d + doff + 12, true);
            ft = rd32(d + doff + 16, true);
            if (!oft && !name_rva && !ft) break;  /* 终止描述符 */
            lib[0] = '\0';
            if (name_rva) {
                RVA2OFF(name_rva, noff);
                if (noff != SIZE_MAX && noff < len)
                    fscpy(lib, sizeof(lib), (const char *)d + noff);
            }
            if (!lib[0]) fscpy(lib, sizeof(lib), "(unknown)");
            out->ndlls++;
            /* thunk 数组：PE32+=8 字节/项（高位表示 ordinal），PE32=4 */
            {
                uint32_t thunk_rva = oft ? oft : ft;
                size_t toff;
                size_t step = (magic == 0x20b) ? 8 : 4;
                RVA2OFF(thunk_rva, toff);
                if (toff != SIZE_MAX) {
                    size_t j;
                    for (j = 0; j < 512; j++, toff += step) {
                        uint64_t ent = (step == 8) ? rd64(d + toff, true)
                                                   : rd32(d + toff, true);
                        if (!ent) break;
                        if (!range_ok(len, toff, step)) break;
                        if (out->nimports >= NP_BS_MAX_IMPORTS) {
                            out->imports_truncated = true; break;
                        }
                        {
                            np_bs_import_t *im = &out->imports[out->nimports++];
                            memset(im, 0, sizeof(*im));
                            fscpy(im->lib, sizeof(im->lib), lib);
                            make_ref(im->ref, sizeof(im->ref), out->source,
                                     (uint64_t)toff);
                            if (ent & ((uint64_t)1 << (step * 8 - 1))) {
                                im->by_ordinal = true;
                                im->ordinal = (uint16_t)(ent & 0xffff);
                                (void)snprintf(im->name, sizeof(im->name),
                                               "#%u", im->ordinal);
                            } else {
                                uint32_t hint_rva = (uint32_t)(ent & 0x7fffffff);
                                size_t noff2;
                                RVA2OFF(hint_rva, noff2);
                                if (noff2 != SIZE_MAX && noff2 + 2 < len)
                                    fscpy(im->name, sizeof(im->name),
                                          (const char *)d + noff2 + 2); /* 跳 hint */
                                else
                                    fscpy(im->name, sizeof(im->name), "(unnamed)");
                            }
                        }
                    }
                }
            }
        }
    }
#undef RVA2OFF
    out->format = NP_BS_PE;
    out->bits = (magic == 0x20b) ? 64 : 32;
    switch (machine) {
    case 0x14c: out->arch = NP_PARCH_X86; break;
    case 0x8664: out->arch = NP_PARCH_X64; break;
    case 0x1c0: case 0x1c4: out->arch = NP_PARCH_ARM; break;
    case 0xaa64: out->arch = NP_PARCH_AARCH64; break;
    case 0x6264: out->arch = NP_PARCH_LOONGARCH; break;
    default: out->arch = NP_PARCH_UNKNOWN; break;
    }
    return NP_BS_OK;
}

/* ---------- 公共入口 ---------- */
np_bs_status_t np_bs_parse(const unsigned char *d, size_t len,
                           const char *source, np_bs_result_t *out)
{
    if (!d || !out || len < 16) return NP_BS_ERR_ARG;
    memset(out, 0, sizeof(*out));
    fscpy(out->source, sizeof(out->source), source ? source : "(unknown)");
    np_bs_status_t st;
    if (d[0] == 0x7f && d[1] == 'E' && d[2] == 'L' && d[3] == 'F')
        st = parse_elf(d, len, out);
    else if (d[0] == 'M' && d[1] == 'Z')
        st = parse_pe(d, len, out);
    else
        return NP_BS_ERR_NOTPROG;
    if (st == NP_BS_OK) extract_strings(d, len, out);
    return st;
}

np_bs_status_t np_bs_parse_file(const char *path, np_bs_result_t *out)
{
    FILE *f;
    long sz;
    unsigned char *buf;
    size_t got;
    np_bs_status_t st;
    if (!path || !out) return NP_BS_ERR_ARG;
#if defined(_MSC_VER)
    if (fopen_s(&f, path, "rb") != 0) return NP_BS_ERR_ARG;
#else
    f = fopen(path, "rb");
    if (!f) return NP_BS_ERR_ARG;
#endif
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NP_BS_ERR_ARG; }
    sz = ftell(f);
    if (sz < 16 || sz > 64 * 1024 * 1024) { fclose(f); return NP_BS_ERR_NOTPROG; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NP_BS_ERR_ARG; }
    buf = (unsigned char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NP_BS_ERR_FULL; }
    got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = '\0';
    st = np_bs_parse(buf, got, path, out);
    free(buf);
    return st;
}

int np_bs_digest(const np_bs_result_t *r, char *out, size_t cap)
{
    size_t u = 0;
    int i;
    if (!r || !out || cap < 64) return -1;
    if (!appendf(out, cap, &u, "# 二进制摘要 · %s\n格式 %s / %s / %d-bit\n\n",
        r->source, np_bs_format_name(r->format), np_parch_name(r->arch),
        r->bits)) return -1;
    if (!appendf(out, cap, &u, "## 符号（%d%s）\n", r->nsymbols,
        r->symbols_truncated ? "+，已截断" : "")) return -1;
    for (i = 0; i < r->nsymbols && i < 64; i++)
        if (!appendf(out, cap, &u, "- %s%s%s\n", r->symbols[i].name,
            r->symbols[i].undefined ? " [导入]" : "", r->symbols[i].ref)) return -1;
    if (!appendf(out, cap, &u, "\n## 导入（%d）\n", r->nimports)) return -1;
    for (i = 0; i < r->nimports && i < 64; i++)
        if (!appendf(out, cap, &u, "- %s!%s %s\n", r->imports[i].lib,
            r->imports[i].name, r->imports[i].ref)) return -1;
    if (!appendf(out, cap, &u, "\n## 字符串（%d%s）\n", r->nstrings,
        r->strings_truncated ? "+，已截断" : "")) return -1;
    for (i = 0; i < r->nstrings && i < 64; i++)
        if (!appendf(out, cap, &u, "- %s %s\n", r->strings[i].text,
            r->strings[i].ref)) return -1;
    return (int)u;
}

np_bs_status_t np_bs_ai_classify(np_bs_result_t *r, np_bs_classify_fn fn,
                                 void *user)
{
    char dg[NP_BS_MAX_OUT];
    char tmp[NP_BS_MAX_OUT];
    if (!r || !fn) return NP_BS_ERR_ARG;
    if (np_bs_digest(r, dg, sizeof(dg)) < 0) return NP_BS_ERR_FULL;
    tmp[0] = '\0';
    if (!fn(dg, tmp, sizeof(tmp), user) || !tmp[0]) return NP_BS_ERR_PARSE;
    fscpy(r->ai_summary, sizeof(r->ai_summary), tmp);
    r->ai_used = true;
    return NP_BS_OK;
}

int np_bs_render(const np_bs_result_t *r, char *out, size_t cap)
{
    size_t u = 0;
    int i;
    if (!r || !out || cap < 128) return -1;
    if (!appendf(out, cap, &u, "# 二进制摘要报告 · %s\n\n", r->source)) return -1;
    if (!appendf(out, cap, &u,
        "- 格式：%s · 架构：%s · %d-bit\n"
        "- 符号 %d / 导入 %d（%d 个来源）/ 字符串 %d%s\n\n",
        np_bs_format_name(r->format), np_parch_name(r->arch), r->bits,
        r->nsymbols, r->nimports, r->ndlls, r->nstrings,
        (r->symbols_truncated || r->imports_truncated || r->strings_truncated)
        ? " · 部分已截断" : "")) return -1;
    if (!appendf(out, cap, &u, "## 符号表（每条带 offset 引用）\n")) return -1;
    if (!r->nsymbols) {
        if (!appendf(out, cap, &u, "- 无符号（stripped 或无节头表）\n")) return -1;
    } else {
        for (i = 0; i < r->nsymbols && i < 80; i++) {
            const np_bs_symbol_t *s = &r->symbols[i];
            if (!appendf(out, cap, &u, "- %s [%s%s] 0x%llx · %s\n",
                s->name, s->undefined ? "导入" :
                         (s->is_func ? "函数" : "对象"),
                s->bind == NP_BS_SYM_WEAK ? "/weak" : "",
                (unsigned long long)s->value, s->ref)) return -1;
        }
    }
    if (!appendf(out, cap, &u, "\n## 导入函数（按来源分组）\n")) return -1;
    if (!r->nimports) {
        if (!appendf(out, cap, &u, "- 未发现静态导入表\n")) return -1;
    } else {
        for (i = 0; i < r->nimports && i < 80; i++) {
            const np_bs_import_t *im = &r->imports[i];
            if (!appendf(out, cap, &u, "- %s ! %s%s · %s\n",
                im->lib, im->name,
                im->by_ordinal ? " (ordinal)" : "", im->ref)) return -1;
        }
    }
    if (!appendf(out, cap, &u, "\n## 可打印字符串（长度≥%d）\n",
                 NP_BS_MIN_STRLEN)) return -1;
    for (i = 0; i < r->nstrings && i < 80; i++) {
        const np_bs_string_t *s = &r->strings[i];
        if (!appendf(out, cap, &u, "- 0x%lx: %s%s · %s\n",
            (unsigned long)s->offset, s->text,
            s->truncated ? "…" : "", s->ref)) return -1;
    }
    if (r->ai_used && !appendf(out, cap, &u, "\n## AI 分类摘要（注入回调，不可信输入）\n%s\n",
                               r->ai_summary)) return -1;
    if (!appendf(out, cap, &u,
        "\n> 只读解析：ELF 仅节头表存在的符号，PE 仅静态导入目录；"
        "字符串为长度阈值提取，未做语义分类。每条结论带 offset 引用供回跳核验。\n")) return -1;
    return (int)u;
}
