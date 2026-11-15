# Build RapidEditor for Windows x64 with llvm-mingw (no Visual Studio needed).
# llvm-mingw is a single toolchain that targets i686/x86_64/armv7/aarch64, so
# the same download also provides aarch64-w64-mingw32-clang for the ARM64
# payload. Add its bin/ directory to PATH first, then:
#   cmake -S . -B build-win-x64 -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=packaging/windows/llvm-mingw-x86_64.cmake \
#         -DCMAKE_BUILD_TYPE=Release
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER   x86_64-w64-mingw32-clang)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-clang++)
set(CMAKE_RC_COMPILER  x86_64-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
