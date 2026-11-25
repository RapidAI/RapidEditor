/* Regression test for np_hex_parse_bytes (hex find query parsing).
 * The function body is extracted verbatim from src/rapideditor.c by
 * run_tests.ps1 into np_hex_parse_bytes.impl, so the test always
 * exercises the real implementation. Covers hex modes (spaced, packed,
 * 0x-prefixed, comma-separated), odd-digit rejection, ASCII mode with
 * trimming, and the capacity limit. */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "np_hex_parse_bytes.impl"

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (cond) printf("ok  - %s\n", msg); \
    else { printf("FAIL- %s\n", msg); fails++; } } while (0)

static void hx(const char *in, const unsigned char *expect, int en,
               const char *msg) {
    unsigned char out[256];
    int n = -1;
    bool ok = np_hex_parse_bytes(in, false, out, (int)sizeof out, &n);
    CHECK(ok && n == en && memcmp(out, expect, (size_t)n) == 0, msg);
    if (!(ok && n == en))
        printf("      in=[%s] ok=%d n=%d expect_n=%d\n", in, (int)ok, n, en);
}

static void asci(const char *in, const char *expect, const char *msg) {
    unsigned char out[256];
    int n = -1;
    bool ok = np_hex_parse_bytes(in, true, out, (int)sizeof out, &n);
    CHECK(ok && n == (int)strlen(expect) &&
          memcmp(out, expect, (size_t)n) == 0, msg);
    if (!(ok && n == (int)strlen(expect)))
        printf("      in=[%s] ok=%d n=%d expect=[%s]\n", in, (int)ok, n, expect);
}

int main(void) {
    hx("1F 2E CC", (const unsigned char[]){0x1F, 0x2E, 0xCC}, 3,
       "hex: spaced triple");
    hx("1F2ECC", (const unsigned char[]){0x1F, 0x2E, 0xCC}, 3,
       "hex: packed");
    hx("0x1F2ECC", (const unsigned char[]){0x1F, 0x2E, 0xCC}, 3,
       "hex: 0x prefix");
    hx("1f 2e cc", (const unsigned char[]){0x1F, 0x2E, 0xCC}, 3,
       "hex: lowercase digits");
    hx("1F, 2E", (const unsigned char[]){0x1F, 0x2E}, 2,
       "hex: comma separated");
    hx("  1F2E  ", (const unsigned char[]){0x1F, 0x2E}, 2,
       "hex: trimmed");

    /* rejected forms */
    { unsigned char out[256]; int n = -1;
      CHECK(!np_hex_parse_bytes("1F2", false, out, 256, &n),
            "hex: odd digit count rejected"); }
    { unsigned char out[256]; int n = -1;
      CHECK(!np_hex_parse_bytes("GG", false, out, 256, &n),
            "hex: non-hex digit rejected"); }
    { unsigned char out[256]; int n = -1;
      CHECK(!np_hex_parse_bytes("   ", false, out, 256, &n),
            "hex: empty query rejected"); }
    { unsigned char out[8]; int n = -1;
      CHECK(!np_hex_parse_bytes("010203040506070809", false, out, 8, &n),
            "hex: over capacity rejected"); }

    /* ASCII mode */
    asci("ABC", "ABC", "ascii: plain text");
    asci("  abc  ", "abc", "ascii: trimmed");
    { unsigned char out[256]; int n = -1;
      CHECK(!np_hex_parse_bytes("", true, out, 256, &n),
            "ascii: empty rejected"); }

    printf(fails ? "\n%d FAILURES\n" : "\nALL TESTS PASSED (%d failures)\n",
           fails);
    return fails ? 1 : 0;
}
