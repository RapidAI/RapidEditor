#!/usr/bin/env bash
# Shared architecture detection for every POSIX build/packaging script.
#
# Source it:  . scripts/arch.sh ; re_arch ; re_short ; re_macos_arch
# Run it:     bash scripts/arch.sh [short|long|macos|appimage]
#
# Override for cross builds:  RE_ARCH=x86_64  or  RE_ARCH=aarch64
#
#   long / re_arch       -> x86_64 | aarch64
#   short / re_short     -> x64 | arm64
#   macos / re_macos_arch-> x86_64 | arm64   (CMAKE_OSX_ARCHITECTURES values)
#   appimage             -> x86_64 | aarch64 (appimagetool asset suffix)

re_arch() {
  if [ -n "${RE_ARCH:-}" ]; then
    case "$RE_ARCH" in
      x86_64|amd64|x64) printf 'x86_64' ;;
      aarch64|arm64)     printf 'aarch64' ;;
      *) printf 'unsupported RE_ARCH: %s\n' "$RE_ARCH" >&2; return 1 ;;
    esac
    return 0
  fi
  case "$(uname -m | tr '[:upper:]' '[:lower:]')" in
    x86_64|amd64)   printf 'x86_64' ;;
    aarch64|arm64)  printf 'aarch64' ;;
    *) printf 'unsupported architecture: %s\n' "$(uname -m)" >&2; return 1 ;;
  esac
}

re_short() {
  case "$(re_arch)" in
    x86_64) printf 'x64' ;;
    aarch64) printf 'arm64' ;;
    *) return 1 ;;
  esac
}

re_macos_arch() {
  case "$(re_arch)" in
    x86_64) printf 'x86_64' ;;
    aarch64) printf 'arm64' ;;
    *) return 1 ;;
  esac
}

re_appimage_arch() { re_arch; }

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  set -euo pipefail
  case "${1:-short}" in
    short)    re_short ;;
    long)     re_arch ;;
    macos)    re_macos_arch ;;
    appimage) re_appimage_arch ;;
    *) echo "usage: $0 [short|long|macos|appimage]" >&2; exit 2 ;;
  esac
  printf '\n'
fi
