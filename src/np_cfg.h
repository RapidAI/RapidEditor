/* np_cfg.h — R3: 函数边界与控制流图（轻量启发式）.
 *
 * 文档要求（R3 · S2 · P0）：线性指令流读不出函数与分支结构。
 * 轻量函数边界识别 + 基本块 CFG；节点/边可点回 offset。
 *
 * 设计：消费 R2 已解码的 np_disasm_insn_t[]（不重新解码字节），
 * 通过注入式分类器把每条指令归为 CALL/JMP/条件跳转/RET/其他，
 * 再按启发式识别函数起点并划分基本块、构建 CFG 边。
 *
 * 安全红线 / 诚实边界：
 *  - 本模块<b>只读</b>指令数组，不改任何字节；
 *  - 函数边界与 CFG 均为<b>启发式</b>（L1 工具推导），每个结论带置信度，
 *    架构未确认或无解码器时不猜测；
 *  - 不做真实反编译/符号解析，函数仅按 prologue/call/jmp 目标/ret 界定，
 *    可能被编译器优化打乱；节点/边携带 addr 与 insn_index 供回跳到 R2/Hex；
 *  - CUI canvas 自绘由 UI 层负责，本模块只产出可渲染的图模型 + Mermaid 文本。
 */
#ifndef NP_CFG_H
#define NP_CFG_H

#include "np_disasm.h"
#include "np_probe.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NP_CFG_MAX_FUNCS   64
#define NP_CFG_MAX_BLOCKS  512
#define NP_CFG_MAX_EDGES   1024
#define NP_CFG_MAX_NAME    96
#define NP_CFG_MAX_LABEL   200
#define NP_CFG_MAX_MERMAID 32768

typedef enum {
    NP_CFG_OK = 0,
    NP_CFG_ERR_ARG,
    NP_CFG_ERR_FULL,
    NP_CFG_ERR_NOINSN,
    NP_CFG_ERR_UNSUPPORTED
} np_cfg_status_t;
const char *np_cfg_status_name(np_cfg_status_t s);

/* 指令分类（注入分类器的输出） */
typedef enum {
    NP_IC_OTHER = 0,
    NP_IC_CALL,          /* call / bl / jal */
    NP_IC_JMP,           /* 无条件跳转 jmp / b / br */
    NP_IC_JCC,           /* 条件跳转 je/jne/bne/blt... */
    NP_IC_RET,           /* ret / bx lr */
    NP_IC_PROLOGUE,      /* push rbp; mov rbp,rsp / stp x29,x30 */
    NP_IC_NOP            /* nop / 填充 */
} np_cfg_iclass_t;

/* 内置 x86/x64 文本分类器（基于助记符文本，够用于 objdump 输出）。
 * 其它架构可注入自定义分类器。返回分类 + 可选的目标地址（jmp/call 的立即数，
 * 解析不出用 0 表示未知）。 */
typedef struct {
    np_cfg_iclass_t cls;
    uint64_t target;     /* 跳转/调用目标虚拟地址；0 或 unknown 时忽略 */
    bool target_valid;
} np_cfg_itype_t;

typedef np_cfg_itype_t (*np_cfg_classify_fn)(const np_disasm_insn_t *ins,
                                             void *user);

/* 函数：起点 insn 索引 + 地址 + 名称（按地址命名，不解析符号）+ 置信度 */
typedef struct {
    size_t  start_insn;
    size_t  end_insn;    /* 含 */
    uint64_t start_addr;
    uint64_t end_addr;
    char    name[NP_CFG_MAX_NAME];
    int     conf;        /* 0..100 启发式置信度 */
    int     nblocks;
} np_cfg_func_t;

/* 基本块：一段连续指令 [start_insn, end_insn]，含入口/出口地址 */
typedef struct {
    size_t  id;
    size_t  start_insn;
    size_t  end_insn;
    uint64_t start_addr;
    uint64_t end_addr;
    int     func_index;  /* 所属函数下标；-1 未归属 */
    char    label[NP_CFG_MAX_LABEL]; /* 摘要文本 */
} np_cfg_block_t;

/* CFG 边：from→to 基本块 id（SIZE_MAX 表示到块外/函数外） */
typedef enum {
    NP_CFG_E_FALLTHROUGH = 0,  /* 顺序流 */
    NP_CFG_E_JCC_TAKEN,        /* 条件跳转成立 */
    NP_CFG_E_JMP,              /* 无条件跳转 */
    NP_CFG_E_CALL,             /* 调用（节点可能为块外） */
    NP_CFG_E_RET               /* 返回（到块外） */
} np_cfg_edge_kind_t;

typedef struct {
    size_t from;         /* 基本块 id */
    size_t to;           /* 基本块 id，或 SIZE_MAX 表示外部 */
    np_cfg_edge_kind_t kind;
    uint64_t target_addr;/* 跳转/调用目标（若已知） */
    size_t   target_insn;/* 目标指令索引（SIZE_MAX 未知） */
} np_cfg_edge_t;

typedef struct {
    int     nfuncs;
    np_cfg_func_t funcs[NP_CFG_MAX_FUNCS];
    int     nblocks;
    np_cfg_block_t blocks[NP_CFG_MAX_BLOCKS];
    int     nedges;
    np_cfg_edge_t edges[NP_CFG_MAX_EDGES];
    size_t  ninstructions;
    bool    truncated;
    np_parch_t arch;
} np_cfg_graph_t;

/* 构建 CFG：insns 为 R2 已解码指令数组（可直接用 np_disasm_at 逐条拷贝）。
 * classify 为 NULL 时用内置 x86/x64 文本分类器（其它架构请注入）。 */
np_cfg_status_t np_cfg_build(const np_disasm_insn_t *insns, size_t n,
                             np_parch_t arch, np_cfg_classify_fn classify,
                             void *user, np_cfg_graph_t *out);

/* 内置 x86/x64 分类器（也可作为注入回调直接传入）。 */
np_cfg_itype_t np_cfg_classify_x86(const np_disasm_insn_t *ins, void *user);

/* 查询：包含 insn 索引的基本块 id（不存在用 SIZE_MAX） */
size_t np_cfg_block_for_insn(const np_cfg_graph_t *g, size_t insn);

/* 查询：包含虚拟地址的基本块 id（不存在用 SIZE_MAX） */
size_t np_cfg_block_for_addr(const np_cfg_graph_t *g, uint64_t addr);

/* 查询：起点为 insn 的函数下标（不存在用 -1） */
int np_cfg_func_for_insn(const np_cfg_graph_t *g, size_t insn);

/* 渲染为 Mermaid flowchart（节点带块 id/起始地址/摘要，边带类型标注）。
 * 容量不足返回 -1；成功返回写入字节数。 */
int np_cfg_render_mermaid(const np_cfg_graph_t *g, char *out, size_t cap);

/* 渲染为文本摘要（函数表 + 每函数基本块数 + 边数），供 UI 快速展示。 */
int np_cfg_render_summary(const np_cfg_graph_t *g, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* NP_CFG_H */
