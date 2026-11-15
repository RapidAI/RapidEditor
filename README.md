# RapidEditor

一个跨平台、单文件 C99 编写的多标签记事本 / 源码编辑器，基于 [CUI](https://github.com/yappologistic/CUI) 即时模式 UI 库。原生窗口后端：Windows (Win32)、macOS (Cocoa)、Linux (X11)。

## 功能特性

- **多文档编辑**：新建 / 切换 / 关闭 / 拖拽排序标签页，`Ctrl+T` / `Ctrl+W` / `Ctrl+Tab` / `Ctrl+1..9`。
- **语法高亮**：按扩展名 / 文件名 / shebang 自动识别 C、C++、JS/TS、Python、Rust、Go、JSON、Shell、SQL 等常见语言，可手动覆盖。
- **EditPlus 风格界面**：可拖动的工作区侧栏（目录 / 文档 / 最近文件）、文档标签、可拖动的输出面板（查找结果 / 消息日志）、信息丰富的状态栏。
- **大文件支持**：动态缓冲区 + 行索引缓存 + 增量撤销，可完整打开、编辑、保存 100 MB 级 / 百万行文本。
- **中文友好**：系统 CJK 字体 fallback、按真实字形 advance 计算的光标 / 选区 / 点击定位，UTF-8 / UTF-8 BOM / UTF-16 / ANSI(GBK) 读写，中文路径命令行与拖放。
- **常用编辑能力**：查找 / 替换 / 转到行、行号、撤销重做、复制粘贴、Tab 缩进、括号高亮、插入日期时间、明暗主题、字号调节、CRLF/LF 切换、未保存关闭保护。

## 从源码构建

需要 CMake ≥ 3.16 与 C99 编译器（Windows: MSVC 或 MinGW-w64；macOS: Clang/Xcode CLT；Linux: GCC + X11/OpenGL 开发库）。

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
# 产物: build/RapidEditor(.exe)
```

Linux 需先安装依赖：

```sh
sudo apt-get install -y cmake ninja-build libx11-dev libgl1-mesa-dev libxcursor-dev libxinerama-dev libxi-dev
```

macOS 打通用二进制：

```sh
cmake -S . -B build -G Ninja -DCMAKE_OSX_ARCHITECTURES="x86_64;arm64"
```

## 安装包与右键菜单

发布版由 GitHub Actions 自动构建。手动打包脚本位于 `packaging/`：

| 平台 | 格式 | 脚本 | 架构策略 | 右键菜单 |
| --- | --- | --- | --- | --- |
| Windows | NSIS `.exe` | `packaging/windows/build_installer.bat` | `windows-universal` 一个包内含 x64 + ARM64 两份二进制；安装时用 `IsNativeARM64` / `RunningX64` 自动选 | 注册表 `HKCR\*\shell\RapidEditor`：「用 RapidEditor 打开」 |
| macOS | `.pkg` | `packaging/macos/build_pkg.sh` | 一份 `x86_64;arm64` 通用二进制，Intel / Apple Silicon 通用 | Finder 右键「打开方式 → RapidEditor」+「快速操作 / 用 RapidEditor 打开」 |
| Linux | `.AppImage` | `packaging/linux/build_appimage.sh` | 分别产出 `x86_64` / `aarch64` 两个包，`install-linux.sh` 按 `uname -m` 自动选 | `.desktop` MimeType 关联「打开方式」+ `file-manager/actions` 右键动作 |

三个平台都**不会修改用户默认的打开程序**；右键菜单只在选中文件时传入路径，主程序通过命令行参数（Windows/Linux）或 Apple Event 队列（macOS）打开新标签页。中文路径同样可用。

各目录的 README 有完整说明：[Windows](packaging/windows/README.md) · [macOS](packaging/macos/README.md) · [Linux](packaging/linux/README.md)。

### 下载后怎么装

- **Windows**：运行 `...-windows-universal-setup.exe`（推荐），或按机器选 `x64` / `arm64` 包。
- **macOS**：打开 `...-macos-universal.pkg`。未签名，首次请右键 →「打开」（详见 macOS README）。
- **Linux**：下载 `install-linux.sh` 后 `chmod +x && ./install-linux.sh`；脚本会识别架构并自动取对应 AppImage。也可以直接运行对应架构的 AppImage。

## 自动发布

`.github/workflows/release.yml`：

| 触发 | 行为 |
| --- | --- |
| 推送 `v*` 标签 | 三平台并行编译 → 上传 artifact → 创建 GitHub Release 附上全部安装包 |
| push / PR 到 main | 只编译并上传 artifact，不发 Release |
| 手动触发 | 可指定版本号、勾选是否发布 |

```sh
git tag v1.0.0 && git push origin v1.0.0
```

Job 说明：

- `windows`：`windows-latest` 上用 llvm-mingw 一次拿到 x64 与 ARM64 两份二进制（不依赖 Visual Studio），产出 3 个 NSIS 安装包；ARM64 编译失败时降级为仅 x64 包，不阻断发布。
- `macos`：`macos-15` 上一次编译 universal 二进制，`pkgbuild` 制作 `.pkg`，内含 Quick Action。
- `linux`：`ubuntu-24.04`（x86_64）与 `ubuntu-24.04-arm`（aarch64）各产一个 AppImage。
- `release`：合并 artifact，附加 `install-linux.sh`，创建 Release。

## 目录结构

```
src/rapideditor.c         主程序（单文件）
vendor/CUI/                CUI UI 库（含 CJK 光标 / 大文件 / macOS 打开方式补丁）
assets/                     PNG / ICO / ICNS 图标（packaging/make_icons.py 生成）
scripts/arch.sh             POSIX 架构检测（x86_64 / aarch64 ↔ x64 / arm64）
packaging/windows/         NSIS 安装包、交叉工具链、右键菜单
packaging/macos/           pkg 打包、Info.plist、Quick Action、postinstall
packaging/linux/           AppImage、AppRun、.desktop、安装/卸载脚本
.github/workflows/         自动编译发布
docs/NOTES.md              vendored CUI 补丁说明
```

## 许可

本项目 MIT，见 [LICENSE](LICENSE)。vendored CUI 遵循其原始许可，见 `vendor/CUI/LICENSE` 与 `vendor/CUI/THIRD_PARTY.md`。
