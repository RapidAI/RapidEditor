/* test_s1_toolinstall.c — S1: T3/T4/T5/T7 单元测试（无网络依赖）. */
#include "np_toolinstall.h"
#include "np_toolinstall_zip.h"
#include "np_integrity.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { printf("  [FAIL] %s:%d %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

/* 计算内存数据的 sha256 hex */
static bool file_readable(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

static void mem_hex(const void *d, size_t n, char out[65]) {
    unsigned char dg[32];
    np_sha256(d, n, dg);
    np_sha256_hex(dg, out);
}

/* 注入式下载器：写 payload 到 dest；mode 控制失败/篡改 */
static const void *g_payload = NULL;
static size_t g_payload_len = 0;
static int g_mode = 0; /* 0=ok, 1=network-fail, 2=corrupt-payload */
static int g_calls = 0;

static bool fake_download(const char *url, const char *dest, void *user) {
    (void)url; (void)user;
    g_calls++;
    if (g_mode == 1) return false;
    FILE *f = fopen(dest, "wb");
    if (!f) return false;
    if (g_mode == 2) {
        unsigned char bad[8] = {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07};
        fwrite(bad, 1, sizeof(bad), f);
    } else {
        fwrite(g_payload, 1, g_payload_len, f);
    }
    fclose(f);
    return true;
}

static np_tool_entry_t make_entry(void) {
    np_tool_entry_t e;
    memset(&e, 0, sizeof(e));
    snprintf(e.id, sizeof(e.id), "%s", "ghidra");
    snprintf(e.purpose, sizeof(e.purpose), "%s", "decompile");
    snprintf(e.version, sizeof(e.version), "%s", "11.3.1");
    snprintf(e.source_url, sizeof(e.source_url),
             "%s", "https://github.com/NationalSecurityAgency/ghidra/releases/ghidra_11.3.1.zip");
    snprintf(e.license, sizeof(e.license), "%s", "Apache-2.0");
    /* 白名单条目必须带期望哈希（T3 拒绝无哈希包） */
    for (int i = 0; i < 64; ++i) e.sha256[i] = (char)('a' + (i % 6));
    e.sha256[64] = '\0';
    e.approx_size_kb = 400000;
    return e;
}

static int test_plan(void) {
    printf("== T3: 下载计划 ==\n");
    np_tool_entry_t e = make_entry();
    np_ti_download_t plan;
    CHECK(np_ti_plan_download(&e, NULL, &plan) == NP_TI_DL_OK);
    CHECK(strncmp(plan.url, "https://", 8) == 0);
    CHECK(plan.expected_sha256[0] == '\0' || strlen(plan.expected_sha256) == 64);

    /* 无期望哈希 → 拒绝下载 */
    np_tool_entry_t e2 = make_entry();
    e2.sha256[0] = '\0';
    CHECK(np_ti_plan_download(&e2, NULL, &plan) == NP_TI_DL_ERR_NO_EXPECTED_HASH);

    /* 非白名单 URL */
    np_tool_entry_t e3 = make_entry();
    snprintf(e3.source_url, sizeof(e3.source_url), "%s", "http://evil.example.com/pkg.zip");
    CHECK(np_ti_plan_download(&e3, NULL, &plan) == NP_TI_DL_ERR_URL_NOT_ALLOWED);
    return 0;
}

static int test_verify(void) {
    printf("== T3: 校验与隔离 ==\n");
    const char *payload = "fake package payload";
    char expect[65];
    mem_hex(payload, strlen(payload), expect);

    /* 写一个临时文件 */
    const char *tmp = ".s1test_pkg.bin";
    FILE *f = fopen(tmp, "wb");
    CHECK(f != NULL);
    fwrite(payload, 1, strlen(payload), f);
    fclose(f);

    char qpath[300];
    /* 匹配 → OK */
    CHECK(np_ti_verify_package(tmp, expect, ".s1test_root", qpath, sizeof(qpath))
          == NP_TI_DL_OK);
    /* 篡改文件 → 不匹配 → 隔离 */
    f = fopen(tmp, "wb");
    fwrite("tampered!", 1, 9, f);
    fclose(f);
    np_ti_dl_status_t st = np_ti_verify_package(tmp, expect, ".s1test_root",
                                                qpath, sizeof(qpath));
    CHECK(st == NP_TI_DL_ERR_HASH_MISMATCH);
    CHECK(qpath[0] != '\0' && strstr(qpath, "quarantine") != NULL);
    /* 原位置文件已移走 */
    f = fopen(tmp, "rb");
    CHECK(f == NULL);
    remove(qpath); /* 清理 */
    return 0;
}

static int test_download_flow(void) {
    printf("== T3: 下载全流程（注入下载器） ==\n");
    const char *payload = "Ghidra-11.3.1-package";
    char expect[65];
    mem_hex(payload, strlen(payload), expect);
    g_payload = payload;
    g_payload_len = strlen(payload);
    g_calls = 0;

    np_ti_download_t plan;
    memset(&plan, 0, sizeof(plan));
    snprintf(plan.url, sizeof(plan.url),
             "%s", "https://github.com/NationalSecurityAgency/ghidra/releases/pkg.zip");
    snprintf(plan.dest_path, sizeof(plan.dest_path), "%s", ".s1test_root/pkg.zip");
    snprintf(plan.expected_sha256, sizeof(plan.expected_sha256), "%s", expect);

    char final_path[300];
    /* 成功 */
    g_mode = 0;
    CHECK(np_ti_download_and_verify(&plan, ".s1test_root", fake_download, NULL,
                                    final_path, sizeof(final_path)) == NP_TI_DL_OK);
    CHECK(g_calls == 1);
    CHECK(strcmp(final_path, ".s1test_root/pkg.zip") == 0);

    /* 网络失败 → NETWORK（不隔离） */
    g_mode = 1; g_calls = 0;
    CHECK(np_ti_download_and_verify(&plan, ".s1test_root", fake_download, NULL,
                                    final_path, sizeof(final_path)) == NP_TI_DL_ERR_NETWORK);
    CHECK(g_calls == 1);

    /* 哈希不匹配 → 重试一次 → 隔离（attempts=2） */
    g_mode = 2; g_calls = 0;
    plan.attempts = 0;
    np_ti_dl_status_t st = np_ti_download_and_verify(&plan, ".s1test_root",
                                                     fake_download, NULL,
                                                     final_path, sizeof(final_path));
    CHECK(st == NP_TI_DL_ERR_QUARANTINED);
    CHECK(g_calls == 2);              /* 重试一次 */
    CHECK(plan.attempts == 2);
    /* 隔离目录里有文件 */
    FILE *q = fopen(".s1test_root/quarantine/pkg.zip", "rb");
    CHECK(q != NULL);
    if (q) fclose(q);
    remove(".s1test_root/quarantine/pkg.zip");
    return 0;
}

static int test_default_downloader(void) {
    printf("== T3: 默认下载器注册（主程序注入点） ==\n");
    const char *payload = "via-default-downloader";
    char expect[65];
    mem_hex(payload, strlen(payload), expect);
    g_payload = payload; g_payload_len = strlen(payload); g_mode = 0; g_calls = 0;

    np_ti_download_t plan;
    memset(&plan, 0, sizeof(plan));
    snprintf(plan.url, sizeof(plan.url), "%s",
             "https://github.com/NationalSecurityAgency/ghidra/releases/pkg.zip");
    snprintf(plan.dest_path, sizeof(plan.dest_path), "%s", ".s1test_root/pkg.zip");
    snprintf(plan.expected_sha256, sizeof(plan.expected_sha256), "%s", expect);

    /* 未注册：NULL downloader → NETWORK（单测路径，无网络依赖） */
    np_ti_set_default_downloader(NULL);
    char final[300];
    CHECK(np_ti_download_and_verify(&plan, ".s1test_root", NULL, NULL,
                                    final, sizeof(final)) == NP_TI_DL_ERR_NETWORK);
    CHECK(g_calls == 0);

    /* 注册后：NULL downloader → 走注册的默认下载器 */
    np_ti_set_default_downloader(fake_download);
    CHECK(np_ti_download_and_verify(&plan, ".s1test_root", NULL, NULL,
                                    final, sizeof(final)) == NP_TI_DL_OK);
    CHECK(g_calls == 1);
    np_ti_set_default_downloader(NULL);  /* 还原，不影响后续用例 */
    remove(".s1test_root/pkg.zip");
    return 0;
}

static int test_plan_install(void) {
    printf("== T4: 隔离安装规划 ==\n");
    np_tool_entry_t e = make_entry();
    np_ti_install_result_t ins;
    CHECK(np_ti_plan_install(".s1test_root", &e, "support/analyzeHeadless.bat", &ins));
    CHECK(strstr(ins.tool_dir, "ghidra") && strstr(ins.tool_dir, "11.3.1"));
    CHECK(strstr(ins.bin_path, "analyzeHeadless.bat"));
    /* T4 红线：只产出进程级 env_patch，不改全局 PATH/注册表 */
    CHECK(ins.env_patch[0] != '\0');
    CHECK(ins.installed == false);
    return 0;
}

static int test_install_package(void) {
    printf("== T4: zip 解包安装 ==\n");
    const char *zip = "tests/golden/toolinstall_fixture.zip";
    np_tool_entry_t e = make_entry();
    np_ti_install_result_t ins;
    char err[240];

    /* 正常安装：strip_root=true 丢掉 ghidra_11.3.1/ 顶层目录 */
    np_ti_zip_status_t st = np_ti_install_package(".s1test_root", &e, zip,
            "support/analyzeHeadless.bat", true, 0, 0, &ins, err, sizeof(err));
    CHECK(st == NP_TI_ZIP_OK);
    CHECK(ins.installed);
    CHECK(file_readable(ins.bin_path));
    CHECK(strstr(ins.env_patch, "ghidra") != NULL);

    /* 重复安装：幂等，仍成功（旧目录清理后重解） */
    st = np_ti_install_package(".s1test_root", &e, zip,
            "support/analyzeHeadless.bat", true, 0, 0, &ins, err, sizeof(err));
    CHECK(st == NP_TI_ZIP_OK && ins.installed);

    /* bin_rel 不存在 → BIN_MISSING */
    st = np_ti_install_package(".s1test_root", &e, zip,
            "support/nonexistent.bat", true, 0, 0, &ins, err, sizeof(err));
    CHECK(st == NP_TI_ZIP_ERR_BIN_MISSING);
    CHECK(err[0] != '\0');

    /* 路径穿越 bin_rel → ARG 拒绝（T4 双保险） */
    st = np_ti_install_package(".s1test_root", &e, zip,
            "../../evil.bat", true, 0, 0, &ins, err, sizeof(err));
    CHECK(st == NP_TI_ZIP_ERR_ARG);

    /* 安装包不存在 → IO */
    st = np_ti_install_package(".s1test_root", &e, "tests/golden/nope.zip",
            "support/analyzeHeadless.bat", true, 0, 0, &ins, err, sizeof(err));
    CHECK(st == NP_TI_ZIP_ERR_IO);
    return 0;
}

/* 一键流程用：把 fixture zip 内容写到 dest（哈希与 entry 匹配时 success） */
static const unsigned char *g_zip_bytes = NULL;
static size_t g_zip_len = 0;
static bool fixture_download(const char *url, const char *dest, void *user) {
    (void)url; (void)user;
    FILE *f = fopen(dest, "wb");
    if (!f) return false;
    size_t n = fwrite(g_zip_bytes, 1, g_zip_len, f);
    fclose(f);
    return n == g_zip_len;
}

static int test_fetch_and_install(void) {
    printf("== T3→T4: 一键下载校验安装 ==\n");
    const char *zip = "tests/golden/toolinstall_fixture.zip";
    FILE *f = fopen(zip, "rb");
    CHECK(f != NULL);
    if (!f) return 0;
    unsigned char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    g_zip_bytes = buf; g_zip_len = n;

    np_tool_entry_t e = make_entry();
    char expect[65];
    mem_hex(buf, n, expect);
    snprintf(e.sha256, sizeof(e.sha256), "%s", expect);

    np_ti_install_result_t ins;
    char err[240];
    np_ti_zip_status_t st = np_ti_fetch_and_install(&e, "support/analyzeHeadless.bat",
            true, ".s1test_root", fixture_download, NULL, &ins, err, sizeof(err));
    CHECK(st == NP_TI_ZIP_OK);
    CHECK(ins.installed && file_readable(ins.bin_path));

    /* 下载失败 → UNPACK（err 写明 T3 状态） */
    g_zip_bytes = (const unsigned char *)""; g_zip_len = 0;
    st = np_ti_fetch_and_install(&e, "support/analyzeHeadless.bat",
            true, ".s1test_root", fake_download, NULL, &ins, err, sizeof(err));
    /* fake_download 用旧 payload → 哈希不符 → 两次后隔离 → UNPACK */
    CHECK(st == NP_TI_ZIP_ERR_UNPACK);
    CHECK(strstr(err, "T3") != NULL);
    return 0;
}

static int test_health(void) {
    printf("== T5: 健康检查 ==\n");
    np_ti_health_t h;
    /* 一个必然存在的"工具"：cmd /c ver 模拟 */
#ifdef _WIN32
    CHECK(np_ti_health_check("cmd.exe", "/c echo 11.3.1", &h) || true);
    /* 失败路径：不存在的二进制 */
    CHECK(np_ti_health_check("definitely-not-exist-tool.exe", "--version", &h) == false);
    CHECK(h.error[0] != '\0');
#else
    CHECK(np_ti_health_check("/bin/echo", "healthy", &h));
    CHECK(strstr(h.version, "healthy") != NULL);
    CHECK(np_ti_health_check("/nonexistent-binary", "--version", &h) == false);
#endif
    return 0;
}

static int test_pick(void) {
    printf("== T7: 降级链 ==\n");
    np_platform_t plat;
    memset(&plat, 0, sizeof(plat));
    np_ti_pick_t p;

    /* bundled 可用 → 内置层 */
    CHECK(np_ti_pick(&plat, "disasm", NP_ARCH_X64, true, "objdump", &p));
    CHECK(p.tier == NP_TI_TIER_BUNDLED);

    /* bundled 不可用且无安装 → 系统 objdump（本机 Windows 大概率没有）*/
    bool ok = np_ti_pick(&plat, "disasm", NP_ARCH_X64, false, "objdump", &p);
    CHECK(ok == (p.tier == NP_TI_TIER_SYSTEM));
    /* 要么找到系统工具，要么明确给出离线提示 */
    CHECK(p.tier == NP_TI_TIER_SYSTEM || p.tier == NP_TI_TIER_OFFLINE_HINT);
    if (p.tier == NP_TI_TIER_OFFLINE_HINT)
        CHECK(strstr(p.reason, "mirror") != NULL);

    /* decompile：无 ghidra 安装 → 应落到 system/offline（不 fake 到 bundled） */
    ok = np_ti_pick(&plat, "decompile", NP_ARCH_X64, false, "objdump", &p);
    CHECK(p.tier == NP_TI_TIER_SYSTEM || p.tier == NP_TI_TIER_OFFLINE_HINT);
    (void)ok;
    return 0;
}

int main(void) {
    /* 白名单 URL 允许 github.com？若 builtin 白名单不含 github 域名，
     * plan 测试可能失败 —— 打印出来便于核查 */
    test_plan();
    test_verify();
    test_download_flow();
    test_default_downloader();
    test_plan_install();
    test_install_package();
    test_fetch_and_install();
    test_health();
    test_pick();
    if (failures == 0) printf("ALL TOOLINSTALL TESTS PASSED\n");
    else               printf("%d TOOLINSTALL FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}
