/* np_job.c - A1 unified task model + event bus (see np_job.h). */
#include "np_job.h"
#include "np_contract.h"

#include <stdlib.h>
#include <string.h>

#define NP_JOB_MAX_LISTENERS 16
#define NP_JOB_DEF_OUTPUT_CAP (16u * 1024u * 1024u)
#define NP_JOB_DEF_CONTEXT_CAP (256u * 1024u)

typedef struct {
    np_job_listener fn;
    void *user;
    bool active;
} np_listener_slot_t;

struct np_job {
    uint64_t        id;
    np_track_t      track;
    np_job_kind_t   kind;
    np_job_status_t status;
    int             progress;
    np_budget_t     budget;
    np_provenance_t prov;

    unsigned char  *out;
    size_t          out_len;
    size_t          out_cap;

    np_listener_slot_t listeners[NP_JOB_MAX_LISTENERS];
    int                event_count;
    bool               emitting;   /* reentrancy guard */
};

/* --- contract name tables ------------------------------------------ */
const char *np_track_name(np_track_t t) {
    switch (t) {
        case NP_TRACK_TEXT:   return "text";
        case NP_TRACK_BINARY: return "binary";
        case NP_TRACK_FORMAT: return "format";
        case NP_TRACK_COMMON: return "common";
        default:              return "unknown";
    }
}

const char *np_job_status_name(np_job_status_t s) {
    switch (s) {
        case NP_JOB_CREATED:     return "created";
        case NP_JOB_QUEUED:      return "queued";
        case NP_JOB_RUNNING:     return "running";
        case NP_JOB_SUCCEEDED:   return "succeeded";
        case NP_JOB_FAILED:      return "failed";
        case NP_JOB_CANCELLED:   return "cancelled";
        case NP_JOB_ROLLED_BACK: return "rolled_back";
        default:                 return "unknown";
    }
}

const char *np_job_kind_name(np_job_kind_t k) {
    switch (k) {
        case NP_JOB_KIND_TEXT_PATCH:   return "text.patch";
        case NP_JOB_KIND_BYTE_PATCH:   return "byte.patch";
        case NP_JOB_KIND_FORMAT_PATCH: return "format.patch";
        case NP_JOB_KIND_ANALYZE:      return "analyze";
        case NP_JOB_KIND_SEARCH:       return "search";
        case NP_JOB_KIND_INSTALL:      return "install";
        case NP_JOB_KIND_DECOMPILE:    return "decompile";
        default:                       return "unknown";
    }
}

const char *np_patch_op_name(np_patch_op_t op) {
    switch (op) {
        case NP_OP_REPLACE: return "replace";
        case NP_OP_INSERT:  return "insert";
        case NP_OP_DELETE:  return "delete";
        default:            return "unknown";
    }
}

const char *np_tool_kind_name(np_tool_kind_t k) {
    switch (k) {
        case NP_TOOL_NATIVE: return "native";
        case NP_TOOL_SKILL:  return "skill";
        case NP_TOOL_MCP:    return "mcp";
        case NP_TOOL_SEARCH: return "search";
        default:             return "unknown";
    }
}

const char *np_risk_name(np_risk_t r) {
    switch (r) {
        case NP_RISK_READONLY: return "readonly";
        case NP_RISK_WRITE:    return "write";
        case NP_RISK_EXEC:     return "exec";
        case NP_RISK_NETWORK:  return "network";
        default:               return "unknown";
    }
}

np_budget_t np_job_default_budget(void) {
    np_budget_t b;
    b.max_output_bytes  = NP_JOB_DEF_OUTPUT_CAP;
    b.max_context_bytes = NP_JOB_DEF_CONTEXT_CAP;
    b.timeout_ms        = 0;
    return b;
}

/* --- event emission ------------------------------------------------- */
static void emit(np_job_t *job, np_event_t ev) {
    if (!job || job->emitting) return;
    job->event_count++;
    job->emitting = true;
    for (int i = 0; i < NP_JOB_MAX_LISTENERS; ++i) {
        np_listener_slot_t *s = &job->listeners[i];
        if (s->active && s->fn) s->fn(ev, job, s->user);
    }
    job->emitting = false;
}

/* --- lifecycle ------------------------------------------------------ */
np_job_t *np_job_create(uint64_t id, np_track_t track, np_job_kind_t kind,
                        const np_budget_t *budget, const np_provenance_t *prov) {
    np_job_t *j = (np_job_t *)calloc(1, sizeof(np_job_t));
    if (!j) return NULL;
    j->id     = id;
    j->track  = track;
    j->kind   = kind;
    j->status = NP_JOB_CREATED;
    j->progress = 0;
    j->budget = budget ? *budget : np_job_default_budget();
    if (prov) j->prov = *prov;
    emit(j, NP_EVT_CREATED);
    return j;
}

void np_job_destroy(np_job_t *job) {
    if (!job) return;
    if (!np_job_is_terminal(job)) np_job_cancel(job);
    free(job->out);
    free(job);
}

