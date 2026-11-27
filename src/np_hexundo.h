#ifndef NP_HEXUNDO_H
#define NP_HEXUNDO_H

/*
 * np_hexundo.h - multi-level undo for manual hex editing.
 *
 * cui_hex_view overwrites bytes in-place and keeps no undo stack. This module
 * wraps np_bytepatch (H1) as a shadow history: each frame's edited buffer is
 * observed, consecutive edits inside a short time window are merged into one
 * undo group, and undo/redo restore the exact bytes back to the caller.
 *
 * UI-free and CUI-independent so the session semantics can be unit-tested.
 */
#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdbool.h>

typedef struct np_hexundo np_hexundo_t;

/* Default merge window (seconds): edits closer than this are one undo step. */
#define NP_HEXUNDO_WINDOW 0.6

/* Create shadow history from the initial document bytes. */
np_hexundo_t *np_hexundo_create(const void *data, size_t len);
void np_hexundo_destroy(np_hexundo_t *h);

/* Advance the module's monotonic clock (call once per rendered frame, dt in
 * seconds, e.g. from cui_delta_time). */
void np_hexundo_tick(np_hexundo_t *h, double dt);

/* Report the current (possibly just edited) buffer. No-op if unchanged.
 * A length change is treated as an external replacement and resets history. */
void np_hexundo_observe(np_hexundo_t *h, const void *cur, size_t len);

/* Close the active edit session so the next undo reverts this whole group.
 * Call on focus loss / tab switch / explicit commit. */
void np_hexundo_flush(np_hexundo_t *h);

bool np_hexundo_can_undo(const np_hexundo_t *h);
bool np_hexundo_can_redo(const np_hexundo_t *h);

/* Move the shadow state one step and return its bytes (valid until the next
 * mutating call). Returns false if no such step exists. */
bool np_hexundo_undo(np_hexundo_t *h);
bool np_hexundo_redo(np_hexundo_t *h);

/* Current shadow bytes. */
const unsigned char *np_hexundo_data(const np_hexundo_t *h, size_t *out_len);

/* Discard all history and re-baseline (file reload / save-as / mode switch). */
void np_hexundo_reset(np_hexundo_t *h, const void *data, size_t len);

#ifdef __cplusplus
}
#endif
#endif /* NP_HEXUNDO_H */
