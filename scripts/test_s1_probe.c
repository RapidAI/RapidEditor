/* test_s1_probe.c — S1: H9/R1/F1 统一探测与路由单元测试. */
#include "np_probe.h"
#include "np_integrity.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { printf("  [FAIL] %s:%d %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

/* 构造一个最小 ELF64 LE x86-64 头 */
static void make_elf64_x64(unsigned char *b, size_t n) {
    memset(b, 0, n);
    memcpy(b, "\x7F""ELF", 4);
    b[4] = 2;  /* ELFCLASS64 */
    b[5] = 1;  /* LE */
    b[6] = 1;  /* EV_CURRENT */
    b[0x10] = 2; b[0x11] = 0; /* ET_EXEC */
    b[0x12] = 0x3E;           /* EM_X86_64 */
    /* entry @0x18 = 0x401000 */
    unsigned char e[8] = {0x00,0x10,0x40,0x00,0x00,0x00,0x00,0x00};
    memcpy(b + 0x18, e, 8);
    (void)n;
}

/* 构造最小 PE32+ x64 头 */
static void make_pe_x64(unsigned char *b, size_t n) {
    memset(b, 0, n);
    b[0] = 'M'; b[1] = 'Z';
    /* e_lfanew = 0x80 */
    unsigned char lf[4] = {0x80, 0x00, 0x00, 0x00};
    memcpy(b + 0x3C, lf, 4);
    memcpy(b + 0x80, "PE\0\0", 4);
    /* machine = 0x8664 */
    b[0x84] = 0x64; b[0x85] = 0x86;
    /* OptionalHeader ImageBase @ PE+0x18+8 = 0x80+0x20 = 0xA0, value 0x140000000 */
    unsigned char ib[8] = {0x00,0x00,0x00,0x40,0x01,0x00,0x00,0x00};
    memcpy(b + 0xA0, ib, 8);
}

/* 构造最小 Mach-O 64 LE */
static void make_macho_arm64(unsigned char *b, size_t n) {
    memset(b, 0, n);
    unsigned char m[4] = {0xCF,0xFA,0xED,0xFE}; /* LE 0xFEEDFACF */
    memcpy(b, m, 4);
    /* cputype = 12 (ARM) @4 LE */
    b[4] = 12;
    (void)n;
}

/* 构造一个最小 ZIP（含 [Content_Types].xml 本地头） */
static size_t make_ooxml(unsigned char *b, size_t cap, const char *part, const char *extra) {
    memset(b, 0, cap);
    size_t off = 0;
    /* 前置部件（可选，如 word/document.xml） */
    if (extra) {
        size_t el = strlen(extra);
        size_t need = 30 + el + 4;
        if (off + need > cap) return 0;
        b[off]= 'P'; b[off+1]='K'; b[off+2]=3; b[off+3]=4;
        b[off+26] = (unsigned char)(el & 0xFF);
        b[off+27] = (unsigned char)(el >> 8);
        memcpy(b + off + 30, extra, el);
        off += off + need > cap ? 0 : need;
    }
    size_t pl = strlen(part);
    size_t need = 30 + pl + 2;
    if (off + need > cap) return 0;
    b[off]= 'P'; b[off+1]='K'; b[off+2]=3; b[off+3]=4;
    b[off+26] = (unsigned char)(pl & 0xFF);
    b[off+27] = (unsigned char)(pl >> 8);
    memcpy(b + off + 30, part, pl);
    off += need;
    /* EOCD */
    b[off]= 'P'; b[off+1]='K'; b[off+2]=5; b[off+3]=6;
    off += 22;
    return off;
}

static int test_elf(void) {
    printf("== R1: ELF x86-64 ==\n");
    unsigned char b[128];
    make_elf64_x64(b, sizeof(b));
    np_prog_info_t p;
    CHECK(np_probe_program(b, sizeof(b), &p) == true);
    CHECK(p.arch == NP_PARCH_X64);
    CHECK(p.bits == 64);
    CHECK(p.endian == NP_ENDIAN_LITTLE);
    CHECK(p.entry == 0x401000);
    CHECK(p.is_pie == false);
    return 0;
}

static int test_elf_pie_mips_be(void) {
    printf("== R1: ELF MIPS BE PIE (龙芯旧 MIPS 区分) ==\n");
    unsigned char b[64];
    memset(b, 0, sizeof(b));
    memcpy(b, "\x7F""ELF", 4);
    b[4] = 1;  /* 32-bit */
    b[5] = 2;  /* BE */
    b[0x10] = 3; b[0x11] = 0; /* ET_DYN */
    b[0x12] = 0x08; b[0x13] = 0x00; /* EM_MIPS */
    np_prog_info_t p;
    CHECK(np_probe_program(b, sizeof(b), &p));
    CHECK(p.arch == NP_PARCH_MIPS);
    CHECK(p.endian == NP_ENDIAN_BIG);
    CHECK(p.is_pie == true);
    /* LoongArch e_machine=0x62=98 → 0x62 LE 编码 b[0x12]=0x62 */
    memset(b, 0, sizeof(b));
    memcpy(b, "\x7F""ELF", 4);
    b[4] = 2; b[5] = 1;
    b[0x12] = 0x62;
    CHECK(np_probe_program(b, sizeof(b), &p));
    CHECK(p.arch == NP_PARCH_LOONGARCH);
    return 0;
}

