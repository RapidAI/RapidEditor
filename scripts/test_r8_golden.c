/* test_r8_golden.c - R8 multi-architecture regression gate.
 *
 * Locks down two things that must not silently regress:
 *   1. objdump text parsing on each ISA's real output format
 *      (x86 space-separated bytes vs fixed-width ISAs' contiguous hex words).
 *   2. R1 architecture recognition on golden program headers (ELF/PE/Mach-O).
 *
 * The golden .txt fixtures are capture-style samples of objdump -D output;
 * they are NOT trusted decodes — they only lock the PARSER contract. Real
 * instruction correctness still needs a Capstone/objdump toolchain (documented
 * honestly in the R2 report).
 */
#include "np_disasm.h"
#include "np_probe.h"
#include <stdio.h>
#include <string.h>

static int checks = 0, fails = 0;
#define CHECK(x) do { ++checks; if (!(x)) { \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #x); ++fails; } } while (0)

/* Parse every instruction line from a golden file, invoking cb per line.
 * Returns the number of parsed instruction lines (non-instruction lines are
 * skipped). */
typedef void (*line_fn)(const np_disasm_insn_t *ins, void *user);
static int scan_golden(const char *path, line_fn cb, void *user) {
    FILE *f = fopen(path, "rb");
    if (!f) { printf("FAIL cannot open %s\n", path); ++fails; return 0; }
    char line[512];
    int parsed = 0;
    while (fgets(line, sizeof line, f)) {
        np_disasm_insn_t ins;
        if (!np_disasm_parse_objdump_line(line, &ins)) continue;
        ++parsed;
        if (cb) cb(&ins, user);
    }
    fclose(f);
    return parsed;
}

/* Expectation record for one golden instruction. */
typedef struct {
    uint64_t addr;
    size_t   size;
    const char *text_frag;
} expect_t;

typedef struct {
    const expect_t *ex;
    size_t n, i;
} expect_ctx;

static void check_expect(const np_disasm_insn_t *ins, void *user) {
    expect_ctx *c = (expect_ctx *)user;
    if (c->i >= c->n) {
        printf("FAIL extra instruction line: [%s]\n", ins->text); ++fails; return;
    }
    const expect_t *e = &c->ex[c->i++];
    CHECK(ins->addr == e->addr);
    CHECK(ins->size == e->size);
    CHECK(strstr(ins->text, e->text_frag) != NULL);
    CHECK(ins->source == NP_DSRC_OBJDUMP);
}

static void test_x64(void) {
    static const expect_t ex[] = {
        { 0x401000, 5, "mov" }, { 0x401005, 4, "add" },
        { 0x401009, 1, "ret" }, { 0x40100a, 1, "nop" }
    };
    expect_ctx c = { ex, 4, 0 };
    CHECK(scan_golden("tests/golden/disasm_x64.txt", check_expect, &c) == 4);
    CHECK(c.i == 4);
}

static void test_arm(void) {
    /* Contiguous 8-digit word = 4 bytes. Thumb 4-digit word = 2 bytes. */
    static const expect_t ex[] = {
        { 0x10000, 4, "ldr" }, { 0x10004, 4, "bx" }, { 0x10008, 4, "mov" }
    };
    expect_ctx c = { ex, 3, 0 };
    CHECK(scan_golden("tests/golden/disasm_arm.txt", check_expect, &c) == 3);
    CHECK(c.i == 3);
}

static void test_aarch64(void) {
    static const expect_t ex[] = {
        { 0x400000, 4, "nop" }, { 0x400004, 4, "add" }, { 0x400008, 4, "ret" }
    };
    expect_ctx c = { ex, 3, 0 };
    CHECK(scan_golden("tests/golden/disasm_aarch64.txt", check_expect, &c) == 3);
    CHECK(c.i == 3);
}

static void test_mips_be(void) {
    static const expect_t ex[] = {
        { 0x400000, 4, "lw" }, { 0x400004, 4, "jr" }, { 0x400008, 4, "nop" }
    };
    expect_ctx c = { ex, 3, 0 };
    CHECK(scan_golden("tests/golden/disasm_mips_be.txt", check_expect, &c) == 3);
    CHECK(c.i == 3);
}

static void test_loongarch(void) {
    static const expect_t ex[] = {
        { 0x12000000, 4, "addi.d" }, { 0x12000004, 4, "addi.d" },
        { 0x12000008, 4, "syscall" }
    };
    expect_ctx c = { ex, 3, 0 };
    CHECK(scan_golden("tests/golden/disasm_loongarch.txt", check_expect, &c) == 3);
    CHECK(c.i == 3);
}

/* R1 architecture recognition on minimal program headers (mirrors the
 * hand-built ELF/Mach-O/PE headers from test_s1_probe). */
static void put16(unsigned char *b, unsigned v) { b[0]=(unsigned char)v; b[1]=(unsigned char)(v>>8); }
static void put32(unsigned char *b, unsigned v) {
    b[0]=(unsigned char)v; b[1]=(unsigned char)(v>>8);
    b[2]=(unsigned char)(v>>16); b[3]=(unsigned char)(v>>24);
}

static void test_arch_recognition(void) {
    unsigned char elf[256]; memset(elf, 0, sizeof elf);
    memcpy(elf, "\x7f""ELF", 4);
    elf[4] = 2;                    /* ELFCLASS64 */
    elf[5] = 1;                    /* little endian */
    put16(elf + 0x10, 2);          /* ET_EXEC */
    put16(elf + 0x12, 0x3e);       /* EM_X86_64 */
    put32(elf + 0x14, 1);          /* version */
    np_probe_result_t p;
    np_probe(elf, sizeof elf, "prog.bin", &p);
    CHECK(p.prog.arch == NP_PARCH_X64);
    CHECK(p.confidence >= 70);

    memset(elf, 0, sizeof elf);
    memcpy(elf, "\x7f""ELF", 4);
    elf[4] = 2; elf[5] = 0;        /* big endian */
    put16(elf + 0x12, 0x08);       /* EM_MIPS */
    np_probe(elf, sizeof elf, "mips.bin", &p);
    CHECK(p.prog.arch == NP_PARCH_MIPS);

    /* LoongArch: probe recognizes EM_LOONGARCH byte as 0x62 (see np_probe.c) */
    memset(elf, 0, sizeof elf);
    memcpy(elf, "\x7f""ELF", 4);
    elf[4] = 2; elf[5] = 1;
    put16(elf + 0x12, 0x62);
    np_probe(elf, sizeof elf, "loong.bin", &p);
    CHECK(p.prog.arch == NP_PARCH_LOONGARCH);
}

int main(void) {
    test_x64();
    test_arm();
    test_aarch64();
    test_mips_be();
    test_loongarch();
    test_arch_recognition();
    printf("test_r8_golden: %d passed, %d failed\n", checks - fails, fails);
    return fails ? 1 : 0;
}
