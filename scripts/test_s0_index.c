/* Unit tests for A4 (np_index): local index + metrics + regression gate. */
#include "np_index.h"
#include "np_integrity.h"
#include <stdio.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

int main(void) {
    /* local index */
    np_local_index_t *idx = np_index_create();
    CHECK(idx && np_index_count(idx) == 0);
    CHECK(np_index_upsert_text(idx, "src/parser.c", "parse_header\ninit_parser\ntokenize\n", 100));
    CHECK(np_index_upsert_text(idx, "src/lexer.c", "next_token\nskip_ws\n", 200));
    unsigned char elf[6] = {0x7F,'E','L','F',2,1};
    CHECK(np_index_upsert_binary(idx, "firmware.elf", elf, 6, 32768, "ELF x86-64"));
    CHECK(np_index_count(idx) == 3);
    const np_index_doc_t *d = np_index_get(idx, "src/parser.c");
    CHECK(d && strstr(d->symbols, "parse_header") != NULL);
    d = np_index_get(idx, "firmware.elf");
    CHECK(d && strstr(d->symbols, "size=32768") != NULL);

    char paths[8][260];
    CHECK(np_index_search(idx, "parse_header", paths, 8) == 1);
    CHECK(strcmp(paths[0], "src/parser.c") == 0);
    CHECK(np_index_search(idx, "firmware", paths, 8) == 1);
    CHECK(np_index_search(idx, "ELF", paths, 8) == 1);  /* metadata type searchable */
    CHECK(np_index_search(idx, "nonexistent", paths, 8) == 0);
    /* upsert replaces, does not duplicate */
    CHECK(np_index_upsert_text(idx, "src/parser.c", "renamed_fn\n", 300));
    CHECK(np_index_count(idx) == 3);
    CHECK(np_index_search(idx, "parse_header", paths, 8) == 0);
    np_index_destroy(idx);

    /* metrics - local + disableable */
    np_metrics_t *m = np_metrics_create();
    CHECK(m && np_metrics_enabled(m));
    np_metrics_record(m, NP_METRIC_TEXT_SUGGEST_ACCEPT, true);
    np_metrics_record(m, NP_METRIC_TEXT_SUGGEST_ACCEPT, true);
    np_metrics_record(m, NP_METRIC_TEXT_SUGGEST_ACCEPT, false);
    CHECK(np_metrics_total(m, NP_METRIC_TEXT_SUGGEST_ACCEPT) == 3);
    CHECK(np_metrics_ok(m, NP_METRIC_TEXT_SUGGEST_ACCEPT) == 2);
    CHECK(np_metrics_rate(m, NP_METRIC_TEXT_SUGGEST_ACCEPT) > 0.66);
    /* invariant metric: ok means invariant held; record a violation */
    np_metrics_record(m, NP_METRIC_INTEGRITY_MISMATCH, true);   /* held */
    np_metrics_record(m, NP_METRIC_INTEGRITY_MISMATCH, false);  /* violation */
    CHECK(np_metrics_ok(m, NP_METRIC_INTEGRITY_MISMATCH) == 1);  /* 1 violation */
    /* disable => no-op */
    np_metrics_set_enabled(m, false);
    np_metrics_record(m, NP_METRIC_BYTE_MISWRITE, false);
    CHECK(np_metrics_total(m, NP_METRIC_BYTE_MISWRITE) == 0);
    CHECK(strcmp(np_metric_name(NP_METRIC_TASK_ONESHOT), "task_oneshot") == 0);
    np_metrics_destroy(m);

    /* regression gate R8/DC9/F9 */
    np_gate_t *g = np_gate_create();
    np_golden_sample_t s; memset(&s, 0, sizeof(s));
    strcpy(s.id, "r8-x64-prologue"); strcpy(s.track_kind, "disasm");
    strcpy(s.arch, "x86-64"); strcpy(s.sample_name, "prologue.bin");
    strcpy(s.expected_hash, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    CHECK(np_gate_add(g, &s));
    strcpy(s.id, "dc9-arm-sum"); strcpy(s.arch, "arm64"); strcpy(s.track_kind, "decompile");
    strcpy(s.expected_hash, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    CHECK(np_gate_add(g, &s));
    strcpy(s.id, "f9-sqlite-schema"); strcpy(s.track_kind, "format");
    strcpy(s.expected_hash, "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc");
    CHECK(np_gate_add(g, &s));
    CHECK(np_gate_count(g) == 3);

    np_gate_result_t rs[3];
    strcpy(rs[0].id, "r8-x64-prologue");
    strcpy(rs[0].hash, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    strcpy(rs[1].id, "dc9-arm-sum"); strcpy(rs[1].hash, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    strcpy(rs[2].id, "f9-sqlite-schema"); strcpy(rs[2].hash, "0000000000000000000000000000000000000000000000000000000000000000");
    char fails[8][80]; size_t nf = 0;
    double pass = np_gate_evaluate(g, rs, 3, fails, 8, &nf);
    CHECK(pass > 0.6 && pass < 0.7);  /* 2/3 */
    CHECK(nf == 1);
    CHECK(strcmp(fails[0], "f9-sqlite-schema") == 0);
    np_gate_destroy(g);

    printf("test_s0_index: ALL PASSED\n");
    return 0;
}