static int test_pe_macho(void) {
    printf("== R1: PE x64 + Mach-O arm64 ==\n");
    unsigned char b[256];
    make_pe_x64(b, sizeof(b));
    np_prog_info_t p;
    CHECK(np_probe_program(b, sizeof(b), &p));
    CHECK(p.arch == NP_PARCH_X64);
    CHECK(p.bits == 64);
    CHECK(p.image_base == 0x140000000);

    make_macho_arm64(b, sizeof(b));
    CHECK(np_probe_program(b, sizeof(b), &p));
    CHECK(p.arch == NP_PARCH_AARCH64);
    CHECK(p.bits == 64);
    return 0;
}

static int test_format(void) {
    printf("== F1: 格式识别 ==\n");
    unsigned char b[512];
    np_fmt_info_t f;

    /* SQLite 魔数 */
    memset(b, 0, sizeof(b));
    memcpy(b, "SQLite format 3\0", 16);
    CHECK(np_probe_format(b, 64, "data.xyz", &f));
    CHECK(f.kind == NP_FMT_SQLITE);

    /* OOXML: ZIP + [Content_Types].xml + word/document.xml */
    size_t n = make_ooxml(b, sizeof(b), "[Content_Types].xml", "word/document.xml");
    CHECK(n > 0);
    CHECK(np_probe_format(b, n, "f.docx", &f));
    CHECK(f.kind == NP_FMT_OOXML);
    CHECK(f.ooxml_creator == 1);

    /* 纯 ZIP（无 OPC 特征） */
    n = make_ooxml(b, sizeof(b), "random/part.txt", NULL);
    CHECK(np_probe_format(b, n, "f.zip", &f));
    CHECK(f.kind == NP_FMT_ZIP_PLAIN);

    /* PDF */
    memcpy(b, "%PDF-1.7\n", 9);
    CHECK(np_probe_format(b, 32, "f.pdf", &f));
    CHECK(f.kind == NP_FMT_PDF);
    CHECK(strncmp(f.subtype, "1.7", 3) == 0);

    /* PNG */
    memcpy(b, "\x89PNG\r\n\x1a\n", 8);
    CHECK(np_probe_format(b, 32, "x", &f));
    CHECK(f.kind == NP_FMT_PNG);
    return 0;
}

static int test_routing(void) {
    printf("== H9: 统一路由 ==\n");
    unsigned char b[128];
    np_probe_result_t r;

    /* 空文件 → TEXT */
    np_probe(NULL, 0, "", &r);
    CHECK(r.route == NP_ROUTE_TEXT);

    /* ELF → PROGRAM */
    make_elf64_x64(b, sizeof(b));
    np_probe(b, sizeof(b), "x.bin", &r);
    CHECK(r.route == NP_ROUTE_PROGRAM);
    CHECK(r.track == NP_TRACK_BINARY);
    CHECK(r.confidence >= 90);

    /* SQLite → FORMAT */
    memset(b, 0, sizeof(b));
    memcpy(b, "SQLite format 3\0", 16);
    np_probe(b, 64, "a.db", &r);
    CHECK(r.route == NP_ROUTE_FORMAT);
    CHECK(r.track == NP_TRACK_FORMAT);

    /* 文本 → TEXT */
    const char *txt = "hello world\nsecond line\n";
    np_probe((const unsigned char *)txt, strlen(txt), "a.txt", &r);
    CHECK(r.route == NP_ROUTE_TEXT);
    CHECK(r.likely_text || r.confidence >= 70);

    /* 二进制乱码 → HEX + 线索 */
    unsigned char junk[64];
    for (int i = 0; i < 64; ++i) junk[i] = (unsigned char)(i * 7 + 3);
    junk[0] = 0x01; junk[1] = 0x9F;
    np_probe(junk, sizeof(junk), "junk.bin", &r);
    CHECK(r.route == NP_ROUTE_HEX);
    CHECK(r.clue[0] != '\0');

    /* XML 文本标注 */
    const char *xml = "<?xml version=\"1.0\"?><root/>";
    np_probe((const unsigned char *)xml, strlen(xml), "c.xml", &r);
    CHECK(r.route == NP_ROUTE_TEXT);
    CHECK(r.fmt.kind == NP_FMT_XML);
    return 0;
}

static int test_zip_helper(void) {
    printf("== F1: ZIP 部件判定 ==\n");
    unsigned char b[512];
    size_t ps = 0;
    size_t n = make_ooxml(b, sizeof(b), "[Content_Types].xml", NULL);
    CHECK(np_probe_zip_contains(b, n, "[Content_Types].xml", &ps));
    CHECK(np_probe_zip_contains(b, n, "no/such.xml", &ps) == false);
    return 0;
}

int main(void) {
    test_elf();
    test_elf_pie_mips_be();
    test_pe_macho();
    test_format();
    test_routing();
    test_zip_helper();
    if (failures == 0) printf("ALL PROBE TESTS PASSED\n");
    else               printf("%d PROBE TEST FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}
