/* Unit tests for A1 (np_job). Standalone C99, no CUI dependency. */
#include "np_job.h"
#include <stdio.h>
#include <string.h>

static int g_events = 0;
static np_event_t g_last = NP_EVT_CREATED;
static void on_event(np_event_t ev, np_job_t *j, void *user) {
    (void)j; (void)user;
    g_events++; g_last = ev;
}

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

int main(void) {
    np_job_t *j = np_job_create(42, NP_TRACK_BINARY, NP_JOB_KIND_BYTE_PATCH, NULL, NULL);
    CHECK(j != NULL);
    CHECK(np_job_id(j) == 42);
    CHECK(np_job_track(j) == NP_TRACK_BINARY);
    CHECK(np_job_status(j) == NP_JOB_CREATED);
    CHECK(strcmp(np_job_status_name(np_job_status(j)), "created") == 0);

    CHECK(np_job_add_listener(j, on_event, NULL) == 0);
    g_events = 0;

    /* happy path: created -> queued -> running -> progress -> succeeded */
    CHECK(np_job_queue(j));
    CHECK(np_job_start(j));
    CHECK(np_job_status(j) == NP_JOB_RUNNING);
    CHECK(!np_job_queue(j));  /* illegal transition */
    np_job_set_progress(j, 50);
    CHECK(np_job_progress(j) == 50);
    np_job_set_progress(j, 999);
    CHECK(np_job_progress(j) == 100);
    CHECK(np_job_append_output(j, "hello", 5));
    size_t n = 0;
    const unsigned char *p = np_job_output(j, &n);
    CHECK(n == 5 && memcmp(p, "hello", 5) == 0);
    CHECK(np_job_finish(j));
    CHECK(np_job_is_terminal(j));
    CHECK(g_events >= 5);  /* queued, started, 2x progress, succeeded */
    CHECK(g_last == NP_EVT_SUCCEEDED);

    /* terminal states reject further transitions */
    CHECK(!np_job_start(j));
    CHECK(!np_job_cancel(j));
    np_job_set_progress(j, 10);
    CHECK(np_job_progress(j) == 100);  /* unchanged */
    CHECK(np_job_rollback(j));          /* succeeded -> rolled back */
    CHECK(np_job_status(j) == NP_JOB_ROLLED_BACK);

    np_job_destroy(j);

    /* cancel path */
    np_job_t *c = np_job_create(7, NP_TRACK_TEXT, NP_JOB_KIND_TEXT_PATCH, NULL, NULL);
    CHECK(np_job_start(c));
    CHECK(np_job_cancel(c));
    CHECK(np_job_status(c) == NP_JOB_CANCELLED);
    CHECK(np_job_is_terminal(c));
    np_job_destroy(c);  /* destroys a terminal job cleanly */

    /* output budget enforcement */
    np_budget_t b = np_job_default_budget();
    b.max_output_bytes = 4;
    np_job_t *k = np_job_create(9, NP_TRACK_COMMON, NP_JOB_KIND_SEARCH, &b, NULL);
    CHECK(np_job_append_output(k, "abc", 3));
    CHECK(!np_job_append_output(k, "defg", 4));  /* would exceed cap */
    np_job_destroy(k);

    printf("test_s0_job: ALL PASSED\n");
    return 0;
}
