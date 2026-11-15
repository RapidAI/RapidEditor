# 实现说明

## vendored CUI

`vendor/CUI/` 是 [yappologistic/CUI](https://github.com/yappologistic/CUI) 的一个子集（只保留构建
RapidEditor 所需的 `CMakeLists.txt`、`cmake/`、`include/cui/`、`src/`、`third_party/`、`LICENSE`、
`THIRD_PARTY.md`）。相对上游有 3 处本地修改：

1. **CJK 光标 / 选区 advance**（`include/cui/cui.h`、`src/cui_editor.c`）：按真实字形宽度计算
   光标、选区与点击定位，中文不再错位。
2. **大文件编辑**（同上）：动态缓冲区 + 行索引缓存 + 增量撤销，可打开编辑 100 MB 级文本。
3. **macOS「打开方式」**（本次新增）：
   - `include/cui/cui_app.h` 增加 `cui_app_post_open_file()` /
     `cui_app_pending_file_count()` / `cui_app_pending_file()` / `cui_app_clear_pending_files()`；
   - `src/platform/cui_app.c` 实现全局待打开文件队列（主线程、无需加锁，可在
     `cui_app_create()` 之前写入）；
   - `src/platform/cui_app_cocoa.m` 的 `CUIPlat` 实现
     `application:openFile:` / `application:openFiles:`，并把 delegate 的设置提前到
     `finishLaunching` **之前**——否则启动期间收到的 Apple Event 会被 AppKit 丢弃；
   - `src/rapideditor.c` 每帧排空队列并调用 `np_open_path()`。

第 3 点是右键菜单在 macOS 上真正能打开文件的必要条件：Finder 的「打开方式」、
Quick Action 和拖到 Dock 图标都是 Apple Event，不是命令行参数。上游 CUI 只处理了窗口内拖放
（`cui_input_file_drop()`），无法接收这些事件。

三处改动相对上游 CUI 的完整 diff 保存在 `docs/patches/`：
`CUI-cjk-caret.patch`、`CUI-cjk-largefile.patch`、`CUI-openwith.patch`。
上游升级时可以按这些 patch 重新套用。

> 头文件必须位于 `vendor/CUI/include/cui/*.h`。上游的包含路径是 `cui/cui.h`，
> 早期 vendored 副本把它们平铺在 `include/` 下导致编译失败，已修正。

## 图标

`assets/rapideditor.png` / `.ico` / `.icns` 由 `packaging/make_icons.py`（Pillow）生成并提交，
这样 CI 三个平台都不需要安装 Python 图像库。修改图标后重新运行：

```sh
python packaging/make_icons.py
```

## 架构判定

- `scripts/arch.sh` 是 POSIX 侧的唯一入口：`uname -m` → `x86_64` / `aarch64`，并映射到
  短名 `x64` / `arm64` 与 macOS 的 `x86_64` / `arm64`。可用 `RE_ARCH` 覆盖（交叉编译时用）。
- Windows 侧由 NSIS 的 `x64.nsh`（`IsNativeARM64` / `RunningX64`）在安装时判定；
  `build_installer.bat` 用 `PROCESSOR_ARCHITECTURE` 判定本机编译目标。
- macOS 用 `CMAKE_OSX_ARCHITECTURES` 直接产通用二进制，无需运行时判断。
- Linux 的 AppImage 运行时是单架构 ELF，`install.sh` 用 `uname -m` 选择资产。

## 为什么 Windows 用 llvm-mingw 而不是 Visual Studio

`windows-latest` 的镜像不保证装了某个具体版本的 VS（实测 `Visual Studio 17 2022`
生成器报 "could not find any instance of Visual Studio"）。工作流改用
[llvm-mingw](https://github.com/mstorsjo/llvm-mingw)：一份工具链同时提供
`x86_64-w64-mingw32-clang.exe` 和 `aarch64-w64-mingw32-clang.exe`，
配合 `packaging/windows/llvm-mingw-x86_64.cmake` 与
`llvm-mingw-aarch64.cmake`，在 x64 runner 上用 Ninja 一次编出两个架构。

好处是同一个 job 内可以同时拿到两份二进制，从而产出一个**通用安装包**：
`installer.nsi` 在 `.onInit` 检测本机架构，把匹配的 payload 改名为 `RapidEditor.exe`。
ARM64 步骤带 `continue-on-error`，若交叉编译出问题仍会产出 x64 安装包。
本地实测两个 PE 的 machine 分别为 `0x8664`（x64）与 `0xaa64`（ARM64）。

## NSIS 编码

`packaging/windows/installer.nsi` 必须是 **带 BOM 的 UTF-8**。NSIS 3 默认按系统 ANSI
代码页读取 `.nsi`；没有 BOM 时中文菜单文字会报 `Bad text encoding`，在英文 runner 上同样如此。
`.bat` 文件则刻意保持纯 ASCII，避免 cmd 代码页问题。
