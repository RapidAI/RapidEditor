#ifndef NP_CAPABILITY_H
#define NP_CAPABILITY_H

/*
 * np_capability.h - X1 + X2: capability registry + routing budget.
 *
 * X1: one ToolDescriptor contract for native tools, Skills, MCP servers and
 *     search profiles. Only healthy, authorized, in-budget tools are exposed
 *     to the model.
 * X2: core tools stay resident; low-frequency tools are discovered on demand.
 *     Score = BM25-ish relevance + historical success rate + priority -
 *     consecutive-failure penalty. A hard tool-count cap is enforced.
 *
 * The registry is an in-memory store; persistence/install of extensions is
 * X3/X4 on top. Pure C99, unit-testable without network or LLM.
 */
#ifdef __cplusplus
extern "C" {
#endif

#include "np_contract.h"
#include <stddef.h>
#include <stdbool.h>

typedef struct np_registry np_registry_t;

/* Full ToolDescriptor from the design document section 09. */
typedef struct {
    char          id[80];           /* "editor.text.read"                   */
    np_tool_kind_t kind;
    char          name[80];
    char          description[240]; /* used for relevance scoring          */
    unsigned      scopes;           /* NP_SCOPE_* bitmask                   */
    np_risk_t     risk;
    char          source[160];      /* builtin / hub url / mcp endpoint     */
    char          version[32];
    char          sha256[65];
    char          license[64];
    bool          health;           /* last health check passed             */
    bool          authorized;       /* within user's granted policy         */
    bool          core;             /* always resident                      */
    int           budget;           /* relative token cost (1..N)           */
    int           usage_score;      /* historical success, -100..100        */
    int           fail_streak;      /* consecutive failures -> downrank     */
} np_tool_desc_t;

np_registry_t *np_registry_create(void);
void np_registry_destroy(np_registry_t *r);

/* Register or replace a tool by id. Returns false on invalid descriptor. */
bool np_registry_register(np_registry_t *r, const np_tool_desc_t *desc);
const np_tool_desc_t *np_registry_get(const np_registry_t *r, const char *id);
size_t np_registry_count(const np_registry_t *r);

/* Record an outcome: success raises usage_score and clears fail_streak;
 * failure lowers score and increments fail_streak (auto-disable after 3). */
void np_registry_report(np_registry_t *r, const char *id, bool success);

/* Health / authorization updates (from X6 health checks / user policy). */
bool np_registry_set_health(np_registry_t *r, const char *id, bool healthy);
bool np_registry_set_authorized(np_registry_t *r, const char *id, bool allowed);
bool np_registry_remove(np_registry_t *r, const char *id);

/* Eligibility: healthy && authorized && fail_streak < 3 && budget <= cap. */
bool np_tool_eligible(const np_tool_desc_t *d, int budget_cap);

/* Relevance score of a tool for a query (simple token overlap, case-insensitive).
 * Higher is better. Used together with usage_score + priority for routing. */
int np_tool_relevance(const np_tool_desc_t *d, const char *query);

/*
 * Route: pick at most `max_tools` tools for `query`, subject to `budget_cap`.
 * Core eligible tools are always included first. Remaining slots go to the
 * highest-ranked non-core tools. Ranking = relevance*2 + usage_score - fail_streak*10.
 * Returns the number of ids written; ids are copied into caller array.
 */
size_t np_registry_route(const np_registry_t *r, const char *query,
                         int budget_cap, size_t max_tools,
                         char out_ids[][80], size_t out_cap);

#ifdef __cplusplus
}
#endif
#endif /* NP_CAPABILITY_H */