np_job_status_t np_job_status(const np_job_t *job) {
    return job ? job->status : NP_JOB_CREATED;
}

bool np_job_is_terminal(const np_job_t *job) {
    if (!job) return true;
    return job->status == NP_JOB_SUCCEEDED ||
           job->status == NP_JOB_FAILED ||
           job->status == NP_JOB_CANCELLED ||
           job->status == NP_JOB_ROLLED_BACK;
}

bool np_job_queue(np_job_t *job) {
    if (!job || job->status != NP_JOB_CREATED) return false;
    job->status = NP_JOB_QUEUED;
    emit(job, NP_EVT_QUEUED);
    return true;
}

bool np_job_start(np_job_t *job) {
    if (!job) return false;
    if (job->status != NP_JOB_CREATED && job->status != NP_JOB_QUEUED) return false;
    job->status = NP_JOB_RUNNING;
    emit(job, NP_EVT_STARTED);
    return true;
}

bool np_job_finish(np_job_t *job) {
    if (!job || job->status != NP_JOB_RUNNING) return false;
    job->status = NP_JOB_SUCCEEDED;
    job->progress = 100;
    emit(job, NP_EVT_SUCCEEDED);
    return true;
}

bool np_job_fail(np_job_t *job) {
    if (!job || job->status != NP_JOB_RUNNING) return false;
    job->status = NP_JOB_FAILED;
    emit(job, NP_EVT_FAILED);
    return true;
}

bool np_job_cancel(np_job_t *job) {
    if (!job || np_job_is_terminal(job)) return false;
    job->status = NP_JOB_CANCELLED;
    emit(job, NP_EVT_CANCELLED);
    return true;
}

bool np_job_rollback(np_job_t *job) {
    if (!job) return false;
    /* A finished, failed or cancelled job may be rolled back (its patches
     * are reverted). A running job must be cancelled first. */
    if (job->status == NP_JOB_RUNNING ||
        job->status == NP_JOB_ROLLED_BACK ||
        job->status == NP_JOB_CREATED ||
        job->status == NP_JOB_QUEUED) return false;
    job->status = NP_JOB_ROLLED_BACK;
    emit(job, NP_EVT_ROLLED_BACK);
    return true;
}

void np_job_set_progress(np_job_t *job, int pct) {
    if (!job || np_job_is_terminal(job)) return;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (pct == job->progress) return;
    job->progress = pct;
    emit(job, NP_EVT_PROGRESS);
}

int np_job_progress(const np_job_t *job) {
    return job ? job->progress : 0;
}

uint64_t      np_job_id(const np_job_t *job) { return job ? job->id : 0; }
np_track_t    np_job_track(const np_job_t *job) { return job ? job->track : NP_TRACK_COMMON; }
np_job_kind_t np_job_kind(const np_job_t *job) { return job ? job->kind : NP_JOB_KIND_ANALYZE; }

const np_budget_t *np_job_budget(const np_job_t *job) {
    return job ? &job->budget : NULL;
}

const np_provenance_t *np_job_provenance(const np_job_t *job) {
    return job ? &job->prov : NULL;
}

bool np_job_append_output(np_job_t *job, const void *data, size_t len) {
    if (!job || !data) return false;
    size_t cap = job->budget.max_output_bytes ? job->budget.max_output_bytes
                                              : NP_JOB_DEF_OUTPUT_CAP;
    if (len > cap - job->out_len) return false;  /* budget exceeded */
    if (job->out_len + len > job->out_cap) {
        size_t newcap = job->out_cap ? job->out_cap : 1024;
        while (newcap < job->out_len + len) {
            if (newcap > cap / 2) { newcap = cap; break; }
            newcap *= 2;
        }
        if (newcap > cap) newcap = cap;
        unsigned char *p = (unsigned char *)realloc(job->out, newcap);
        if (!p) return false;
        job->out = p;
        job->out_cap = newcap;
    }
    memcpy(job->out + job->out_len, data, len);
    job->out_len += len;
    return true;
}

const unsigned char *np_job_output(const np_job_t *job, size_t *out_len) {
    if (out_len) *out_len = job ? job->out_len : 0;
    return job ? job->out : NULL;
}

int np_job_add_listener(np_job_t *job, np_job_listener fn, void *user) {
    if (!job || !fn) return -1;
    for (int i = 0; i < NP_JOB_MAX_LISTENERS; ++i) {
        if (!job->listeners[i].active) {
            job->listeners[i].fn = fn;
            job->listeners[i].user = user;
            job->listeners[i].active = true;
            return i;
        }
    }
    return -1;
}

void np_job_remove_listener(np_job_t *job, int listener_id) {
    if (!job || listener_id < 0 || listener_id >= NP_JOB_MAX_LISTENERS) return;
    job->listeners[listener_id].active = false;
    job->listeners[listener_id].fn = NULL;
}

int np_job_event_count(const np_job_t *job) {
    return job ? job->event_count : 0;
}
