#!/usr/bin/env python3
"""Copy an ELF binary's non-glibc runtime dependencies into an AppDir.

    python3 bundle_libs.py <binary> <appdir>/usr/lib

RapidEditor only needs libX11 (plus its libxcb/libXau/libXdmcp chain); OpenGL
is dlopen()ed at runtime by CUI and must stay on the host. glibc, the dynamic
loader and graphics driver libraries are deliberately NOT bundled -- shipping
those breaks the AppImage on other distributions.
"""
from __future__ import annotations

import re
import shutil
import subprocess
import sys
from pathlib import Path

EXCLUDE = re.compile(
    r"^(libc\.so|libm\.so|libdl\.so|libpthread|ld-linux|ld64|libanl|librt|"
    r"libutil|libresolv|libnss_|libnsl|libgcc_s|libstdc\+\+|"
    r"libGL\.so|libEGL\.so|libGLX|libGLdispatch|libgbm|libdrm|libglapi|"
    r"libvulkan|libnvidia|libLLVM|libz\.so|libbsd\.so|libmd\.so|"
    r"linux-vdso|linux-gate)",
    re.IGNORECASE,
)


def deps(path: Path) -> list[Path]:
    try:
        out = subprocess.run(["ldd", str(path)], capture_output=True, text=True, check=False).stdout
    except FileNotFoundError:
        print("WARNING: ldd not found, skipping library bundling", file=sys.stderr)
        return []
    result = []
    for line in out.splitlines():
        line = line.strip()
        if "=>" not in line or "not found" in line:
            continue
        candidate = line.split("=>", 1)[1].strip().split()[0]
        p = Path(candidate)
        if p.is_file() and p.is_absolute():
            result.append(p)
    return result


def main() -> None:
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    binary, libdir = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    libdir.mkdir(parents=True, exist_ok=True)

    queue = deps(binary)
    seen: set[str] = set()
    copied: list[str] = []
    while queue:
        src = queue.pop()
        key = str(src.resolve())
        if key in seen or EXCLUDE.match(src.name):
            continue
        seen.add(key)
        dst = libdir / src.name
        if not dst.exists():
            shutil.copy2(src, dst)
            copied.append(src.name)
        queue.extend(deps(dst))
    print("Bundled %d libraries: %s" % (len(copied), ", ".join(sorted(copied))))


if __name__ == "__main__":
    main()
