/* np_capability.c - X1+X2 capability registry + routing (see header). */
#include "np_capability.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define NP_MAX_TOOLS 256
#define NP_FAIL_DISABLE 3

struct np_registry {
    np_tool_desc_t tools[NP_MAX_TOOLS];
    size_t count;
};

np_registry_t *np_registry_create(void) {
    return (np_registry_t *)calloc(1, sizeof(np_registry_t));
}

void np_registry_destroy(np_registry_t *r) { free(r); }

static long find(const np_registry_t *r, const char *id) {
    if (!r || !id) return -1;
    for (size_t i = 0; i < r->count; ++i)
        if (strcmp(r->tools[i].id, id) == 0) return (long)i;
    return -1;
}

bool np_registry_register(np_registry_t *r, const np_tool_desc_t *desc) {
    if (!r || !desc || !desc->id[0]) return false;
    long at = find(r, desc->id);
    if (at >= 0) { r->tools[at] = *desc; return true; }
    if (r->count >= NP_MAX_TOOLS) return false;
    r->tools[r->count++] = *desc;
    return true;
}

const np_tool_desc_t *np_registry_get(const np_registry_t *r, const char *id) {
    long at = find(r, id);
    return at >= 0 ? &r->tools[at] : NULL;
}

size_t np_registry_count(const np_registry_t *r) { return r ? r->count : 0; }

void np_registry_report(np_registry_t *r, const char *id, bool success) {
    long at = find(r, id);
    if (at < 0) return;
    np_tool_desc_t *d = &r->tools[at];
    if (success) {
        d->fail_streak = 0;
        if (d->usage_score < 100) d->usage_score += 5;
    } else {
        d->fail_streak++;
        d->usage_score -= 15;
        if (d->usage_score < -100) d->usage_score = -100;
    }
}

bool np_registry_set_health(np_registry_t *r, const char *id, bool healthy) {
    long at = find(r, id);
    if (at < 0) return false;
    r->tools[at].health = healthy;
    if (healthy) r->tools[at].fail_streak = 0;
    return true;
}

bool np_registry_set_authorized(np_registry_t *r, const char *id, bool allowed) {
    long at = find(r, id);
    if (at < 0) return false;
    r->tools[at].authorized = allowed;
    return true;
}

bool np_registry_remove(np_registry_t *r, const char *id) {
    long at = find(r, id);
    if (at < 0) return false;
    for (size_t i = (size_t)at; i + 1 < r->count; ++i)
        r->tools[i] = r->tools[i + 1];
    r->count--;
    return true;
}

bool np_tool_eligible(const np_tool_desc_t *d, int budget_cap) {
    if (!d) return false;
    if (!d->health || !d->authorized) return false;
    if (d->fail_streak >= NP_FAIL_DISABLE) return false;
    if (budget_cap > 0 && d->budget > budget_cap) return false;
    return true;
}

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

static bool is_token_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '.' || c == '_' || c == '-';
}

/* Count how many distinct query tokens appear as whole tokens in text.
 * Approximates BM25 token overlap well enough for tool routing. */
int np_tool_relevance(const np_tool_desc_t *d, const char *query) {
    if (!d || !query || !*query) return 0;
    char q[512]; size_t qn = 0;
    for (const char *p = query; *p && qn < sizeof(q)-1; ++p) q[qn++] = lower(*p);
    q[qn] = '\0';

    char hay[1024]; size_t hn = 0;
    const char *fields[2] = { d->id, d->description };
    for (int f = 0; f < 2; ++f) {
        for (const char *p = fields[f]; *p && hn < sizeof(hay)-1; ++p)
            hay[hn++] = lower(*p);
        if (hn < sizeof(hay)-1) hay[hn++] = ' ';
    }
    hay[hn] = '\0';

    int score = 0;
    size_t i = 0;
    while (i < qn) {
        while (i < qn && !is_token_char(q[i])) i++;
        size_t s = i;
        while (i < qn && is_token_char(q[i])) i++;
        size_t len = i - s;
        if (len < 2) continue; /* skip tiny words */
        /* search token q[s..s+len) as a whole token in hay */
        for (size_t j = 0; j + len <= hn; ++j) {
            bool left_ok  = (j == 0) || !is_token_char(hay[j-1]);
            bool right_ok = (j+len == hn) || !is_token_char(hay[j+len]);
            if (left_ok && right_ok && memcmp(hay+j, q+s, len) == 0) {
                score += 1;
                break;
            }
        }
    }
    return score;
}

size_t np_registry_route(const np_registry_t *r, const char *query,
                         int budget_cap, size_t max_tools,
                         char out_ids[][80], size_t out_cap) {
    if (!r || !out_ids || out_cap == 0) return 0;
    if (max_tools > out_cap) max_tools = out_cap;

    size_t written = 0;

    /* 1) core tools first (resident) */
    for (size_t i = 0; i < r->count && written < max_tools; ++i) {
        const np_tool_desc_t *d = &r->tools[i];
        if (d->core && np_tool_eligible(d, budget_cap)) {
            snprintf(out_ids[written++], 80, "%s", d->id);
        }
    }

    /* 2) rank remaining eligible non-core tools */
    typedef struct { size_t idx; int rank; } cand_t;
    cand_t cands[NP_MAX_TOOLS];
    size_t nc = 0;
    for (size_t i = 0; i < r->count; ++i) {
        const np_tool_desc_t *d = &r->tools[i];
        if (d->core || !np_tool_eligible(d, budget_cap)) continue;
        int rank = np_tool_relevance(d, query) * 2 + d->usage_score - d->fail_streak * 10;
        cands[nc].idx = i; cands[nc].rank = rank; nc++;
    }
    for (size_t a = 0; a < nc; ++a) {
        for (size_t b = a + 1; b < nc; ++b) {
            if (cands[b].rank > cands[a].rank) {
                cand_t t = cands[a]; cands[a] = cands[b]; cands[b] = t;
            }
        }
    }
    for (size_t k = 0; k < nc && written < max_tools; ++k) {
        snprintf(out_ids[written++], 80, "%s", r->tools[cands[k].idx].id);
    }
    return written;
}
