/* np_probe.c — S1: H9 统一探测路由 + R1 程序头 + F1 格式识别. */
#include "np_probe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* 名称表                                                               */
/* ------------------------------------------------------------------ */

const char *np_parch_name(np_parch_t a) {
    switch (a) {
    case NP_PARCH_X86: return "x86";
    case NP_PARCH_X64: return "x86-64";
    case NP_PARCH_ARM: return "ARM";
    case NP_PARCH_AARCH64: return "AArch64";
    case NP_PARCH_MIPS: return "MIPS (legacy)";
    case NP_PARCH_LOONGARCH: return "LoongArch";
    case NP_PARCH_RISCV: return "RISC-V";
    case NP_PARCH_POWERPC: return "PowerPC";
    case NP_PARCH_S390X: return "s390x";
    default: return "unknown";
    }
}

const char *np_route_name(np_route_t r) {
    switch (r) {
    case NP_ROUTE_TEXT: return "text";
    case NP_ROUTE_PROGRAM: return "program";
    case NP_ROUTE_FORMAT: return "format";
    default: return "hex";
    }
}

const char *np_format_name(np_format_kind_t k) {
    switch (k) {
    case NP_FMT_OOXML: return "OOXML (OPC)";
    case NP_FMT_ODF: return "OpenDocument";
    case NP_FMT_ZIP_PLAIN: return "ZIP archive";
    case NP_FMT_SQLITE: return "SQLite 3";
    case NP_FMT_PDF: return "PDF";
    case NP_FMT_PNG: return "PNG";
    case NP_FMT_JPEG: return "JPEG";
    case NP_FMT_GIF: return "GIF";
    case NP_FMT_BMP: return "BMP";
    case NP_FMT_ELF: return "ELF";
    case NP_FMT_PE: return "PE (MZ)";
    case NP_FMT_MACHO: return "Mach-O";
    case NP_FMT_XML: return "XML";
    case NP_FMT_JSON: return "JSON";
    case NP_FMT_TAR: return "tar";
    case NP_FMT_GZIP: return "gzip";
    case NP_FMT_OLE2: return "OLE2 / CFB";
    default: return "unknown";
    }
}

/* ------------------------------------------------------------------ */
/* R1 程序头解析                                                        */
/* ------------------------------------------------------------------ */

/* ELF: 0x7F 'E' 'L' 'F' | EI_CLASS(1=32,2=64) EI_DATA(1=LE,2=BE)
 * e_machine @18(32bit)/20(64bit, 但偏移因 class 而异——e_machine 都在偏移 0x12).
 * 实际布局：e_type@0x10, e_machine@0x12 对两种 class 相同。 */
static bool parse_elf(const unsigned char *d, size_t n, np_prog_info_t *o) {
    if (n < 20) return false;
    unsigned class = d[4], edata = d[5];
    unsigned etype = (unsigned)d[0x10] | ((unsigned)d[0x11] << 8);
    unsigned mach  = (unsigned)d[0x12] | ((unsigned)d[0x13] << 8);

    memset(o, 0, sizeof(*o));
    o->endian = (edata == 2) ? NP_ENDIAN_BIG : NP_ENDIAN_LITTLE;
    o->bits   = (class == 2) ? 64 : (class == 1 ? 32 : 0);
    o->is_pie = (etype == 3); /* ET_DYN */

    switch (mach) {
    case 0x03: o->arch = NP_PARCH_X86;  break;
    case 0x3E: o->arch = NP_PARCH_X64;  break;
    case 0x28: o->arch = NP_PARCH_ARM;  o->thumb_capable = true; break;
    case 0xB7: o->arch = NP_PARCH_AARCH64; break;
    case 0x08: o->arch = NP_PARCH_MIPS; break;   /* 旧 MIPS（龙芯早期 3A2000 前的指令集） */
    case 0xF3: o->arch = NP_PARCH_RISCV; break;
    case 0x14: o->arch = NP_PARCH_POWERPC; break;
    case 0x16: o->arch = NP_PARCH_S390X; break;
    case 0x62: o->arch = NP_PARCH_LOONGARCH; break; /* EM_LOONGARCH = 258 */
    default:   o->arch = NP_PARCH_UNKNOWN;  break;
    }

    /* e_entry @0x18：按 endian 手工组装，避免对齐与编译器差异 */
    if (o->bits == 64 && n >= 0x28) {
        uint64_t v = 0;
        if (o->endian == NP_ENDIAN_LITTLE) {
            for (int i = 7; i >= 0; --i) v = (v << 8) | d[0x18 + i];
        } else {
            for (int i = 0; i < 8; ++i) v = (v << 8) | d[0x18 + i];
        }
        o->entry = v;
        /* ET_EXEC 基址即入口段近似；ET_DYN 典型 0x400000/0x555555... 仅提示 */
        o->image_base = o->is_pie ? 0 : v;
    } else if (o->bits == 32 && n >= 0x1C) {
        uint32_t v = 0;
        if (o->endian == NP_ENDIAN_LITTLE) {
            for (int i = 3; i >= 0; --i) v = (v << 8) | d[0x18 + i];
        } else {
            for (int i = 0; i < 4; ++i) v = (v << 8) | d[0x18 + i];
        }
        o->entry = v;
        o->image_base = o->is_pie ? 0 : v;
    }
    return true;
}

