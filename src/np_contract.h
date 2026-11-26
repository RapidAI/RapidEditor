#ifndef NP_CONTRACT_H
#define NP_CONTRACT_H

/*
 * np_contract.h - S0 shared data contracts for the AI-native editor.
 *
 * This header is the SINGLE source of truth for the cross-track data
 * structures defined in section 09 of the design document: Job, Patch,
 * Citation, ToolDescriptor, FeatureCard, SearchHit and ContextPack.
 *
 * Design rules (from the document's red lines):
 *   - Three tracks share the Job/Review/Undo frame, but patch PRIMITIVES
 *     are track-specific and never mixed.
 *   - Every AI conclusion is a hypothesis: it carries provenance,
 *     revision and an excerpt hash so the resolver can read it back locally.
 *   - Pure C99, no CUI dependency, so every module can be unit-tested
 *     standalone before it is wired into the UI.
 */
#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Tracks                                                              */
/* ------------------------------------------------------------------ */
typedef enum {
    NP_TRACK_TEXT   = 0,   /* lines / character selections              */
    NP_TRACK_BINARY = 1,   /* byte ranges / instructions                */
    NP_TRACK_FORMAT = 2,   /* parts / cells / tables / records          */
    NP_TRACK_COMMON = 3    /* capability layer, toolchain, index        */
} np_track_t;

const char *np_track_name(np_track_t t);

/* ------------------------------------------------------------------ */
/* Job (A1) - unified task model with intent/plan/execute/verify/rollback */
/* ------------------------------------------------------------------ */
typedef enum {
    NP_JOB_CREATED = 0,
    NP_JOB_QUEUED,
    NP_JOB_RUNNING,
    NP_JOB_SUCCEEDED,
    NP_JOB_FAILED,
    NP_JOB_CANCELLED,
    NP_JOB_ROLLED_BACK
} np_job_status_t;

const char *np_job_status_name(np_job_status_t s);

typedef enum {
    NP_JOB_KIND_TEXT_PATCH = 0,
    NP_JOB_KIND_BYTE_PATCH,
    NP_JOB_KIND_FORMAT_PATCH,
    NP_JOB_KIND_ANALYZE,
    NP_JOB_KIND_SEARCH,
    NP_JOB_KIND_INSTALL,
    NP_JOB_KIND_DECOMPILE
} np_job_kind_t;

const char *np_job_kind_name(np_job_kind_t k);

/* ------------------------------------------------------------------ */
/* Patch (A3) - three-track patch primitives                          */
/* ------------------------------------------------------------------ */
typedef enum {
    NP_OP_REPLACE = 0,
    NP_OP_INSERT,
    NP_OP_DELETE
} np_patch_op_t;

const char *np_patch_op_name(np_patch_op_t op);

/* ------------------------------------------------------------------ */
/* Citation (B5) - file:line / file:0xOFFSET / file#struct-path        */
/* ------------------------------------------------------------------ */
typedef enum {
    NP_CITE_LINE     = 0,  /* file:line        - text track    */
    NP_CITE_OFFSET   = 1,  /* file:0xOFFSET    - binary track  */
    NP_CITE_STRUCT   = 2,  /* file#struct-path - format track  */
    NP_CITE_EXTERNAL = 3   /* external knowledge source (X11)  */
} np_cite_kind_t;

/* ------------------------------------------------------------------ */
/* ToolDescriptor (X1) - one contract for native/skill/mcp/search      */
/* ------------------------------------------------------------------ */
typedef enum {
    NP_TOOL_NATIVE = 0,
    NP_TOOL_SKILL  = 1,
    NP_TOOL_MCP    = 2,
    NP_TOOL_SEARCH = 3
} np_tool_kind_t;

const char *np_tool_kind_name(np_tool_kind_t k);

typedef enum {
    NP_RISK_READONLY = 0,  /* no writes                                */
    NP_RISK_WRITE    = 1,  /* may write via Patch Job + Review         */
    NP_RISK_EXEC     = 2,  /* may spawn a local process                */
    NP_RISK_NETWORK  = 3   /* may reach the network                    */
} np_risk_t;

const char *np_risk_name(np_risk_t r);

/* scope bitmask - least privilege by default */
#define NP_SCOPE_NONE      0u
#define NP_SCOPE_TEXT      (1u << 0)
#define NP_SCOPE_BINARY    (1u << 1)
#define NP_SCOPE_FORMAT    (1u << 2)
#define NP_SCOPE_FS_READ   (1u << 3)
#define NP_SCOPE_FS_WRITE  (1u << 4)
#define NP_SCOPE_NETWORK   (1u << 5)
#define NP_SCOPE_EXEC      (1u << 6)

#ifdef __cplusplus
}
#endif
#endif /* NP_CONTRACT_H */
