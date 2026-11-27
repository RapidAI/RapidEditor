/* np_patchimpact.h — R6: 补丁影响分析（字节改动 → 指令/函数/块）.
 *
 * 文档要求（R6 · S2 · P1）：改一个字节会影响哪条指令、哪个函数不可见。
 * H7 字节 Diff 旁联动反汇编 before/after；高亮受影响函数与基本块；
 * 与 H2 校验守卫串联。
 *
 * 设计：消费 H7 的 byte hunks + before/after 两组已解码指令与 R3 CFG，
 * 把每个字节区间改动映射到"哪些指令被直接改写/因位移而失效/在边界处
 * 被新增或删除"，再归并到受影响的基本块与函数，标注影响级别；
 * 同时用 H2 CRC32/SHA256 校验两份输入的完整性摘要（防对照版本被篡改）。
 *
 * 安全红线 / 诚实边界：
 *  - 只读：不修改 before/after 任何字节；不改 diff hunks；
 *  - 影响映射是<b>静态启发式</b>（L1），位移后的指令需重新解码才准确，
 *    本模块只对已提供的 before/after 指令做区间匹配，不猜测未解码内容；
 *  - 校验摘要只证明"这两份字节的哈希"，不证明补丁语义正确；
 *  - 报告带 provenance（两端 CRC32/SHA256、hunk 数、受影响数）。
 */
#ifndef NP_PATCHIMPACT_H
#define NP_PATCHIMPACT_H

#include "np_bytediff.h"
#include "np_cfg.h"
#include "np_disasm.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NP_PI_MAX_AFFECTED  128
#define NP_PI_MAX_SUMMARY   24576
#define NP_PI_MAX_LABEL     160
#define NP_PI_MAX_HASH      80

typedef enum {
    NP_PI_OK = 0,
    NP_PI_ERR_ARG,
    NP_PI_ERR_FULL,
    NP_PI_ERR_NODIFF
} np_pi_status_t;
const char *np_pi_status_name(np_pi_status_t s);

/* 影响级别 */
typedef enum {
    NP_PI_DIRECT = 0,     /* 字节直接落在指令内 → 该指令语义改变 */
    NP_PI_SHIFTED,        /* 插入/删除导致后续指令 offset 位移（需重解码） */
    NP_PI_BOUNDARY,       /* 改动在指令边界（新增/删除整条指令） */
    NP_PI_FUNC_UNCERTAIN  /* 受影响区域无对应函数/块，位置不确定 */
} np_pi_level_t;
const char *np_pi_level_name(np_pi_level_t l);

/* 单条受影响指令 */
typedef struct {
    np_pi_level_t level;
    size_t   insn_index;       /* 在 before 指令数组中的下标（SHIFTED 可为 SIZE_MAX） */
    uint64_t before_addr;
    uint64_t after_addr;       /* after 中对应指令地址（若可匹配） */
    size_t   byte_off;         /* 触发改动的字节偏移 */
    size_t   affected_bytes;   /* 受影响字节数 */
    int      func_index;       /* 所属函数下标；-1 未知 */
    size_t   block_id;         /* 所属基本块；SIZE_MAX 未知 */
    char     label[NP_PI_MAX_LABEL];
} np_pi_affected_t;

/* 完整性摘要（H2 串联） */
typedef struct {
    uint32_t crc_before, crc_after;
    char     sha_before[NP_PI_MAX_HASH];
    char     sha_after[NP_PI_MAX_HASH];
    bool     verified;          /* diff replay 与 after 逐字节一致（H7 verify） */
} np_pi_digest_t;

typedef struct {
    int      naffected;
    np_pi_affected_t affected[NP_PI_MAX_AFFECTED];
    int      nfuncs_hit;
    int      nblocks_hit;
    size_t   nhunks;
    bool     truncated;
    np_pi_digest_t digest;
    char     source_before[NP_PI_MAX_LABEL];
    char     source_after[NP_PI_MAX_LABEL];
} np_pi_result_t;

/* 分析补丁影响：
 *  before/after 为两版本字节；res 为 np_bytediff_compute 的结果；
 *  ins_before/n_before 与 ins_after/n_after 为两端已解码指令（可为 NULL）；
 *  cfg_before/cfg_after 为两端 CFG（可为 NULL）。所有输入只读。
 */
np_pi_status_t np_pi_analyze(const void *before, size_t blen,
                             const void *after, size_t alen,
                             const np_bd_result_t *diff,
                             const np_disasm_insn_t *ins_before, size_t n_before,
                             const np_disasm_insn_t *ins_after, size_t n_after,
                             const np_cfg_graph_t *cfg_before,
                             const np_cfg_graph_t *cfg_after,
                             const char *source_before, const char *source_after,
                             np_pi_result_t *out);

/* 渲染为 Markdown 影响报告（含 digest provenance、受影响指令/函数/块、诚实边界）。 */
int np_pi_render(const np_pi_result_t *r, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* NP_PATCHIMPACT_H */
