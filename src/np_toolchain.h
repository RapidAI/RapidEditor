#ifndef NP_TOOLCHAIN_H
#define NP_TOOLCHAIN_H

/*
 * np_toolchain.h - T1 + T2: platform probe + tool whitelist registry.
 *
 * T1: probe host OS / CPU arch / pointer width / RAM / installed tools
 *     (JDK, objdump, Ghidra, Capstone v6...), producing a capability matrix.
 *     READ-ONLY: never changes the system.
 * T2: whitelist of external analyzers - purpose, supported platforms,
 *     version, official source URL, expected SHA-256, license, size. Only
 *     whitelisted domains may be downloaded from (enforced by T3 later).
 *
 * Pure C99. Probe uses compile-time constants + optional runtime checks so
 * it is deterministic in unit tests and truthful on the real host.
 */
#ifdef __cplusplus
extern "C" {
#endif

#include "np_contract.h"
#include <stddef.h>
#include <stdbool.h>

typedef enum {
    NP_OS_WINDOWS = 0,
    NP_OS_LINUX,
    NP_OS_MACOS,
    NP_OS_ANDROID,
    NP_OS_UNKNOWN
} np_os_t;

typedef enum {
    NP_ARCH_X86 = 0,     /* 32-bit x86        */
    NP_ARCH_X64,         /* x86-64            */
    NP_ARCH_ARM,         /* 32-bit ARM        */
    NP_ARCH_ARM64,       /* AArch64           */
    NP_ARCH_LOONGARCH32,
    NP_ARCH_LOONGARCH64,
    NP_ARCH_MIPS,
    NP_ARCH_RISCV,
    NP_ARCH_UNKNOWN
} np_arch_t;

const char *np_os_name(np_os_t os);
const char *np_arch_name(np_arch_t a);

typedef struct {
    np_os_t    os;
    np_arch_t  arch;
    int        pointer_bits;      /* 32 / 64 */
    uint64_t   ram_mb;            /* 0 if unknown */
    bool       has_jdk;
    bool       has_objdump;
    bool       has_ghidra;
    bool       has_capstone_v6;   /* LoongArch needs v6 */
    char       jdk_version[32];
    char       notes[256];
} np_platform_t;

/* Fill `out` with a probe of the current host. `scan_path` may be a PATH-like
 * string to search for installed tools; NULL means compile-time defaults only. */
void np_toolchain_probe(np_platform_t *out, const char *scan_path);

/* --- T2 whitelist ---------------------------------------------------- */
typedef struct {
    char        id[48];           /* "ghidra", "jdk", "capstone-v6"       */
    char        purpose[80];      /* "decompile", "runtime", "disasm"     */
    np_arch_t   archs[8];         /* supported archs, NP_ARCH_UNKNOWN end */
    size_t      narchs;
    char        version[32];
    char        source_url[256];  /* official https URL                  */
    char        sha256[65];       /* expected package hash (may be empty) */
    char        license[48];
    uint64_t    approx_size_kb;
    np_os_t     platforms[4];
    size_t      nplatforms;
} np_tool_entry_t;

typedef struct np_whitelist np_whitelist_t;

np_whitelist_t *np_whitelist_create(void);
void np_whitelist_destroy(np_whitelist_t *wl);

/* Built-in whitelist from verified official sources (Ghidra/JDK/Capstone/
 * llvm-objdump). Returns the number of entries loaded. */
size_t np_whitelist_load_builtin(np_whitelist_t *wl);

bool np_whitelist_add(np_whitelist_t *wl, const np_tool_entry_t *e);
const np_tool_entry_t *np_whitelist_find(const np_whitelist_t *wl, const char *id);
const np_tool_entry_t *np_whitelist_at(const np_whitelist_t *wl, size_t i);
size_t np_whitelist_count(const np_whitelist_t *wl);

/* T2 hard rule: only HTTPS + whitelisted official domains. Returns true if
 * a URL is acceptable for download. */
bool np_whitelist_url_allowed(const char *url);

/* Which whitelisted tools are needed for a task on a given platform/arch,
 * and are currently NOT present on the host. Writes ids into out[]. */
size_t np_whitelist_missing(const np_whitelist_t *wl, const np_platform_t *plat,
                            const char *purpose, np_arch_t need_arch,
                            char out_ids[][48], size_t out_cap);

/* T2: does a whitelist entry support the target arch + OS? */
bool np_tool_supports(const np_tool_entry_t *e, np_arch_t arch, np_os_t os);

#ifdef __cplusplus
}
#endif
#endif /* NP_TOOLCHAIN_H */