/* PE: 'MZ' + e_lfanew @0x3C → 'PE\0\0' + COFF machine @+4 */
static bool parse_pe(const unsigned char *d, size_t n, np_prog_info_t *o) {
    if (n < 0x40) return false;
    uint32_t lfanew = 0;
    for (int i = 3; i >= 0; --i) lfanew = (lfanew << 8) | d[0x3C + i];
    if (lfanew == 0 || lfanew + 6 > n) return false;
    if (!(d[lfanew] == 'P' && d[lfanew+1] == 'E' && d[lfanew+2] == 0 && d[lfanew+3] == 0))
        return false;
    unsigned mach = (unsigned)d[lfanew+4] | ((unsigned)d[lfanew+5] << 8);
    memset(o, 0, sizeof(*o));
    o->endian = NP_ENDIAN_LITTLE;
    switch (mach) {
    case 0x014C: o->arch = NP_PARCH_X86; o->bits = 32; break;
    case 0x8664: o->arch = NP_PARCH_X64; o->bits = 64; break;
    case 0x01C4: o->arch = NP_PARCH_ARM; o->bits = 32; o->thumb_capable = true; break;
    case 0xAA64: o->arch = NP_PARCH_AARCH64; o->bits = 64; break;
    case 0x6264: o->arch = NP_PARCH_LOONGARCH; o->bits = 64; break; /* PE/LoongArch 已有移植 */
    case 0x01F0: o->arch = NP_PARCH_POWERPC; o->bits = 32; break;
    default:     o->arch = NP_PARCH_UNKNOWN; break;
    }
    /* ImageBase 在 OptionalHeader +24（PE32+）或 +28（PE32），简化：读 PE32+ 位置 */
    if (o->bits == 64 && lfanew + 0x30 <= n) {
        uint64_t base = 0;
        size_t ob = lfanew + 0x18 + 8; /* OptionalHeader.ImageBase offset for PE32+ = 24 */
        if (ob + 8 <= n) {
            for (int i = 7; i >= 0; --i) base = (base << 8) | d[ob + i];
            o->image_base = base;
        }
    }
    return true;
}

/* Mach-O: magic 0xCAFEBABE / 0xFEEDFACE / 0xFEEDFACF（LE 读出即字节序变换） */
static bool parse_macho(const unsigned char *d, size_t n, np_prog_info_t *o) {
    if (n < 16) return false;
    uint32_t m = 0;
    for (int i = 3; i >= 0; --i) m = (m << 8) | d[i]; /* LE 组装 */
    memset(o, 0, sizeof(*o));
    switch (m) {
    case 0xFEEDFACE: o->bits = 32; o->endian = NP_ENDIAN_LITTLE; break;
    case 0xFEEDFACF: o->bits = 64; o->endian = NP_ENDIAN_LITTLE; break;
    case 0xBEBAFECA: o->bits = 32; o->endian = NP_ENDIAN_BIG; break;
    case 0xCFFAEDFE: o->bits = 64; o->endian = NP_ENDIAN_BIG; break;
    default: return false;
    }
    /* cputype @4 (LE/BE 按 endian) */
    uint32_t cpu = 0;
    if (o->endian == NP_ENDIAN_LITTLE) {
        for (int i = 7; i >= 4; --i) cpu = (cpu << 8) | d[i];
    } else {
        for (int i = 4; i <= 7; ++i) cpu = (cpu << 8) | d[i];
    }
    switch (cpu & 0x00FFFFFF) {
    case 7:  o->arch = NP_PARCH_X86; if (o->bits == 64) o->arch = NP_PARCH_X64; break;
    case 12: o->arch = NP_PARCH_ARM; if (o->bits == 64) o->arch = NP_PARCH_AARCH64; break;
    default: o->arch = NP_PARCH_UNKNOWN; break;
    }
    return true;
}

bool np_probe_program(const unsigned char *data, size_t len, np_prog_info_t *out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!data || len < 16) return false;
    if (data[0] == 0x7F && data[1] == 'E' && data[2] == 'L' && data[3] == 'F')
        return parse_elf(data, len, out);
    if (data[0] == 'M' && data[1] == 'Z')
        return parse_pe(data, len, out);
    return parse_macho(data, len, out);
}

