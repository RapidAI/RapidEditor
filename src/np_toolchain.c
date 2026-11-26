/* np_toolchain.c - T1+T2 platform probe + whitelist (see header). */
#include "np_toolchain.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#  include <unistd.h>
#endif

/* ---------------- platform detection ---------------- */
#if defined(_WIN32) || defined(_WIN64)
  #define NP_OS_CURRENT NP_OS_WINDOWS
#elif defined(__ANDROID__)
  #define NP_OS_CURRENT NP_OS_ANDROID
#elif defined(__APPLE__)
  #define NP_OS_CURRENT NP_OS_MACOS
#elif defined(__linux__)
  #define NP_OS_CURRENT NP_OS_LINUX
#else
  #define NP_OS_CURRENT NP_OS_UNKNOWN
#endif

static np_arch_t detect_arch(void) {
#if defined(__LOONGARCH64__) || defined(__loongarch64)
    return NP_ARCH_LOONGARCH64;
#elif defined(__loongarch__) || defined(__LOONGARCH__)
    return NP_ARCH_LOONGARCH32;
#elif defined(__aarch64__) || defined(_M_ARM64)
    return NP_ARCH_ARM64;
#elif defined(__arm__) || defined(_M_ARM)
    return NP_ARCH_ARM;
#elif defined(__mips__) || defined(__mips)
    return NP_ARCH_MIPS;
#elif defined(__riscv)
    return NP_ARCH_RISCV;
#elif defined(__x86_64__) || defined(_M_X64) || defined(_M_AMD64)
    return NP_ARCH_X64;
#elif defined(__i386__) || defined(_M_IX86)
    return NP_ARCH_X86;
#else
    return NP_ARCH_UNKNOWN;
#endif
}

const char *np_os_name(np_os_t os) {
    switch (os) {
        case NP_OS_WINDOWS: return "Windows";
        case NP_OS_LINUX:   return "Linux";
        case NP_OS_MACOS:   return "macOS";
        case NP_OS_ANDROID: return "Android";
        default:            return "Unknown";
    }
}

const char *np_arch_name(np_arch_t a) {
    switch (a) {
        case NP_ARCH_X86:         return "x86";
        case NP_ARCH_X64:         return "x86-64";
        case NP_ARCH_ARM:         return "arm";
        case NP_ARCH_ARM64:       return "arm64";
        case NP_ARCH_LOONGARCH32: return "LoongArch32";
        case NP_ARCH_LOONGARCH64: return "LoongArch64";
        case NP_ARCH_MIPS:        return "MIPS";
        case NP_ARCH_RISCV:       return "RISC-V";
        default:                  return "unknown";
    }
}

static bool path_has(const char *scan_path, const char *needle, char *found, size_t cap) {
    if (!scan_path || !needle) return false;
    /* scan_path is ';'/':'-separated; check each token as a substring match
     * against the needle (e.g. "jdk", "objdump", "ghidra"). */
    const char *p = scan_path;
    char buf[1024];
    while (*p) {
        const char *s = p;
        while (*p && *p != ';' && *p != ':') p++;
        size_t n = (size_t)(p - s);
        if (n < sizeof(buf)) {
            memcpy(buf, s, n); buf[n] = '\0';
            if (strstr(buf, needle)) {
                if (found) snprintf(found, cap, "%s", buf);
                return true;
            }
        }
        if (*p) p++;
    }
    return false;
}

void np_toolchain_probe(np_platform_t *out, const char *scan_path) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->os = NP_OS_CURRENT;
    out->arch = detect_arch();
    out->pointer_bits = (int)(sizeof(void *) * 8);

    char buf[128];
    if (path_has(scan_path, "jdk", buf, sizeof(buf)) ||
        path_has(scan_path, "java", buf, sizeof(buf))) {
        out->has_jdk = true;
        snprintf(out->jdk_version, sizeof(out->jdk_version), "detected");
    }
    out->has_objdump = path_has(scan_path, "objdump", buf, sizeof(buf));
    out->has_ghidra  = path_has(scan_path, "ghidra", buf, sizeof(buf));
    out->has_capstone_v6 = path_has(scan_path, "capstone-v6", buf, sizeof(buf));

    /* RAM: best-effort, portable */
#if defined(_WIN32)
    /* Kept 0 to stay dependency-free; the app may fill this via OS APIs. */
    out->ram_mb = 0;
