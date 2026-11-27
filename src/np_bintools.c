/* np_bintools.c — X8: 原生二进制与格式编辑工具集（实现）.
 * 纯 C99、UI-free；5 只读 + 1 写（三门禁、不裸写）。
 */
#include "np_bintools.h"
#include "np_integrity.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ---------- 工具 ---------- */
static void fscpy(char *dst, size_t cap, const char *src)
{
    size_t i = 0;
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    while (i + 1 < cap && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static bool appendf(char *out, size_t cap, size_t *used, const char *fmt, ...)
{
    va_list ap;
    int n;
    if (*used >= cap) return false;
    va_start(ap, fmt);
    n = vsnprintf(out + *used, cap - *used, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap - *used) return false;
    *used += (size_t)n;
    return true;
}

const char *np_bt_status_name(np_bt_status_t s)
{
    switch (s) {
    case NP_BT_OK: return "ok";
    case NP_BT_ERR_ARG: return "bad-args";
    case NP_BT_ERR_RANGE: return "range";
    case NP_BT_ERR_NO_HIT: return "no-hit";
    case NP_BT_ERR_STALE_REVISION: return "stale-revision";
    case NP_BT_ERR_HASH_MISMATCH: return "hash-mismatch";
    case NP_BT_ERR_DENIED: return "denied";
    case NP_BT_ERR_FULL: return "full";
    case NP_BT_ERR_UNSUPPORTED: return "unsupported";
    }
    return "?";
}

/* ---------- editor.binary.read ---------- */
np_bt_status_t np_bt_binary_read(const unsigned char *data, size_t len,
                                 size_t offset, size_t nbytes,
                                 const char *source, uint64_t revision,
                                 char *out, size_t cap,
                                 np_citation_t *out_cite)
{
    size_t u = 0, i;
    if (!data || !out || cap == 0) return NP_BT_ERR_ARG;
    if (offset > len || nbytes > len - offset) return NP_BT_ERR_RANGE;
    out[0] = '\0';
    /* hexdump：每行 offset + 16 字节 hex + ASCII */
    for (i = 0; i < nbytes; i += 16) {
        size_t j, row = (nbytes - i < 16) ? nbytes - i : 16;
        if (!appendf(out, cap, &u, "%08lx  ", (unsigned long)(offset + i)))
            return NP_BT_ERR_FULL;
        for (j = 0; j < 16; j++) {
            if (j < row) {
                if (!appendf(out, cap, &u, "%02x ", data[offset + i + j]))
                    return NP_BT_ERR_FULL;
            } else {
                if (!appendf(out, cap, &u, "   ")) return NP_BT_ERR_FULL;
            }
        }
        if (!appendf(out, cap, &u, " |")) return NP_BT_ERR_FULL;
        for (j = 0; j < row; j++) {
            unsigned char c = data[offset + i + j];
            if (!appendf(out, cap, &u, "%c",
                         (c >= 0x20 && c < 0x7f) ? (char)c : '.'))
                return NP_BT_ERR_FULL;
        }
        if (!appendf(out, cap, &u, "|\n")) return NP_BT_ERR_FULL;
    }
    if (out_cite) {
        memset(out_cite, 0, sizeof(*out_cite));
        out_cite->kind = NP_CITE_OFFSET;
        fscpy(out_cite->file, sizeof(out_cite->file), source ? source : "");
        out_cite->start = offset;
        out_cite->end = offset + nbytes;
        out_cite->revision = revision;
        out_cite->excerpt_hash = np_crc32(data + offset, nbytes);
    }
    return NP_BT_OK;
}

/* ---------- editor.binary.search ---------- */
np_bt_status_t np_bt_binary_search(const unsigned char *data, size_t len,
                                   const unsigned char *needle, size_t nlen,
                                   size_t out_offsets[], size_t out_cap,
                                   size_t *n_hits, bool *truncated)
{
    size_t i;
    if (!data || !needle || !nlen || !out_offsets || !out_cap || !n_hits)
        return NP_BT_ERR_ARG;
    *n_hits = 0;
    if (truncated) *truncated = false;
    if (nlen > len) return NP_BT_ERR_NO_HIT;
    for (i = 0; i + nlen <= len; i++) {
        if (memcmp(data + i, needle, nlen) == 0) {
            if (*n_hits < out_cap) out_offsets[*n_hits] = i;
            else if (truncated) *truncated = true;
            (*n_hits)++;
        }
    }
    return *n_hits ? NP_BT_OK : NP_BT_ERR_NO_HIT;
}

/* ---------- 路径白名单（安全红线） ---------- */
bool np_bt_path_allowed(const char *target,
                        const char *const *prefixes, size_t n_prefix)
{
    size_t i;
    const char *p;
    if (!target || !*target || !prefixes || n_prefix == 0) return false;
    /* 绝对路径 / 盘符 / .. 穿越一律拒绝 */
    if (target[0] == '/' || target[0] == '\\') return false;
    if (strlen(target) >= 2 && target[1] == ':') return false;
    for (p = target; *p; p++) {
        if (p[0] == '.' && p[1] == '.' &&
            (p == target || p[-1] == '/' || p[-1] == '\\')) return false;
    }
    for (i = 0; i < n_prefix; i++) {
        const char *pre = prefixes[i];
        size_t n;
        if (!pre || !*pre) continue;
        n = strlen(pre);
        if (strlen(target) < n) continue;
        if (strncmp(target, pre, n) == 0) return true;
    }
    return false;
}

/* ---------- editor.binary.patch（三门禁，不裸写） ---------- */
np_bt_status_t np_bt_binary_patch(const unsigned char *data, size_t len,
                                  const np_bt_write_policy_t *policy,
                                  np_patch_set_t *ps,
                                  size_t offset, size_t old_len,
                                  const unsigned char *bytes, size_t bytes_len)
{
    uint32_t crc;
    if (!data || !policy || !ps || !policy->target) return NP_BT_ERR_ARG;
    /* 门禁 1：路径白名单（空表=fail-closed） */
    if (!np_bt_path_allowed(policy->target, policy->allow_prefix,
                            policy->n_prefix)) return NP_BT_ERR_DENIED;
    /* 门禁 2：expected_hash（当前内容 CRC32 必须一致） */
    crc = np_crc32(data, len);
    if (crc != policy->expected_crc32) return NP_BT_ERR_HASH_MISMATCH;
    /* 门禁 3：base_revision（A3 patch 契约） */
    if (policy->base_revision != np_patch_set_revision(ps))
        return NP_BT_ERR_STALE_REVISION;
    /* 范围校验 */
    if (offset > len || old_len > len - offset ||
        (bytes_len && !bytes)) return NP_BT_ERR_RANGE;
    /* 只把 hunk 加入 patch set，不裸写 buffer/磁盘 */
    if (np_patch_add_byte(ps, NP_OP_REPLACE, offset, old_len,
                          bytes, bytes_len) != NP_PATCH_OK)
        return NP_BT_ERR_FULL;
    return NP_BT_OK;
}

/* ---------- editor.binary.disasm ---------- */
np_bt_status_t np_bt_binary_disasm(const np_bt_insn_view_t *insns, size_t n,
                                   size_t start_insn, size_t end_insn,
                                   char *out, size_t cap)
{
    size_t u = 0, i;
    if (!insns || !out || cap == 0) return NP_BT_ERR_ARG;
    if (start_insn > end_insn || end_insn >= n) return NP_BT_ERR_RANGE;
    out[0] = '\0';
    for (i = start_insn; i <= end_insn; i++) {
        /* 视图只带地址/offset/size（文本由调用方另附 np_disasm_at）；
         * 此处输出可定位的指令元数据，供模型引用 offset 回跳。 */
        if (!appendf(out, cap, &u, "#%lu @0x%llx off=0x%lx size=%lu\n",
                     (unsigned long)i, (unsigned long long)insns[i].addr,
                     (unsigned long)insns[i].offset,
                     (unsigned long)insns[i].size)) return NP_BT_ERR_FULL;
    }
    return NP_BT_OK;
}

/* ---------- editor.format.tree ---------- */
np_bt_status_t np_bt_format_tree(const np_bt_tree_node_t *nodes, size_t n,
                                 size_t max_nodes, char *out, size_t cap,
                                 np_citation_t *out_cite, const char *source)
{
    size_t u = 0, i;
    size_t lim = n < max_nodes ? n : max_nodes;
    if (!nodes || !out || cap == 0) return NP_BT_ERR_ARG;
    out[0] = '\0';
    for (i = 0; i < lim; i++) {
        size_t k;
        if (!appendf(out, cap, &u, "%*s%s · %s · %s\n",
                     (int)(nodes[i].depth * 2), "",
                     nodes[i].type, nodes[i].path, nodes[i].label))
            return NP_BT_ERR_FULL;
        (void)k;
    }
    if (out_cite) {
        memset(out_cite, 0, sizeof(*out_cite));
        out_cite->kind = NP_CITE_STRUCT;
        fscpy(out_cite->file, sizeof(out_cite->file), source ? source : "");
        if (lim) fscpy(out_cite->path, sizeof(out_cite->path), nodes[0].path);
    }
    return NP_BT_OK;
}

/* ---------- editor.format.query_readonly ---------- */
np_bt_status_t np_bt_format_query_readonly(const char *result_md,
                                           char *out, size_t cap,
                                           np_citation_t *out_cite,
                                           const char *source)
{
    if (!result_md || !*result_md || !out || cap == 0) return NP_BT_ERR_ARG;
    /* 只读门禁：必须带 F6 结果 provenance 标记，防止把任意文本当查询结果 */
    if (!strstr(result_md, "结构化查询结果")) return NP_BT_ERR_UNSUPPORTED;
    fscpy(out, cap, result_md);
    if (out_cite) {
        memset(out_cite, 0, sizeof(*out_cite));
        out_cite->kind = NP_CITE_STRUCT;
        fscpy(out_cite->file, sizeof(out_cite->file), source ? source : "");
        fscpy(out_cite->path, sizeof(out_cite->path), "query_result");
    }
    return NP_BT_OK;
}

/* ---------- 注册 ---------- */
size_t np_bintools_register(np_registry_t *reg)
{
    np_tool_desc_t d;
    size_t n = 0;
    if (!reg) return 0;
#define REG(tool_id, tool_desc, tool_risk, tool_budget, tool_scope) do { \
        memset(&d, 0, sizeof(d)); \
        snprintf(d.id, sizeof(d.id), "%s", (tool_id)); \
        d.kind = NP_TOOL_NATIVE; d.core = true; d.budget = (tool_budget); \
        d.risk = (tool_risk); d.health = true; d.authorized = true; \
        d.scopes = (tool_scope); \
        snprintf(d.description, sizeof(d.description), "%s", (tool_desc)); \
        if (np_registry_register(reg, &d)) n++; \
    } while (0)
    REG("editor.binary.read", "read a byte range as bounded hexdump with offset citation",
        NP_RISK_READONLY, 1, NP_SCOPE_BINARY);
    REG("editor.binary.search", "search a byte sequence, returns bounded offsets",
        NP_RISK_READONLY, 1, NP_SCOPE_BINARY);
    REG("editor.binary.patch", "stage a byte patch via whitelist+expected_hash+revision gates",
        NP_RISK_WRITE, 2, NP_SCOPE_BINARY);
    REG("editor.binary.disasm", "read decoded instructions (offset/addr) from R2 view",
        NP_RISK_READONLY, 1, NP_SCOPE_BINARY);
    REG("editor.format.tree", "read format structure tree from parsed OOXML/DB result",
        NP_RISK_READONLY, 1, NP_SCOPE_FORMAT);
    REG("editor.format.query_readonly", "wrap F6 read-only query result with citation",
        NP_RISK_READONLY, 1, NP_SCOPE_FORMAT);
#undef REG
    return n;
}
