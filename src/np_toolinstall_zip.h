#ifndef NP_TOOLINSTALL_ZIP_H
#define NP_TOOLINSTALL_ZIP_H

/* T4 隔离安装：把 T3 已校验的 zip 包解包进隔离工具目录。
 * 独立于 np_toolinstall.c —— 后者被多个单测链接，不应引入 np_zip 依赖
 * （np_zip 依赖 CUI 的 inflate 解码器）。主程序链接本文件。
 *
 * 安全链：入口包必须已过 T3 校验（SHA-256 白名单）→ 本函数用 np_zip_extract
 * 做路径穿越/大小/文件数限制的解包 → 校验 bin_rel 在 tool_dir 内真实存在 →
 * 产出 T4 的 tool_dir / bin_path / env_patch（进程级，不改全局 PATH）。 */

#include "np_toolinstall.h"   /* np_tool_entry_t / np_ti_install_result_t */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NP_TI_ZIP_OK = 0,
    NP_TI_ZIP_ERR_ARG,          /* 参数缺失 */
    NP_TI_ZIP_ERR_UNPACK,       /* zip 损坏 / 超限时拒绝（详见 err） */
    NP_TI_ZIP_ERR_BIN_MISSING,  /* bin_rel 指向的文件解包后不存在 */
    NP_TI_ZIP_ERR_IO
} np_ti_zip_status_t;

const char *np_ti_zip_status_name(np_ti_zip_status_t s);

/* 解包 package_zip 到 <root>/<id>/<version>/，验证 bin_rel 存在后填充 out。
 * strip_root=true：丢掉 zip 里常见的单一顶层目录（GitHub Release 包）。
 * max_total_bytes / max_files 传 0 用内置安全默认（256MB / 2048 文件）。
 * err 可选，接收人话失败原因。返回 NP_TI_ZIP_OK 且 out->installed=true 表示成功。
 * 重复安装同一版本：先清掉旧 tool_dir 再解包（隔离目录内操作，不碰系统）。 */
np_ti_zip_status_t np_ti_install_package(const char *install_root,
        const np_tool_entry_t *e, const char *package_zip, const char *bin_rel,
        bool strip_root, uint64_t max_total_bytes, int max_files,
        np_ti_install_result_t *out, char *err, size_t err_cap);

/* 一键流程：T3 下载计划 → 下载+SHA-256 校验（失败隔离）→ T4 zip 解包安装。
 * downloader 传 NULL 用 np_ti_set_default_downloader 注册的真下载器。
 * 返回值：下载阶段失败映射为 NP_TI_ZIP_ERR_UNPACK（err 里写明 T3 状态名），
 * 解包阶段按 np_ti_install_package 的状态返回。成功后 out->installed=true。
 * 注意：本函数是同步阻塞的（np_net_download 同步），UI 应在后台线程调用。 */
np_ti_zip_status_t np_ti_fetch_and_install(const np_tool_entry_t *e,
        const char *bin_rel, bool strip_root, const char *install_root,
        bool (*downloader)(const char *url, const char *dest, void *user),
        void *user, np_ti_install_result_t *out, char *err, size_t err_cap);

#ifdef __cplusplus
}
#endif

#endif /* NP_TOOLINSTALL_ZIP_H */
