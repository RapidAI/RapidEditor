/* Emit a QR code as a PGM image for cross-checking with cv2.QRCodeDetector.
 * Usage: qr_emit <payload-string> <out.pgm> */
#include "np_qr.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: qr_emit <text> <out.pgm>\n"); return 2; }
    np_qr_code qr;
    if (!np_qr_encode(argv[1], strlen(argv[1]), &qr)) {
        fprintf(stderr, "encode failed\n"); return 1;
    }
    int scale = 8, quiet = 4;
    int dim = (qr.size + quiet * 2) * scale;
    FILE *f = fopen(argv[2], "wb");
    if (!f) { perror("fopen"); return 1; }
    fprintf(f, "P5\n%d %d\n255\n", dim, dim);
    for (int y = 0; y < dim; y++) {
        for (int x = 0; x < dim; x++) {
            int mx = x / scale - quiet, my = y / scale - quiet;
            int dark = (mx >= 0 && my >= 0 && mx < qr.size && my < qr.size &&
                        qr.modules[my][mx]);
            fputc(dark ? 0 : 255, f);
        }
    }
    fclose(f);
    fprintf(stderr, "size=%d version=%d\n", qr.size, (qr.size - 17) / 4);
    return 0;
}