#else
    long pages = sysconf(_SC_PHYS_PAGES);
    long psz = sysconf(_SC_PAGESIZE);
    if (pages > 0 && psz > 0) out->ram_mb = (uint64_t)pages * (uint64_t)psz / (1024*1024);
#endif

    if (out->arch == NP_ARCH_LOONGARCH64 || out->arch == NP_ARCH_LOONGARCH32)
        snprintf(out->notes, sizeof(out->notes),
                 "LoongArch: Capstone v6 / cs_support() required; v5 has no CS_ARCH_LOONGARCH, fall back to objdump.");
    else if (out->arch == NP_ARCH_UNKNOWN)
        snprintf(out->notes, sizeof(out->notes), "Unknown arch: require user selection before disassembly.");
}

/* ---------------- T2 whitelist ---------------- */
struct np_whitelist {
    np_tool_entry_t entries[64];
    size_t count;
};

np_whitelist_t *np_whitelist_create(void) {
    return (np_whitelist_t *)calloc(1, sizeof(np_whitelist_t));
}
void np_whitelist_destroy(np_whitelist_t *wl) { free(wl); }

static bool add(np_whitelist_t *wl, const np_tool_entry_t *e) {
    if (!wl || !e || wl->count >= 64) return false;
    wl->entries[wl->count++] = *e;
    return true;
}

size_t np_whitelist_load_builtin(np_whitelist_t *wl) {
    if (!wl) return 0;
    size_t before = wl->count;
    np_tool_entry_t e;

    /* Ghidra - official GitHub releases; requires 64-bit JDK (verified in
     * Ghidra's official README). Hashes are filled at download time by T3
     * from the release's published checksums; kept empty here by design. */
    memset(&e, 0, sizeof(e));
    snprintf(e.id, sizeof(e.id), "ghidra");
    snprintf(e.purpose, sizeof(e.purpose), "decompile");
    np_arch_t gh_archs[] = {NP_ARCH_X64, NP_ARCH_ARM64};
    memcpy(e.archs, gh_archs, sizeof(gh_archs)); e.narchs = 2;
    snprintf(e.version, sizeof(e.version), "11.x");
    snprintf(e.source_url, sizeof(e.source_url), "https://github.com/NationalSecurityAgency/ghidra/releases");
    snprintf(e.license, sizeof(e.license), "Apache-2.0");
    np_os_t gh_plat[] = {NP_OS_WINDOWS, NP_OS_LINUX, NP_OS_MACOS};
    memcpy(e.platforms, gh_plat, sizeof(gh_plat)); e.nplatforms = 3;
    e.approx_size_kb = 400 * 1024;
    add(wl, &e);

    /* JDK 64-bit - required by Ghidra */
    memset(&e, 0, sizeof(e));
    snprintf(e.id, sizeof(e.id), "jdk64");
    snprintf(e.purpose, sizeof(e.purpose), "runtime");
    np_arch_t j_archs[] = {NP_ARCH_X64, NP_ARCH_ARM64};
    memcpy(e.archs, j_archs, sizeof(j_archs)); e.narchs = 2;
    snprintf(e.version, sizeof(e.version), "21 LTS");
    snprintf(e.source_url, sizeof(e.source_url), "https://api.adoptium.net/v3/binary/latest/21/ga");
    snprintf(e.license, sizeof(e.license), "GPL-2.0-with-classpath-exception");
    e.nplatforms = 0; /* all */
    e.approx_size_kb = 200 * 1024;
    add(wl, &e);

    /* Capstone v6 - embedded baseline, LoongArch needs CS_API_MAJOR >= 6 */
    memset(&e, 0, sizeof(e));
    snprintf(e.id, sizeof(e.id), "capstone-v6");
    snprintf(e.purpose, sizeof(e.purpose), "disasm");
    np_arch_t c_archs[] = {NP_ARCH_X64, NP_ARCH_X86, NP_ARCH_ARM, NP_ARCH_ARM64,
                           NP_ARCH_LOONGARCH64, NP_ARCH_LOONGARCH32, NP_ARCH_MIPS, NP_ARCH_RISCV};
    memcpy(e.archs, c_archs, sizeof(c_archs)); e.narchs = 8;
    snprintf(e.version, sizeof(e.version), "6.0");
    snprintf(e.source_url, sizeof(e.source_url), "https://github.com/capstone-engine/capstone/releases");
    snprintf(e.license, sizeof(e.license), "BSD-2-Clause");
    e.nplatforms = 0;
    e.approx_size_kb = 8192;
    add(wl, &e);

    /* llvm-objdump - cross-arch fallback for LoongArch / others */
    memset(&e, 0, sizeof(e));
    snprintf(e.id, sizeof(e.id), "llvm-objdump");
    snprintf(e.purpose, sizeof(e.purpose), "disasm");
    np_arch_t o_archs[] = {NP_ARCH_X64, NP_ARCH_ARM64, NP_ARCH_LOONGARCH64,
                           NP_ARCH_LOONGARCH32, NP_ARCH_MIPS, NP_ARCH_RISCV};
    memcpy(e.archs, o_archs, sizeof(o_archs)); e.narchs = 6;
    snprintf(e.version, sizeof(e.version), "18.x");
    snprintf(e.source_url, sizeof(e.source_url), "https://releases.llvm.org/download.html");
    snprintf(e.license, sizeof(e.license), "Apache-2.0-with-LLVM-exceptions");
    np_os_t o_plat[] = {NP_OS_WINDOWS, NP_OS_LINUX, NP_OS_MACOS};
    memcpy(e.platforms, o_plat, sizeof(o_plat)); e.nplatforms = 3;
    e.approx_size_kb = 60 * 1024;
    add(wl, &e);

    return wl->count - before;
}

