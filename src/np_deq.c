/* np_deq.c - DC9/DC5 staged equivalence gate (see header). */
#include "np_deq.h"
#include "np_integrity.h"       /* np_crc32 for golden hash */

#include <stdio.h>
#include <string.h>

const char *np_deq_level_name(np_deq_level_t l) {
    switch (l) {
        case NP_DEQ_V0_REASSEMBLY: return "V0 重汇编字节比对";
        case NP_DEQ_V1_RECOMPILE:  return "V1 重编译指令比对";
        case NP_DEQ_V2_BEHAVIOUR:  return "V2 隔离行为差分";
        case NP_DEQ_V3_HUMAN:      return "V3 人工审查";
        default:                   return "未知级别";
    }
}
const char *np_deq_verdict_name(np_deq_verdict_t v) {
    switch (v) {
        case NP_DEQ_EQUIVALENT:    return "等价";
        case NP_DEQ_NOT_EQUIVALENT:return "不等价";
        default:                   return "未验证";
    }
}

static void scpy(char *d, size_t cap, const char *s) {
    if (!cap) return;
    if (!s) { d[0] = 0; return; }
    size_t i = 0;
    for (; i + 1 < cap && s[i]; ++i) d[i] = s[i];
    d[i] = 0;
}

np_deq_golden_t np_deq_make_golden(const char *arch, const char *func,
                                   const char *const *points, size_t n) {
    np_deq_golden_t g;
    memset(&g, 0, sizeof g);
    scpy(g.arch, sizeof g.arch, arch ? arch : "");
    scpy(g.func, sizeof g.func, func ? func : "");
    g.nkeypoints = n < 8 ? n : 8;
    char canon[8 * 96 + 16];
    canon[0] = 0;
    size_t at = 0;
    for (size_t i = 0; i < g.nkeypoints; ++i) {
        const char *p = points ? points[i] : "";
        scpy(g.keypoints[i], sizeof g.keypoints[i], p);
        size_t pl = strlen(p);
        if (at + pl + 2 < sizeof canon) {
            memcpy(canon + at, p, pl); at += pl;
            canon[at++] = '|';
        }
    }
    canon[at] = 0;
    g.hash = np_crc32(canon, at);
    return g;
}

bool np_deq_match_golden(const np_deq_golden_t *g,
                         const char *const *candidate_points, size_t n) {
    if (!g || g->nkeypoints == 0) return false;
    for (size_t i = 0; i < g->nkeypoints; ++i) {
        const char *want = g->keypoints[i];
        bool found = false;
        for (size_t k = 0; k < n; ++k) {
            if (candidate_points[k] && strstr(candidate_points[k], want)) {
                found = true; break;
            }
        }
        if (!found) return false;       /* every golden key point must appear */
    }
    return true;
}

np_deq_result_t np_deq_evaluate(np_deq_level_t required,
                                const char *original, const char *candidate,
                                np_deq_check_fn check, void *user,
                                const np_deq_golden_t *golden,
                                const char *const *candidate_points, size_t npoints,
                                const char *known_gaps) {
    np_deq_result_t r;
    memset(&r, 0, sizeof r);
    r.verdict = NP_DEQ_UNVERIFIED;
    r.required_level = required > NP_DEQ_V3_HUMAN ? NP_DEQ_V3_HUMAN : required;
    r.achieved_level = NP_DEQ_V0_REASSEMBLY;   /* nothing achieved yet */
    scpy(r.known_gaps, sizeof r.known_gaps, known_gaps);

    /* Golden pin is mandatory for EQUIVALENT. */
    r.golden_matched = golden && np_deq_match_golden(golden, candidate_points, npoints);

    if (!check || !original || !candidate) {
        scpy(r.known_gaps, sizeof r.known_gaps,
             "无注入验证器/输入缺失，无法验证任何级别");
        return r;                        /* UNVERIFIED, fail-closed */
    }

    for (int l = 0; l <= (int)r.required_level; ++l) {
        np_deq_check_t c = check((np_deq_level_t)l, original, candidate, user);
        c.level = (np_deq_level_t)l;
        r.checks[r.nchecks++] = c;
        if (!c.passed) {
            r.verdict = NP_DEQ_NOT_EQUIVALENT;
            r.achieved_level = (np_deq_level_t)(l > 0 ? l - 1 : 0);
            /* Record why at the first failing level. */
            char note[320];
            snprintf(note, sizeof note, "%s 未通过：%s",
                     np_deq_level_name((np_deq_level_t)l), c.detail);
            if (r.known_gaps[0]) strncat(r.known_gaps, "; ", sizeof r.known_gaps - strlen(r.known_gaps) - 1);
            strncat(r.known_gaps, note, sizeof r.known_gaps - strlen(r.known_gaps) - 1);
            return r;                    /* stop: fail-closed */
        }
        r.achieved_level = (np_deq_level_t)l;
    }

    /* All required levels passed, but still need golden pin. */
    r.verdict = r.golden_matched ? NP_DEQ_EQUIVALENT : NP_DEQ_UNVERIFIED;
    if (!r.golden_matched && !r.known_gaps[0])
        scpy(r.known_gaps, sizeof r.known_gaps, "未匹配 golden 函数样本，不得标等价");
    return r;
}

void np_deq_summary(const np_deq_result_t *r, char *buf, size_t cap) {
    if (!r || !buf || cap == 0) return;
    snprintf(buf, cap, "%s · 要求至 %s · 达到 %s · golden %s%s",
             np_deq_verdict_name(r->verdict),
             np_deq_level_name(r->required_level),
             np_deq_level_name(r->achieved_level),
             r->golden_matched ? "匹配" : "未匹配",
             r->known_gaps[0] ? " · 缺口见记录" : "");
}
