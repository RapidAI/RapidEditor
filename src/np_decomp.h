/* np_decomp.h — DC1: 本地反编译后端桥接（统一函数→伪代码+offset 映射）.
 *
 * 文档要求（DC1 · S2）：Ghidra headless / rizin / retdec 本地进程调用；
 * 统一"函数 → 伪代码 + offset 映射"输出；不上传二进制。
 *
 * 安全红线：
 *  - 本模块<b>只读</b>：消费 R2 已解码指令 + R3 CFG，不改任何字节；
 *  - 外部后端通过<b>注入回调</b>接入（本模块不 spawn 进程、不访问网络、不硬链
 *    Ghidra/rizin/retdec），且只传本地文件路径，<b>绝不上传二进制内容</b>；
 *  - 后端输出视为<b>不可信输入</b>：剥离控制字符、有界截断、PII 形态脱敏；
 *  - 所有输出标注<b>候选·待确认</b>，不覆盖原文件，不写回二进制。
 *
 * 诚实边界：内置生成器是自研启发式（mov→赋值/call→调用/jcc→if/jmp→goto/
 * ret→return），不是真正反编译；外部后端的正确性由后端负责，本模块只做
 * 统一封装、清洗与 offset 映射。DC5 等价验证走 np_deq，本模块不声称等价。
 */
#ifndef NP_DECOMP_H
#define NP_DECOMP_H

#include "np_cfg.h"
#include "np_disasm.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NP_DC_MAX_LINES    256
#define NP_DC_MAX_LINE     200
#define NP_DC_MAX_NAME     96
#define NP_DC_MAX_CODE     24576
#define NP_DC_MAX_SOURCE   200
#define NP_DC_MAX_PATH     260

typedef enum {
    NP_DC_OK = 0,
    NP_DC_ERR_ARG,
    NP_DC_ERR_NOCFG,
    NP_DC_ERR_FULL,
    NP_DC_ERR_BACKEND,       /* 注入后端失败 */
    NP_DC_ERR_UNSUPPORTED
} np_dc_status_t;
const char *np_dc_status_name(np_dc_status_t s);

/* 后端来源 */
typedef enum {
    NP_DC_BUILTIN = 0,       /* 内置启发式生成器 */
    NP_DC_GHIDRA,
    NP_DC_RIZIN,
    NP_DC_RETDEC,
    NP_DC_CUSTOM
} np_dc_backend_t;
const char *np_dc_backend_name(np_dc_backend_t b);

/* 伪代码行 + offset 映射（逐行绑定指令偏移，供回跳核验） */
typedef struct {
    int      line_no;        /* 1-based 行号 */
    size_t   offset;         /* 绑定的字节偏移（SIZE_MAX=无对应指令） */
    uint64_t addr;           /* 虚拟地址 */
    char     text[NP_DC_MAX_LINE];
} np_dc_line_t;

typedef struct {
    np_dc_backend_t backend;
    char     source[NP_DC_MAX_SOURCE];   /* 引用协议文件名 */
    char     func_name[NP_DC_MAX_NAME];
    uint64_t func_addr;
    int      conf;                       /* 0..100 置信度（启发式） */
    bool     candidate;                  /* 恒 true：候选·待确认 */
    int      nlines;
    np_dc_line_t lines[NP_DC_MAX_LINES];
    char     code[NP_DC_MAX_CODE];       /* 完整伪代码文本（含 provenance 头） */
} np_dc_result_t;

/* 外部反编译后端回调：给定本地二进制路径与函数名，把伪代码写入 out（有界）。
 * 返回 true 成功。后端只在本地运行；本模块不传二进制内容、不访问网络。
 * user 为后端自有状态（命令模板/进程管理等）。 */
typedef bool (*np_dc_backend_fn)(const char *local_path, const char *func_name,
                                 uint64_t func_addr, char *out, size_t cap,
                                 void *user);

/* 内置启发式生成：从已解码指令数组 + CFG 的函数生成伪代码（只读）。
 * insns/n 为 R2 结果；g 为 R3 结果；func_index 为目标函数下标。 */
np_dc_status_t np_dc_generate_builtin(const np_disasm_insn_t *insns, size_t n,
                                      const np_cfg_graph_t *g, int func_index,
                                      const char *source, np_dc_result_t *out);

/* 外部后端生成：调用注入回调，清洗输出并构建 offset 映射（按行顺序绑定
 * 到函数指令的 offset，超出指令数则 offset=SIZE_MAX）。失败/输出为空时不猜测。 */
np_dc_status_t np_dc_generate_external(np_dc_backend_t backend,
                                       const char *local_path,
                                       const char *func_name, uint64_t func_addr,
                                       np_dc_backend_fn fn, void *user,
                                       const char *source,
                                       np_dc_result_t *out);

/* 查询：包含 offset 的伪代码行号（不存在返回 -1）。 */
int np_dc_line_for_offset(const np_dc_result_t *r, size_t offset);

/* 查询：行号的 offset（越界返回 SIZE_MAX）。 */
size_t np_dc_offset_for_line(const np_dc_result_t *r, int line_no);

/* 不可信输出清洗（控制字符剥离 + 有界截断 + PII 形态脱敏）。 */
void np_dc_sanitize(const char *in, char *out, size_t cap);

/* 渲染 Markdown（带 provenance、候选标注、逐行 offset 映射）。 */
int np_dc_render(const np_dc_result_t *r, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* NP_DECOMP_H */
