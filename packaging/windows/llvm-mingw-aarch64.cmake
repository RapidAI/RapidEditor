# Cross-compile RapidEditor for Windows ARM64 from an x64 Windows or Linux host.
# Requires llvm-mingw (https://github.com/mstorsjo/llvm-mingw) on PATH:
#   cmake -S . -B build-win-arm64 -G Ninja \
#         -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_TOOLCHAIN_FILE=packaging/windows/llvm-mingw-aarch64.cmake
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR ARM64)
set(CMAKE_C_COMPILER   aarch64-w64-mingw32-clang)
set(CMAKE_CXX_COMPILER aarch64-w64-mingw32-clang++)
set(CMAKE_RC_COMPILER  aarch64-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
