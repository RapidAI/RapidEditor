/* np_skillmgr.h — S1: X3 Skill/MCP 自动搜索与安装 / X4 手动安装、启停、更新与删除.
 *
 * 文档要求（S1 · X 系列）：
 *  - X3: 能力缺口 → 搜索官方/社区/GitHub/企业镜像 → 下载验包 → 安装注册。
 *        本模块实现"验包+安装注册"与搜索记录结构（搜索源路由由上层完成）。
 *  - X4: 扩展管理器支持本地 SKILL.md / manifest 安装、启停、删除；
 *        状态持久化在用户数据目录，随注册表（np_capability）联动启停。
 *
 * Skill 包形态（与 MaClaw/SkillHub 对齐）：
 *   myskill/
 *     SKILL.md  或 manifest.json   —— 名称/版本/描述/入口/权限声明
 *     ... 资源文件
 * 安装 = 校验 → 拷贝到 <root>/skills/<name>/<version>/ → 记入 installed.json。
 * 安全：manifest 必须声明 sha256（包内容哈希）才允许安装（X3 红线）。
 */
#ifndef NP_SKILLMGR_H
#define NP_SKILLMGR_H

#include <stddef.h>
#include <stdbool.h>

#include "np_contract.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NP_SKILL_MAX 64

typedef enum {
    NP_SKILL_OK = 0,
    NP_SKILL_ERR_NO_MANIFEST,     /* 包内找不到 SKILL.md/manifest.json */
    NP_SKILL_ERR_BAD_MANIFEST,    /* 字段缺失或解析失败 */
    NP_SKILL_ERR_HASH_MISSING,    /* 未声明 sha256 → 拒装 */
    NP_SKILL_ERR_HASH_MISMATCH,
    NP_SKILL_ERR_EXISTS,          /* 同名同版本已安装 */
    NP_SKILL_ERR_FULL,            /* 超过 NP_SKILL_MAX */
    NP_SKILL_ERR_NOT_FOUND,       /* 启停/删除目标不存在 */
    NP_SKILL_ERR_IO
} np_skill_status_t;

const char *np_skill_status_name(np_skill_status_t s);

typedef struct {
    char name[64];
    char version[24];
    char description[160];
    char entry[160];          /* 入口脚本/命令模板 */
    char source_url[256];     /* 来源（GitHub/SkillHub/本地路径） */
    char sha256[65];          /* 内容哈希（安装时校验） */
    bool mcp;                 /* true=MCP 服务器包，false=纯 Skill */
    bool enabled;
} np_skill_t;

typedef struct np_skill_store np_skill_store_t;

/* 创建/销毁。store_dir 为持久化目录（installed.json 所在），NULL 用默认
 * 用户数据目录。 */
np_skill_store_t *np_skill_store_open(const char *store_dir);
void np_skill_store_close(np_skill_store_t *st);

/* X4：从本地目录安装（dir 内含 SKILL.md）。expected_sha256 可为 NULL 表示
 * 本地安装信任目录内容（记录实际哈希）。
 * X3：远程包先由调用方下载+校验哈希，再调本函数。 */
np_skill_status_t np_skill_install(np_skill_store_t *st, const char *dir,
                                   const char *source_url,
                                   const char *expected_sha256,
                                   np_skill_t *out_installed);

/* SKILL.md / manifest.json 最小解析器（供 UI 预览与 X3 搜索结果确认）。 */
np_skill_status_t np_skill_parse_manifest(const char *dir, np_skill_t *out);

/* X4：启停（联动 np_capability 注册表健康位）。 */
np_skill_status_t np_skill_set_enabled(np_skill_store_t *st,
                                       const char *name, bool enabled);

/* X4：删除（目录 + 记录）。 */
np_skill_status_t np_skill_uninstall(np_skill_store_t *st, const char *name);

/* 列举。返回条目数；out_cap 不足时截断。 */
size_t np_skill_list(const np_skill_store_t *st, np_skill_t *out, size_t out_cap);
size_t np_skill_count(const np_skill_store_t *st);
const np_skill_t *np_skill_find(const np_skill_store_t *st, const char *name);

/* X3：能力缺口搜索请求（记录结构；真实搜索走 np_net + SearchProfile）。 */
typedef struct {
    char query[128];         /* 如 "decompile loongarch" */
    char source[48];         /* "skillhub" / "github" / "enterprise-mirror" */
    char result_name[64];
    char result_url[256];
    char result_sha256[65];
    double score;
} np_skill_search_hit_t;

#ifdef __cplusplus
}
#endif
#endif /* NP_SKILLMGR_H */
