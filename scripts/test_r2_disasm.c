/* test_r2_disasm.c - R2 multi-architecture disassembly view tests. */
#include "np_disasm.h"
#include <stdio.h>
#include <string.h>

static int checks = 0, fails = 0;
#define CHECK(x) do { ++checks; if (!(x)) { \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #x); ++fails; } } while (0)

static unsigned char CODE[] = { 0x48,0x89,0x5c,0x24,0x08, 0x48,0x83,0xc4,0x10, 0xc3 };

/* Fake decoder: emits fixed-size instructions based on arch/syntax/mode. */
static int fake_decode(const unsigned char *data, size_t off, size_t len,
                       np_parch_t arch, np_disasm_syntax_t syntax,
                       np_disasm_armmode_t mode, uint64_t base,
                       np_disasm_insn_t *out, size_t cap, void *user) {
    (void)user;
    if (arch == NP_PARCH_UNKNOWN || !cap) return -1;
    size_t sz = (arch == NP_PARCH_X64) ? 5 : ((arch == NP_PARCH_ARM && mode == NP_DMODE_THUMB) ? 2 : 4);
    size_t n = 0;
    for (size_t p = 0; p + sz <= len && n < cap; p += sz) {
        out[n].offset = off + p;
        out[n].size = sz;
        out[n].addr = base + off + p;
        const char *syn = syntax == NP_DSYNTAX_ATT ? "movq" : "mov";
        snprintf(out[n].text, sizeof out[n].text, "%s r%d, [r%d]", syn, (int)n, (int)n + 1);
        (void)data;
        n++;
    }
    return n ? (int)n : -1;
}

static int fail_decode(const unsigned char *d, size_t o, size_t l, np_parch_t a,
                       np_disasm_syntax_t s, np_disasm_armmode_t m, uint64_t b,
                       np_disasm_insn_t *out, size_t cap, void *u) {
    (void)d;(void)o;(void)l;(void)a;(void)s;(void)m;(void)b;(void)out;(void)cap;(void)u;
    return -1;
}

static np_probe_result_t good_probe(np_parch_t arch, int conf, uint64_t base) {
    np_probe_result_t p; memset(&p, 0, sizeof p);
    p.prog.arch = arch; p.confidence = conf; p.prog.image_base = base;
    p.prog.bits = (arch == NP_PARCH_X86) ? 32 : 64;
    return p;
}

static void test_decode_and_sources(void) {
    np_disasm_view_t *v = np_disasm_view_create();
    np_disasm_decoder_t dec = { fake_decode, NULL, NP_DSRC_TEST };
    np_disasm_view_set_decoders(v, &dec, 1);
    np_probe_result_t p = good_probe(NP_PARCH_X64, 95, 0x400000);
    CHECK(np_disasm_decode(v, CODE, sizeof CODE, 0, sizeof CODE, &p, NULL, 0));
    CHECK(np_disasm_count(v) == 2);            /* 10 bytes / 5 = 2 */
    const np_disasm_insn_t *i = np_disasm_at(v, 0);
    CHECK(i && i->offset == 0 && i->size == 5);
    CHECK(i && i->addr == 0x400000);
    CHECK(i && strstr(i->text, "mov") != NULL);
    CHECK(np_disasm_source(v) == NP_DSRC_TEST);
    CHECK(strcmp(np_disasm_source_name(NP_DSRC_TEST), "测试注入") == 0);
    np_disasm_view_destroy(v);
}

static void test_bidirectional_nav(void) {
    np_disasm_view_t *v = np_disasm_view_create();
    np_disasm_decoder_t dec = { fake_decode, NULL, NP_DSRC_TEST };
    np_disasm_view_set_decoders(v, &dec, 1);
    np_probe_result_t p = good_probe(NP_PARCH_X86, 90, 0);  /* 4-byte insns */
    CHECK(np_disasm_decode(v, CODE, sizeof CODE, 0, 8, &p, NULL, 0));
    CHECK(np_disasm_count(v) == 2);
    CHECK(np_disasm_insn_for_offset(v, 0) == 0);
    CHECK(np_disasm_insn_for_offset(v, 3) == 0);   /* inside first */
    CHECK(np_disasm_insn_for_offset(v, 4) == 1);
    CHECK(np_disasm_insn_for_offset(v, 7) == 1);
    CHECK(np_disasm_insn_for_offset(v, 8) == -1);  /* beyond */
    CHECK(np_disasm_offset_for_insn(v, 0) == 0);
    CHECK(np_disasm_offset_for_insn(v, 1) == 4);
    CHECK(np_disasm_offset_for_insn(v, 2) == (size_t)-1);
    np_disasm_view_destroy(v);
}

