/* np_toolinstall.h — S1: T3 自动下载与完整性校验 / T4 隔离安装与配置生成
 *                    / T5 健康检查与能力探测 / T7 失败降级与离线兜底.
 *
 * 文档要求（S1 · T 系列）：
 *  - T3: 断点续传 + SHA-256 / 签名校验；不匹配即隔离、重试一次、仍失败则报告，
 *        绝不执行未验包。
 *  - T4: 安装到用户数据目录（%LOCALAPPDATA%/RapidEditor/tools）；仅进程级
 *        环境注入，不改全局 PATH / 注册表。
 *  - T5: 启动自检：版本、依赖（如 JDK）、支持架构；"装上了≠能跑"。
 *  - T7: 优先用缓存/内置 Capstone；再降级系统 objdump；再提示离线（企业镜像）。
 *
 * 依赖：np_toolchain(T1/T2 白名单)、np_integrity(SHA-256)。
 * 下载本体复用 np_net_download（HTTP 传输与 T3 校验解耦：本模块先算
 * SHA-256 再落盘生效，不匹配的文件只进隔离区）。
 */
#ifndef NP_TOOLINSTALL_H
#define NP_TOOLINSTALL_H

#include <stddef.h>
#include <stdbool.h>

#include "np_toolchain.h"
#include "np_contract.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 安装根目录（T4）。传 NULL 用默认（Windows: %LOCALAPPDATA%/RapidEditor/tools；
 * 其他: ~/.rapideditor/tools）。返回值指向内部静态缓冲，勿释放。 */
const char *np_ti_install_root(const char *user_override);

/* ------------------------------------------------------------------ */
/* T3 下载计划与完整性校验                                              */
/* ------------------------------------------------------------------ */

typedef enum {
    NP_TI_DL_OK = 0,
    NP_TI_DL_ERR_URL_NOT_ALLOWED,   /* 非 HTTPS 或域名不在白名单 */
    NP_TI_DL_ERR_HASH_MISMATCH,     /* 已隔离，绝不执行 */
    NP_TI_DL_ERR_NO_EXPECTED_HASH,  /* 白名单条目未提供期望哈希 → 拒绝下载 */
    NP_TI_DL_ERR_NETWORK,           /* 下载失败（可重试一次） */
    NP_TI_DL_ERR_QUARANTINED,       /* 重试后仍失败，文件留在隔离区 */
    NP_TI_DL_ERR_IO
} np_ti_dl_status_t;

const char *np_ti_dl_status_name(np_ti_dl_status_t s);

typedef struct {
    char url[256];
    char dest_path[260];        /* 安装目录下的目标包路径 */
    char expected_sha256[65];   /* 空 = T3 拒绝下载 */
    uint64_t approx_size_kb;
    int  attempts;              /* out: 实际尝试次数（1 或 2） */
} np_ti_download_t;

/* T3 计划生成：按白名单条目构造下载计划（校验 URL 白名单）。 */
np_ti_dl_status_t np_ti_plan_download(const np_tool_entry_t *e,
                                      const char *install_root,
                                      np_ti_download_t *plan);

/* T3 校验落盘文件：SHA-256 匹配 → OK；不匹配 → 移入隔离区 quarantine/ 并
 * 返回 HASH_MISMATCH；重试逻辑由调用方驱动（本函数幂等）。
 * file 是已下载的临时包路径。 */
np_ti_dl_status_t np_ti_verify_package(const char *file,
                                       const char *expected_sha256,
                                       const char *install_root,
                                       char *quarantined_path, size_t cap);

/* T3 全流程（网络可用时）：白名单 URL 检查 → 下载 → 校验 → 隔离/放行。
 * downloader 注入以便单测（传 NULL 用 np_net_download 真下载）。
 * downloader 返回 false 表示网络失败。 */
np_ti_dl_status_t np_ti_download_and_verify(
        np_ti_download_t *plan, const char *install_root,
        bool (*downloader)(const char *url, const char *dest, void *user),
        void *user, char *final_path, size_t cap);

/* ------------------------------------------------------------------ */
/* T4 隔离安装                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    char tool_dir[260];        /* <root>/<tool-id>/<version>/                */
    char bin_path[260];        /* tool_dir + bin_rel                          */
    char env_patch[520];       /* 进程级 PATH 前缀（分号分隔，Windows 风格）  */
    bool installed;
} np_ti_install_result_t;

/* T4：为已通过校验的包计算隔离安装路径与进程级环境补丁。
 * bin_rel 形如 "ghidra_11.x/support/analyzeHeadless.bat"。
 * 本函数只规划路径，不移动文件（解包由各工具专属逻辑完成，Z 尚未实现）。 */
bool np_ti_plan_install(const char *install_root, const np_tool_entry_t *e,
                        const char *bin_rel, np_ti_install_result_t *out);

/* ------------------------------------------------------------------ */
/* T5 健康检查                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    bool  runnable;              /* 能执行并退出码 0 */
    char  version[64];           /* --version 首行（截断） */
    char  error[160];
} np_ti_health_t;

/* T5：运行 `<bin> <version_arg>` 判断"装上了且能跑"。
 * version_arg 典型 "--version"。阻塞执行（进程级超时由调用方保证）。 */
bool np_ti_health_check(const char *bin_path, const char *version_arg,
                        np_ti_health_t *out);

/* ------------------------------------------------------------------ */
/* T7 失败降级链                                                        */
/* ------------------------------------------------------------------ */

typedef enum {
    NP_TI_TIER_BUNDLED = 0,   /* 内置 Capstone / 缓存        */
    NP_TI_TIER_INSTALLED,     /* T4 隔离安装的工具           */
    NP_TI_TIER_SYSTEM,        /* 系统 objdump               */
    NP_TI_TIER_OFFLINE_HINT,  /* 全部失败 → 提示企业镜像/离线 */
    NP_TI_TIER_NONE
} np_ti_tier_t;

const char *np_ti_tier_name(np_ti_tier_t t);

typedef struct {
    np_ti_tier_t tier;
    char         tool_id[48];    /* 选中工具（tier 对应）        */
    char         bin_path[260];
    char         reason[160];
} np_ti_pick_t;

/* T7：为某任务目的（purpose 如 "disasm"/"decompile"）与目标架构挑选可用层。
 * bundled_ok=true 表示内置能力可用；installed 工具需通过健康检查；
 * system_only 给出系统工具名（如 "objdump"），探测其在 PATH 是否存在。
 * 这是纯决策函数：不下载、不安装，只选层并解释原因。 */
bool np_ti_pick(const np_platform_t *plat, const char *purpose,
                np_arch_t need_arch, bool bundled_ok,
                const char *system_tool, np_ti_pick_t *out);

#ifdef __cplusplus
}
#endif
#endif /* NP_TOOLINSTALL_H */
