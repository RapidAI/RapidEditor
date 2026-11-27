#ifndef NP_DEQ_H
#define NP_DEQ_H

/*
 * np_deq.h - DC9/DC5: staged decompilation equivalence gate.
 *
 * AI-generated "source" can look right while meaning something different, and
 * recompilation is not always possible. This module implements the V0–V3
 * staged equivalence decision and the hard rule: a candidate may be labelled
 * EQUIVALENT only if every required level actually passed. Otherwise the
 * result is honestly `not_equivalent` / `unverified` with recorded reasons —
 * never a confident-looking guess.
 *
 * Verification is INJECTED (re-assembler / re-compiler / emulator / human are
 * all callbacks), so this gate is UI-free and testable with zero toolchain.
 * Golden function fixtures pin the expected pseudo-code key points per arch.
 */
#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/* DC5 levels, in increasing evidential strength. */
typedef enum {
    NP_DEQ_V0_REASSEMBLY = 0,   /* re-assemble candidate -> byte compare */
    NP_DEQ_V1_RECOMPILE,        /* recompile candidate -> instruction/CFG compare */
    NP_DEQ_V2_BEHAVIOUR,        /* isolated emulation -> differential behaviour */
    NP_DEQ_V3_HUMAN            /* human review + recorded known gaps */
} np_deq_level_t;

#define NP_DEQ_LEVEL_COUNT 4

const char *np_deq_level_name(np_deq_level_t l);

/* Verdict. Only EQUIVALENT may be presented as "equivalent". */
typedef enum {
    NP_DEQ_UNVERIFIED = 0,
    NP_DEQ_NOT_EQUIVALENT,
    NP_DEQ_EQUIVALENT
} np_deq_verdict_t;

const char *np_deq_verdict_name(np_deq_verdict_t v);

/* One injected check result. `detail` is a short human-readable reason. */
typedef struct {
    np_deq_level_t level;
    bool           passed;
    char           detail[256];
} np_deq_check_t;

/* Verification callback: perform one level's check. `user` carries external
 * tools (assembler/compiler/emulator/reviewer). Returns a check record. */
typedef np_deq_check_t (*np_deq_check_fn)(np_deq_level_t level,
                                          const char *original,
                                          const char *candidate,
                                          void *user);

/* Golden function sample: one architecture's fixed expected key points. */
typedef struct {
    char           arch[32];       /* "x64" / "arm" / ... */
    char           func[64];       /* function name */
    char           keypoints[8][96];/* expected pseudo-code/behavior key points */
    size_t         nkeypoints;
    uint32_t       hash;           /* CRC32 of canonical keypoint string */
} np_deq_golden_t;

/* Build a golden record from arch/func + keypoint list; computes hash. */
np_deq_golden_t np_deq_make_golden(const char *arch, const char *func,
                                   const char *const *points, size_t n);
/* Verify a candidate's extracted keypoints against a golden record (all
 * expected points must be present in candidate_points). */
bool np_deq_match_golden(const np_deq_golden_t *g,
                         const char *const *candidate_points, size_t n);

/* The decision record. */
typedef struct {
    np_deq_verdict_t verdict;
    np_deq_level_t   required_level;    /* highest level the caller demanded */
    np_deq_level_t   achieved_level;    /* highest contiguous passing level */
    np_deq_check_t   checks[NP_DEQ_LEVEL_COUNT];
    size_t           nchecks;
    char             known_gaps[512];   /* recorded gaps / caveats */
    bool             golden_matched;    /* pin against golden sample set */
} np_deq_result_t;

/* Run V0..required_level in order via the injected checker. A level fails =>
 * stop (fail-closed): verdict is NOT_EQUIVALENT (if a check actually failed)
 * or UNVERIFIED (if no checker available). All levels pass AND golden matched
 * => EQUIVALENT. `golden` may be NULL (then golden_matched=false and the
 * verdict can never be EQUIVALENT — honest). */
np_deq_result_t np_deq_evaluate(np_deq_level_t required,
                                const char *original, const char *candidate,
                                np_deq_check_fn check, void *user,
                                const np_deq_golden_t *golden,
                                const char *const *candidate_points, size_t npoints,
                                const char *known_gaps);

/* Convenience: render a one-line summary into buf. */
void np_deq_summary(const np_deq_result_t *r, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* NP_DEQ_H */
