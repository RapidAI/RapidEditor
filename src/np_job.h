#ifndef NP_JOB_H
#define NP_JOB_H

/*
 * np_job.h - A1: unified task model + event bus.
 *
 * All AI work (text patch, byte patch, format patch, analysis, search,
 * tool install, decompile) runs as a Job with a single state machine.
 * One Job == one undo group. Every Job carries budgets, a cancel token
 * and provenance so it can be paused, cancelled, audited and rolled back.
 *
 * Pure C99, thread-agnostic: the production code runs Jobs on the three
 * existing worker backends; this module only owns state + bookkeeping.
 */
#ifdef __cplusplus
extern "C" {
#endif

#include "np_contract.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/* Opaque job handle. Fields are exposed via accessors to keep the state
 * machine authoritative (no direct status writes from callers). */
typedef struct np_job np_job_t;

/* Event kinds emitted on the bus. */
typedef enum {
    NP_EVT_CREATED = 0,
    NP_EVT_QUEUED,
    NP_EVT_STARTED,
    NP_EVT_PROGRESS,
    NP_EVT_SUCCEEDED,
    NP_EVT_FAILED,
    NP_EVT_CANCELLED,
    NP_EVT_ROLLED_BACK
} np_event_t;

/* Resource budgets - hard caps, configurable at the application layer. */
typedef struct {
    size_t  max_output_bytes;   /* output cap; 0 = module default         */
    size_t  max_context_bytes;  /* context window cap                     */
    int     timeout_ms;         /* <=0 = no timeout                        */
} np_budget_t;

typedef struct {
    char    tool[80];       /* tool descriptor id that produced output    */
    char    source[160];    /* origin: local / provider / mirror path     */
    char    sha256[65];     /* hash of the output artifact (may be empty) */
    uint64_t revision;      /* document revision this job was based on    */
} np_provenance_t;

/* Event callback. `job` is valid for the duration of the call only. */
typedef void (*np_job_listener)(np_event_t ev, np_job_t *job, void *user);

/* Create a job. Returns NULL on allocation failure. */
np_job_t *np_job_create(uint64_t id, np_track_t track, np_job_kind_t kind,
                        const np_budget_t *budget, const np_provenance_t *prov);

/* Destroy a job. Cancels first if still running. Safe with NULL. */
void np_job_destroy(np_job_t *job);

/* --- state machine ------------------------------------------------- */
np_job_status_t np_job_status(const np_job_t *job);
bool np_job_is_terminal(const np_job_t *job);

/* Legal transitions are enforced. Returns false if the transition is
 * not allowed from the current state. */
bool np_job_queue(np_job_t *job);
bool np_job_start(np_job_t *job);
bool np_job_finish(np_job_t *job);
bool np_job_fail(np_job_t *job);
bool np_job_cancel(np_job_t *job);
bool np_job_rollback(np_job_t *job);

/* Progress is clamped to 0..100 and ignored on terminal jobs. */
void np_job_set_progress(np_job_t *job, int pct);
int  np_job_progress(const np_job_t *job);

/* --- identity / metadata ------------------------------------------ */
uint64_t     np_job_id(const np_job_t *job);
np_track_t   np_job_track(const np_job_t *job);
np_job_kind_t np_job_kind(const np_job_t *job);
const np_budget_t     *np_job_budget(const np_job_t *job);
const np_provenance_t *np_job_provenance(const np_job_t *job);

/* Output buffer: append bytes, capped by max_output_bytes.
 * Returns true if the whole chunk was accepted. */
bool np_job_append_output(np_job_t *job, const void *data, size_t len);
const unsigned char *np_job_output(const np_job_t *job, size_t *out_len);

/* --- event bus ----------------------------------------------------- */
/* Register a listener. Returns an id used to remove it. Max 16 listeners. */
int  np_job_add_listener(np_job_t *job, np_job_listener fn, void *user);
void np_job_remove_listener(np_job_t *job, int listener_id);
/* Number of events emitted (for tests/telemetry). */
int  np_job_event_count(const np_job_t *job);

/* Default budgets used when budget == NULL. */
np_budget_t np_job_default_budget(void);

#ifdef __cplusplus
}
#endif
#endif /* NP_JOB_H */