/* ------------------------------------------------------------------ */
/* ZIP 辅助（F1 容器判定）                                              */
/* ------------------------------------------------------------------ */

static uint16_t rd16le(const unsigned char *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t rd32le(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool np_probe_zip_contains(const unsigned char *d, size_t n,
                           const char *part_name, size_t *part_size_out) {
    if (!d || n < 4 || !part_name) return false;
    const size_t scan = n < 65536 ? n : 65536;
    size_t plen = strlen(part_name);
    /* 扫描本地文件头 50 4B 03 04 */
    for (size_t i = 0; i + 34 <= scan; ++i) {
        if (d[i] != 'P' || d[i+1] != 'K' || d[i+2] != 3 || d[i+3] != 4) continue;
        uint16_t namelen = rd16le(d + i + 26);
        uint32_t usize   = rd32le(d + i + 18);
        if (i + 30 + namelen > n) continue;
        if ((size_t)namelen == plen &&
            memcmp(d + i + 30, part_name, plen) == 0) {
            if (part_size_out) *part_size_out = usize;
            return true;
        }
        /* 只判首个相关块即可，继续扫以容错前导垃圾 */
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* F1 格式识别                                                          */
/* ------------------------------------------------------------------ */

static bool sniff_text(const unsigned char *d, size_t n, double *ratio_out) {
    size_t printable = 0, ctrl_other = 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = d[i];
        if (c == '\t' || c == '\r' || c == '\n' || (c >= 0x20 && c != 0x7F)) printable++;
        else if (c < 0x09 || (c > 0x0D && c < 0x20)) ctrl_other++;
    }
    size_t total = printable + ctrl_other;
    if (!total) { *ratio_out = 0; return false; }
    *ratio_out = (double)printable / (double)total;
    return *ratio_out >= 0.95;
}

static void ooxml_subtype(const unsigned char *d, size_t n, np_fmt_info_t *o) {
    if (np_probe_zip_contains(d, n, "word/document.xml", NULL)) {
        o->ooxml_creator = 1; snprintf(o->subtype, sizeof(o->subtype), "%s", "word");
    } else if (np_probe_zip_contains(d, n, "xl/workbook.xml", NULL)) {
        o->ooxml_creator = 2; snprintf(o->subtype, sizeof(o->subtype), "%s", "excel");
    } else if (np_probe_zip_contains(d, n, "ppt/presentation.xml", NULL)) {
        o->ooxml_creator = 3; snprintf(o->subtype, sizeof(o->subtype), "%s", "powerpoint");
    }
    o->encrypted = np_probe_zip_contains(d, n, "EncryptedPackage", NULL);
}

static bool odf_mimetype(const unsigned char *d, size_t n, char *out, size_t outn) {
    /* ODF 把 mimetype 作为第一个本地头条目且 stored */
    if (n < 38) return false;
    if (!(d[0]=='P' && d[1]=='K' && d[2]==3 && d[3]==4)) return false;
    uint16_t namelen = rd16le(d + 26);
    uint16_t extralen = rd16le(d + 28);
    if (namelen < 20 || (size_t)30 + namelen > n) return false;
    if (memcmp(d + 30, "mimetype", 8) != 0) return false;
    size_t off = 30 + namelen + extralen;
    uint16_t clen = rd16le(d + 20);
    if (off + clen > n || clen >= outn) return false;
    memcpy(out, d + off, clen);
    out[clen] = '\0';
    return true;
}

bool np_probe_format(const unsigned char *d, size_t n,
                     const char *filename_hint, np_fmt_info_t *out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!d || n < 4) return false;
    (void)filename_hint; /* 扩展名不可信（文档 F1 明确），仅留接口 */

    if (d[0]=='P' && d[1]=='K') {
        /* OOXML 双判定：ZIP + [Content_Types].xml（OPC 规范强制部件） */
        if (np_probe_zip_contains(d, n, "[Content_Types].xml", NULL)) {
            out->kind = NP_FMT_OOXML;
            ooxml_subtype(d, n, out);
            return true;
        }
        char mime[64];
        if (odf_mimetype(d, n, mime, sizeof(mime))) {
            out->kind = NP_FMT_ODF;
            snprintf(out->subtype, sizeof(out->subtype), "%s", mime);
            out->encrypted = np_probe_zip_contains(d, n, "encryption.xml", NULL);
            return true;
        }
        out->kind = NP_FMT_ZIP_PLAIN;
        return true;
    }
    if (n >= 16 && memcmp(d, "SQLite format 3\0", 16) == 0) {
        out->kind = NP_FMT_SQLITE;
        return true;
    }
    if (n >= 5 && memcmp(d, "%PDF-", 5) == 0) {
        out->kind = NP_FMT_PDF;
        snprintf(out->subtype, sizeof(out->subtype), "%.8s", (const char *)d + 5); /* 版本号 */
        return true;
    }
    if (n >= 8 && memcmp(d, "\x89PNG\r\n\x1a\n", 8) == 0) { out->kind = NP_FMT_PNG;  return true; }
    if (n >= 3 && d[0]==0xFF && d[1]==0xD8 && d[2]==0xFF) { out->kind = NP_FMT_JPEG; return true; }
    if (n >= 6 && (memcmp(d, "GIF87a", 6) == 0 || memcmp(d, "GIF89a", 6) == 0)) {
        out->kind = NP_FMT_GIF; return true;
    }
    if (n >= 2 && d[0]=='B' && d[1]=='M') { out->kind = NP_FMT_BMP; return true; }
    if (n >= 8 && memcmp(d, "\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8) == 0) {
        out->kind = NP_FMT_OLE2; return true;
    }
    if (n >= 2 && d[0]==0x1F && d[1]==0x8B) { out->kind = NP_FMT_GZIP; return true; }
    if (n >= 262 && memcmp(d + 257, "ustar", 5) == 0) { out->kind = NP_FMT_TAR; return true; }
    return false;
}

/* ------------------------------------------------------------------ */
/* H9 统一路由                                                          */
/* ------------------------------------------------------------------ */

void np_probe(const unsigned char *d, size_t n, const char *filename_hint,
              np_probe_result_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!d || n == 0) {
        out->route = NP_ROUTE_TEXT;
        out->track = NP_TRACK_TEXT;
        out->confidence = 100;
        snprintf(out->clue, sizeof(out->clue), "%s", "empty file");
        return;
    }

    /* BOM 检测 */
    if (n >= 3 && d[0]==0xEF && d[1]==0xBB && d[2]==0xBF) out->has_bom = true;
    if (n >= 2 && ((d[0]==0xFF && d[1]==0xFE) || (d[0]==0xFE && d[1]==0xFF))) out->has_bom = true;

    /* 1) 程序头优先（R1）：ELF/PE/Mach-O 明确 → 程序轨 */
    np_prog_info_t prog;
    if (np_probe_program(d, n, &prog)) {
        out->route = NP_ROUTE_PROGRAM;
        out->track = NP_TRACK_BINARY;
        out->prog  = prog;
        out->confidence = 95;
        snprintf(out->clue, sizeof(out->clue), "%s header: %s %s %d-bit",
                 d[0] == 0x7F ? "ELF" : (d[0] == 'M' ? "PE" : "Mach-O"),
                 np_parch_name(prog.arch), prog.endian ? "BE" : "LE", prog.bits);
        return;
    }

    /* 2) 数据格式（F1）：魔数 + 容器双判定 */
    np_fmt_info_t fmt;
    if (np_probe_format(d, n, filename_hint, &fmt)) {
        out->route = NP_ROUTE_FORMAT;
        out->track = NP_TRACK_FORMAT;
        out->fmt   = fmt;
        out->confidence = 90;
        snprintf(out->clue, sizeof(out->clue), "%s%s%s", np_format_name(fmt.kind),
                 fmt.subtype[0] ? "/" : "", fmt.subtype);
        return;
    }

    /* 3) 文本回退：可打印率 ≥95% → 文本轨 */
    double ratio = 0;
    bool is_text = sniff_text(d, n, &ratio);
    out->printable_ratio = ratio;
    if (is_text || out->has_bom) {
        out->route = NP_ROUTE_TEXT;
        out->track = NP_TRACK_TEXT;
        out->confidence = out->has_bom ? 85 : 75;
        /* 4) 文本中的结构化格式：XML/JSON 线索 */
        if (n >= 5 && d[0]=='<' && (memcmp(d, "<?xml", 5) == 0 || d[1] != '!')) {
            out->fmt.kind = NP_FMT_XML;
            snprintf(out->clue, sizeof(out->clue), "%s", "text (XML content)");
        } else {
            snprintf(out->clue, sizeof(out->clue), "%s", "plain text (printable ratio ok)");
        }
        return;
    }

    /* 5) 未识别 → 保持 Hex 并给线索 */
    out->route = NP_ROUTE_HEX;
    out->track = NP_TRACK_BINARY;
    out->confidence = 30;
    snprintf(out->clue, sizeof(out->clue),
             "unrecognized binary, printable=%.0f%%, first bytes %02X %02X %02X %02X",
             ratio * 100.0, d[0], d[1],
             n > 2 ? d[2] : 0, n > 3 ? d[3] : 0);
}
