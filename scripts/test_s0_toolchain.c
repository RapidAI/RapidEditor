/* Unit tests for T1+T2 (np_toolchain): platform probe + whitelist. */
#include "np_toolchain.h"
#include <stdio.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

int main(void) {
    /* T1 probe: deterministic compile-time fields, runtime scan for tools */
    np_platform_t p;
    np_toolchain_probe(&p, NULL);
    CHECK(p.pointer_bits == 32 || p.pointer_bits == 64);
    CHECK(p.os != NP_OS_UNKNOWN || p.arch == NP_ARCH_UNKNOWN); /* at least one known */
    CHECK(strlen(np_os_name(p.os)) > 0);
    CHECK(strlen(np_arch_name(p.arch)) > 0);

    np_toolchain_probe(&p, "C:/Program Files/Java/jdk-21/bin;D:/tools/objdump;ghidra_11");
    CHECK(p.has_jdk);
    CHECK(p.has_objdump);
    CHECK(p.has_ghidra);
    CHECK(!p.has_capstone_v6);
    np_toolchain_probe(&p, "/opt/capstone-v6/bin:/usr/bin/objdump");
    CHECK(p.has_capstone_v6);
    CHECK(p.has_objdump);

    /* T2 whitelist */
    np_whitelist_t *wl = np_whitelist_create();
    CHECK(wl && np_whitelist_count(wl) == 0);
    CHECK(np_whitelist_load_builtin(wl) == 4);
    CHECK(np_whitelist_count(wl) == 4);

    const np_tool_entry_t *gh = np_whitelist_find(wl, "ghidra");
    CHECK(gh && strcmp(gh->license, "Apache-2.0") == 0);
    CHECK(np_tool_supports(gh, NP_ARCH_X64, NP_OS_WINDOWS));
    CHECK(!np_tool_supports(gh, NP_ARCH_LOONGARCH64, NP_OS_WINDOWS));

    const np_tool_entry_t *cs = np_whitelist_find(wl, "capstone-v6");
    CHECK(cs && np_tool_supports(cs, NP_ARCH_LOONGARCH64, NP_OS_WINDOWS));

    /* URL allow-list: HTTPS + official domains only */
    CHECK(np_whitelist_url_allowed("https://github.com/NationalSecurityAgency/ghidra/releases"));
    CHECK(np_whitelist_url_allowed("https://api.adoptium.net/v3/binary/latest/21"));
    CHECK(!np_whitelist_url_allowed("http://github.com/foo"));           /* not HTTPS */
    CHECK(!np_whitelist_url_allowed("https://evil.example.com/tool"));    /* not whitelisted */
    CHECK(!np_whitelist_url_allowed("https://github.com.evil.com/x"));    /* subdomain trick */

    /* missing tools: platform has nothing installed -> decompile on x64 needs ghidra + jdk */
    np_platform_t bare; memset(&bare, 0, sizeof(bare));
    bare.os = NP_OS_WINDOWS; bare.arch = NP_ARCH_X64;
    char ids[8][48];
    size_t n = np_whitelist_missing(wl, &bare, "decompile", NP_ARCH_X64, ids, 8);
    CHECK(n == 1 && strcmp(ids[0], "ghidra") == 0);

    /* if gh + jdk present, nothing missing for decompile */
    bare.has_ghidra = true; bare.has_jdk = true;
    n = np_whitelist_missing(wl, &bare, "decompile", NP_ARCH_X64, ids, 8);
    CHECK(n == 0);

    /* disasm on LoongArch: capstone-v6 + llvm-objdump missing on bare host */
    memset(&bare, 0, sizeof(bare));
    bare.os = NP_OS_LINUX; bare.arch = NP_ARCH_LOONGARCH64;
    n = np_whitelist_missing(wl, &bare, "disasm", NP_ARCH_LOONGARCH64, ids, 8);
    CHECK(n == 2); /* capstone-v6 + llvm-objdump */

    np_whitelist_destroy(wl);
    printf("test_s0_toolchain: ALL PASSED\n");
    return 0;
}
