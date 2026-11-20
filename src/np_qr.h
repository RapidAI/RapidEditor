#ifndef NP_QR_H
#define NP_QR_H

/* Tiny QR Code encoder (ISO/IEC 18004, Model 2) for rendering the maclaw
 * device-authorization payload as a scannable QR on a cui_canvas.
 *
 * Scope: byte mode only, error-correction level M, automatic version select
 * from 1..15. That covers UTF-8/ASCII payloads up to ~500 bytes, far more
 * than the device-auth QR (~300 bytes). No external dependency. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NP_QR_MAX_VERSION 15
#define NP_QR_MAX_MODULES (4 * NP_QR_MAX_VERSION + 17) /* 77 */

typedef struct {
    int size;                                  /* modules per side */
    uint8_t modules[NP_QR_MAX_MODULES][NP_QR_MAX_MODULES]; /* 1 = dark */
} np_qr_code;

/* Encode `len` bytes of `data` into `out`. Returns true on success; false if
 * the data is too large for version 15 / ECC M. */
bool np_qr_encode(const void *data, size_t len, np_qr_code *out);

#ifdef __cplusplus
}
#endif
#endif /* NP_QR_H */
