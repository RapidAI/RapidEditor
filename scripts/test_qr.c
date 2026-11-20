/* Unit checks for the QR encoder. Structural invariants that hold for all
 * valid QR codes; a separate PGM+cv2 cross-check verifies actual scannability. */
#include "np_qr.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

int main(void) {
    np_qr_code q;
    const char *s = "{\"v\":2,\"type\":\"maclaw_third_party_authorization\","
                    "\"session_id\":\"maqr_abc123def456ghi789\","
                    "\"hub_url\":\"https://hub.maclaw.top\","
                    "\"expires_at\":\"2026-11-21T10:00:00Z\","
                    "\"name\":\"RapidEditor AI\",\"url\":\"\",\"key\":\"\","
                    "\"model\":\"\",\"models\":null,\"protocol\":\"\","
                    "\"context_length\":0}";

    CHECK(np_qr_encode(s, strlen(s), &q), "encode 259-byte payload");
    int v = (q.size - 17) / 4;
    CHECK(q.size >= 21 && q.size <= 77 && (q.size - 17) % 4 == 0, "valid size");
    CHECK(v >= 1 && v <= 15, "version in range");

    /* Three finder patterns: top-left, top-right, bottom-left, each a 7x7
     * ring with a 3x3 dark center. */
    int f[3][2] = {{3,3},{q.size-4,3},{3,q.size-4}};
    for (int k = 0; k < 3; k++) {
        int cx = f[k][0], cy = f[k][1];
        CHECK(q.modules[cy][cx] == 1, "finder center dark");
        CHECK(q.modules[cy-3][cx-3] == 1 && q.modules[cy+3][cx+3] == 1, "finder corners dark");
        CHECK(q.modules[cy-2][cx] == 0 && q.modules[cy+2][cx] == 0, "finder ring white");
        CHECK(q.modules[cy-1][cx-1] == 1 && q.modules[cy][cx] == 1, "finder core dark");
    }
    /* Timing pattern row 6 alternates in the middle section. */
    for (int x = 9; x < q.size - 9; x++) {
        int expected = (x % 2 == 0) ? 1 : 0;
        CHECK(q.modules[6][x] == expected, "timing pattern alternates");
    }
    /* Dark module at (8, size-8) is always dark. */
    CHECK(q.modules[q.size - 8][8] == 1, "dark module");

    /* Small payload uses a small version; large payload uses a bigger one. */
    np_qr_code small;
    CHECK(np_qr_encode("HI", 2, &small), "encode small");
    CHECK(small.size < q.size, "small payload -> smaller version");

    /* Oversized payload is rejected. */
    char big[600]; memset(big, 'x', sizeof big);
    CHECK(!np_qr_encode(big, sizeof big, &q), "oversize rejected");

    printf("%s (%d failures)\n", fails ? "TESTS FAILED" : "ALL TESTS PASSED", fails);
    return fails ? 1 : 0;
}