bool np_whitelist_add(np_whitelist_t *wl, const np_tool_entry_t *e) { return add(wl, e); }

const np_tool_entry_t *np_whitelist_find(const np_whitelist_t *wl, const char *id) {
    if (!wl || !id) return NULL;
    for (size_t i = 0; i < wl->count; ++i)
        if (strcmp(wl->entries[i].id, id) == 0) return &wl->entries[i];
    return NULL;
}

size_t np_whitelist_count(const np_whitelist_t *wl) { return wl ? wl->count : 0; }

bool np_whitelist_url_allowed(const char *url) {
    if (!url) return false;
    if (strncmp(url, "https://", 8) != 0) return false; /* HTTPS only */
    /* whitelisted official domains */
    static const char *domains[] = {
        "github.com", "api.adoptium.net", "releases.llvm.org",
        "download.oracle.com", "sourceforge.net", "npmjs.com"
    };
    const char *host = url + 8;
    const char *slash = strchr(host, '/');
    size_t hostlen = slash ? (size_t)(slash - host) : strlen(host);
    for (size_t i = 0; i < sizeof(domains)/sizeof(domains[0]); ++i) {
        size_t dl = strlen(domains[i]);
        /* allow exact host or subdomain ending in "."+domain */
        if (dl <= hostlen) {
            const char *tail = host + hostlen - dl;
            if (strncmp(tail, domains[i], dl) == 0 &&
                (dl == hostlen || tail[-1] == '.')) return true;
        }
    }
    return false;
}

bool np_tool_supports(const np_tool_entry_t *e, np_arch_t arch, np_os_t os) {
    if (!e) return false;
    bool arch_ok = (e->narchs == 0);
    for (size_t i = 0; i < e->narchs; ++i)
        if (e->archs[i] == arch) { arch_ok = true; break; }
    bool os_ok = (e->nplatforms == 0);
    for (size_t i = 0; i < e->nplatforms; ++i)
        if (e->platforms[i] == os) { os_ok = true; break; }
    return arch_ok && os_ok;
}

size_t np_whitelist_missing(const np_whitelist_t *wl, const np_platform_t *plat,
                            const char *purpose, np_arch_t need_arch,
                            char out_ids[][48], size_t out_cap) {
    if (!wl || !plat || !purpose || !out_ids || out_cap == 0) return 0;
    size_t n = 0;
    for (size_t i = 0; i < wl->count && n < out_cap; ++i) {
        const np_tool_entry_t *e = &wl->entries[i];
        if (strcmp(e->purpose, purpose) != 0) continue;
        if (!np_tool_supports(e, need_arch, plat->os)) continue;
        /* is it already installed? */
        bool present = false;
        if (strcmp(e->id, "ghidra") == 0) present = plat->has_ghidra;
        else if (strcmp(e->id, "jdk64") == 0) present = plat->has_jdk;
        else if (strcmp(e->id, "capstone-v6") == 0) present = plat->has_capstone_v6;
        else if (strcmp(e->id, "llvm-objdump") == 0) present = plat->has_objdump;
        if (!present) snprintf(out_ids[n++], 48, "%s", e->id);
    }
    return n;
}
