/* np_bintools.h — X8: 原生二进制与格式编辑工具集（AI 可调用）.
 *
 * 文档要求（X8 · S2 · P0）：Hex、反汇编、格式结构树与补丁审查之间缺少
 * 可调用接口。editor.binary.read/search/patch/disasm 与
 * editor.format.tree/query_readonly；依赖 H1/H2/F8、expected_hash 与白名单；
 * 禁止 MCP 直接裸写。
 *
 * 安全红线（本模块核心）：
 *  - 6 个工具中 5 个只读；唯一写工具 editor.binary.patch <b>不裸写</b>：
 *    必须同时通过 (1) 路径白名单（allow_prefix 之一前缀命中，空表=拒绝全部；
 *    .. 穿越/绝对路径拒绝）、(2) expected_hash 门禁（当前内容 CRC32 必须等于
 *    调用方声明值，不符即 STALE）、(3) base_revision 门禁（np_patch_set 的
 *    base_revision 必须等于 buffer.revision）——三门禁任一失败即拒绝，
 *    且不修改任何字节；通过后也只把字节 hunk 加入 np_patch_set，由上层
 *    B3/H1 审查提交（本模块不直接落盘、不直接改 buffer）。
 *  - 每个读工具产出 B5 Citation（file:0xOFFSET / file#struct-path），
 *    带 revision 绑定与 excerpt hash。
 *
 * 诚实边界：UI-free 纯逻辑；disasm/tree/query 委托已解码的 R2 指令数组、
 * F2/F3 解析结果与 F6 只读查询（本模块不重新解码/解析），调用方需先提供；
 * format 写回仍走 F8，本模块只做只读格式工具。
 */
#ifndef NP_BINTOOLS_H
#define NP_BINTOOLS_H

#include "np_citation.h"
#include "np_capability.h"     /* np_registry_t */
#include "np_contract.h"
#include "np_patch.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NP_BT_MAX_HITS     64
#define NP_BT_MAX_PATH     260
#define NP_BT_MAX_PREFIX   16
#define NP_BT_MAX_PREFIXLEN 128
#define NP_BT_MAX_HEXDUMP  4096
#define NP_BT_MAX_OUT      8192

typedef enum {
    NP_BT_OK = 0,
    NP_BT_ERR_ARG,
    NP_BT_ERR_RANGE,          /* offset/len 越界 */
    NP_BT_ERR_NO_HIT,
    NP_BT_ERR_STALE_REVISION,
    NP_BT_ERR_HASH_MISMATCH,  /* expected_hash 与当前内容不符 */
    NP_BT_ERR_DENIED,         /* 路径白名单拒绝 / 裸写被拒 */
    NP_BT_ERR_FULL,
    NP_BT_ERR_UNSUPPORTED     /* 未提供已解码指令/解析结果 */
} np_bt_status_t;
const char *np_bt_status_name(np_bt_status_t s);

/* ---------- editor.binary.read ---------- */
/* 读 [offset,offset+len) 的 hexdump（每行 16 字节 + ASCII）。
 * out_cite 可选，成功时填充 file:0xOFFSET 引用（start=offset,end=offset+len）。 */
np_bt_status_t np_bt_binary_read(const unsigned char *data, size_t len,
                                 size_t offset, size_t nbytes,
                                 const char *source, uint64_t revision,
                                 char *out, size_t cap,
                                 np_citation_t *out_cite);

/* ---------- editor.binary.search ---------- */
/* 字节序列搜索（最多 NP_BT_MAX_HITS 个 offset；超出置 truncated）。 */
np_bt_status_t np_bt_binary_search(const unsigned char *data, size_t len,
                                   const unsigned char *needle, size_t nlen,
                                   size_t out_offsets[], size_t out_cap,
                                   size_t *n_hits, bool *truncated);

/* ---------- editor.binary.patch（唯一写工具，三门禁） ---------- */
typedef struct {
    const char *target;          /* 目标路径（白名单校验） */
    uint32_t    expected_crc32;  /* H2 门禁：当前内容 CRC32 必须等于此值 */
    uint64_t    base_revision;   /* A3 门禁：patch_set base_revision */
    const char *const *allow_prefix; /* 白名单前缀数组；NULL/空=拒绝全部 */
    size_t      n_prefix;
} np_bt_write_policy_t;

/* 写工具：把 REPLACE 字节 hunk 加入 ps（不裸写、不改 buffer/磁盘）。
 * 门禁顺序：target 白名单 → expected_crc32 匹配 → base_revision 匹配 →
 * hunk 范围校验 → np_patch_add_byte。任一失败返回对应状态且不产生 hunk。 */
np_bt_status_t np_bt_binary_patch(const unsigned char *data, size_t len,
                                  const np_bt_write_policy_t *policy,
                                  np_patch_set_t *ps,
                                  size_t offset, size_t old_len,
                                  const unsigned char *bytes, size_t bytes_len);

/* ---------- editor.binary.disasm（委托 R2 已解码指令） ---------- */
/* 从已解码指令数组读 [start_insn,end_insn] 的文本（address + mnemonic）。
 * insns/n 由调用方提供（R2 解码结果），本模块不解码字节。 */
typedef struct { uint64_t addr; size_t offset; size_t size; } np_bt_insn_view_t;

np_bt_status_t np_bt_binary_disasm(const np_bt_insn_view_t *insns, size_t n,
                                   size_t start_insn, size_t end_insn,
                                   char *out, size_t cap);

/* ---------- editor.format.tree（委托 F2/F3 解析结果） ---------- */
/* 以缩进文本输出结构节点（type/path/label），最多 max_nodes；
 * nodes 为由调用方从 np_ooxml_doc_t.nodes / np_db_schema_t 展平的视图。 */
typedef struct {
    int      depth;
    char     type[40];
    char     path[200];
    char     label[200];
} np_bt_tree_node_t;

np_bt_status_t np_bt_format_tree(const np_bt_tree_node_t *nodes, size_t n,
                                 size_t max_nodes, char *out, size_t cap,
                                 np_citation_t *out_cite, const char *source);

/* ---------- editor.format.query_readonly（委托 F6 结果文本） ---------- */
/* result_md 为 np_sq_render 已渲染的只读查询结果，本模块只做门禁包装：
 * 非空、带 provenance 标记、有界拷贝，并附结构引用。 */
np_bt_status_t np_bt_format_query_readonly(const char *result_md,
                                           char *out, size_t cap,
                                           np_citation_t *out_cite,
                                           const char *source);

/* ---------- 安全辅助（可单测） ---------- */
/* 路径白名单判定：空表拒绝；前缀须命中且不允许 .. 路径穿越/绝对路径/盘符。 */
bool np_bt_path_allowed(const char *target,
                        const char *const *prefixes, size_t n_prefix);

/* ---------- 注册（X1/X2 路由） ---------- */
/* 注册 6 个 native 工具描述符；read/search/disasm/tree/query 为 READONLY，
 * patch 为 WRITE（仍需三门禁 + B3 审查）。返回成功注册数。 */
size_t np_bintools_register(np_registry_t *reg);

#ifdef __cplusplus
}
#endif
#endif /* NP_BINTOOLS_H */