static void test_syntax_mode(void) {
    np_disasm_view_t *v = np_disasm_view_create();
    CHECK(np_disasm_view_syntax(v) == NP_DSYNTAX_INTEL);
    CHECK(np_disasm_view_armmode(v) == NP_DMODE_ARM);
    np_disasm_view_set_syntax(v, NP_DSYNTAX_ATT);
    np_disasm_view_set_armmode(v, NP_DMODE_THUMB);
    CHECK(np_disasm_view_syntax(v) == NP_DSYNTAX_ATT);
    CHECK(np_disasm_view_armmode(v) == NP_DMODE_THUMB);
    np_disasm_decoder_t dec = { fake_decode, NULL, NP_DSRC_TEST };
    np_disasm_view_set_decoders(v, &dec, 1);
    np_probe_result_t p = good_probe(NP_PARCH_ARM, 85, 0);
    CHECK(np_disasm_decode(v, CODE, sizeof CODE, 0, 4, &p, NULL, 0));
    CHECK(strstr(np_disasm_at(v, 0)->text, "movq") != NULL);  /* AT&T honored */
    CHECK(np_disasm_at(v, 0)->size == 2);                     /* Thumb honored */
    np_disasm_view_destroy(v);
}

static void test_confidence_gate(void) {
    np_disasm_view_t *v = np_disasm_view_create();
    np_disasm_decoder_t dec = { fake_decode, NULL, NP_DSRC_TEST };
    np_disasm_view_set_decoders(v, &dec, 1);
    char err[160];
    np_probe_result_t low = good_probe(NP_PARCH_X64, 50, 0);
    CHECK(!np_disasm_decode(v, CODE, sizeof CODE, 0, sizeof CODE, &low, err, sizeof err));
    CHECK(np_disasm_count(v) == 0);
    CHECK(strstr(err, "架构未确认") != NULL);
    np_probe_result_t unk = good_probe(NP_PARCH_UNKNOWN, 99, 0);
    CHECK(!np_disasm_decode(v, CODE, sizeof CODE, 0, sizeof CODE, &unk, NULL, 0));
    CHECK(np_disasm_count(v) == 0);
    np_disasm_view_destroy(v);
}

static void test_fallback_chain(void) {
    np_disasm_view_t *v = np_disasm_view_create();
    np_disasm_decoder_t decs[2] = {
        { fail_decode, NULL, NP_DSRC_CAPSTONE },
        { fake_decode, NULL, NP_DSRC_OBJDUMP }
    };
    np_disasm_view_set_decoders(v, decs, 2);
    np_probe_result_t p = good_probe(NP_PARCH_X64, 92, 0);
    CHECK(np_disasm_decode(v, CODE, sizeof CODE, 0, sizeof CODE, &p, NULL, 0));
    CHECK(np_disasm_source(v) == NP_DSRC_OBJDUMP);   /* source provenance */
    CHECK(np_disasm_at(v, 0)->source == NP_DSRC_OBJDUMP);
    np_disasm_view_destroy(v);
}

static void test_no_decoder_and_range(void) {
    np_disasm_view_t *v = np_disasm_view_create();
    np_probe_result_t p = good_probe(NP_PARCH_X64, 92, 0);
    char err[160];
    CHECK(!np_disasm_decode(v, CODE, sizeof CODE, 0, 4, &p, err, sizeof err));
    CHECK(strstr(err, "无可用反汇编器") != NULL);
    np_disasm_decoder_t dec = { fake_decode, NULL, NP_DSRC_TEST };
    np_disasm_view_set_decoders(v, &dec, 1);
    CHECK(!np_disasm_decode(v, CODE, sizeof CODE, 8, 100, &p, err, sizeof err));
    CHECK(strstr(err, "越界") != NULL);
    np_disasm_view_clear(v);
    CHECK(np_disasm_count(v) == 0);
    CHECK(np_disasm_source(v) == NP_DSRC_NONE);
    np_disasm_view_destroy(v);
}

static void test_objdump_parse(void) {
    np_disasm_insn_t ins;
    CHECK(np_disasm_parse_objdump_line("  401000:\t48 89 5c 24 08\tmov rbx, [rsp+8]", &ins));
    CHECK(ins.addr == 0x401000);
    CHECK(ins.size == 5);
    CHECK(strcmp(ins.text, "mov rbx, [rsp+8]") == 0);
    CHECK(ins.source == NP_DSRC_OBJDUMP);
    CHECK(np_disasm_parse_objdump_line("400500: e8 00 00 00 00 call 400505", &ins));
    CHECK(ins.addr == 0x400500 && ins.size == 5);
    CHECK(strcmp(ins.text, "call 400505") == 0);
    /* LoongArch-style 4-byte words. */
    CHECK(np_disasm_parse_objdump_line("12000000: 02 00 00 4c addi.d $r0, $r0, 0", &ins));
    CHECK(ins.size == 4 && strstr(ins.text, "addi.d") != NULL);
    /* Reject garbage / empty operands / too long. */
    CHECK(!np_disasm_parse_objdump_line("not hex at all", &ins));
    CHECK(!np_disasm_parse_objdump_line("48 89", &ins));
    CHECK(!np_disasm_parse_objdump_line("  401000: 48 89", &ins));
}

int main(void) {
    test_decode_and_sources();
    test_bidirectional_nav();
    test_syntax_mode();
    test_confidence_gate();
    test_fallback_chain();
    test_no_decoder_and_range();
    test_objdump_parse();
    printf("test_r2_disasm: %d passed, %d failed\n", checks - fails, fails);
    return fails ? 1 : 0;
}
