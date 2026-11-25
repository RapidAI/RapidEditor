# RapidEditor

一个跨平台的多标签记事本 / 源码编辑器，核心为 C99，基于 [CUI](https://github.com/yappologistic/CUI) 即时模式 UI 库。原生窗口后端：Windows (Win32)、macOS (Cocoa)、Linux (X11)、Android 平板 (SDL2/NativeActivity，实验性)。HTML 内嵌预览的原生桥接层按平台使用 C++ / Objective-C++（见下）。

## 功能特性

- **多文档编辑**：新建 / 切换 / 关闭 / 拖拽排序标签页，`Ctrl+T` / `Ctrl+W` / `Ctrl+Tab` / `Ctrl+1..9`。
- **语法高亮**：按扩展名 / 文件名 / shebang 自动识别 C、C++、JS/TS、Python、Rust、Go、JSON、Shell、SQL 等常见语言，可手动覆盖。
- **EditPlus 风格界面**：可拖动的工作区侧栏（目录 / 文档 / 最近文件）、文档标签、可拖动的输出面板（查找结果 / 消息日志）、信息丰富的状态栏。
- **现代专业视觉**：程序名与图标集成在系统标题栏 / 任务栏，工具栏提供密度切换与 F1 帮助；文档面包屑、查找结果徽标、TEXT/HEX 状态标记；紧凑 / 舒适 / 宽松三档界面密度、深浅主题与「跟随系统主题」均可持久化；空白会话显示带最近文件快捷入口的欢迎页。
- **统一预览面板**（`Ctrl+Shift+P` 折叠 / 展开）：左侧编辑、右侧预览，可拖动分隔条；按文件类型自动切换——
  - **Markdown**：右侧实时渲染（标题 / 列表 / 代码块 / 表格 / 任务项等），随编辑即时更新；支持行内 `$...$` 与块级 `$$...$$` 数学公式（常用 TeX 符号转 Unicode），以及 ```` ```mermaid ```` 流程图（`graph/flowchart TD|LR`，节点 + 箭头 + 连线标签）原生预览；
  - **LaTeX / PDF**：用 TinyTeX / TeX Live 的 `xelatex`（或 `pdflatex`）编译成 PDF，再用 poppler 逐页渲染为图片，在面板内翻页 / 查看；保存时自动重编译，也可点「重新编译」；直接打开 `.pdf` 文件亦可在面板内预览；
  - **HTML**：程序内建 loopback HTTP 服务器提供当前目录，**真正的浏览器直接内嵌在右侧预览面板**（与编辑器同窗口，随分隔条 / 折叠联动）。各平台使用系统自带内核、无需打包沉重运行时：Windows 用 WebView2（Edge 内核，仅带一个 ~190 KB 的加载器）、macOS 用 WKWebView、Linux 用 WebKitGTK。系统内核缺失时面板给出提示，并可一键在系统默认浏览器打开。
  - 外部工具（TeX 引擎 / poppler / 浏览器）均为可选依赖，缺失时面板给出明确提示，不影响编辑功能。
- **大文件支持**：动态缓冲区 + 行索引缓存 + 增量撤销，可完整打开、编辑、保存 100 MB 级 / 百万行文本。
- **中文友好**：系统 CJK 字体 fallback、按真实字形 advance 计算的光标 / 选区 / 点击定位，UTF-8 / UTF-8 BOM / UTF-16 / ANSI(GBK) 读写，中文路径命令行与拖放。
- **常用编辑能力**：查找（大小写敏感、全词匹配可选）/ 替换（逐个或全部）/ 转到行、行号、撤销重做、复制粘贴、Tab 缩进、括号高亮、插入日期时间、明暗主题、字号调节、CRLF/LF 切换、未保存关闭保护。
- **AI 助手**：右侧常驻面板与选区内联快捷按钮，支持润色、翻译、检查、解释、续写和模板生成；按文件类型自动选择提示词，流式显示输出并可停止 / 重试；改写结果支持逐处接受 / 拒绝后再写入，并保留每个文档的 AI 历史。AI 返回整段被代码围栏包裹时会自动剥离（仅对写入文档的结果，问答保留原样）。可自定义 AI 动作（常用指令一键调用）并在右键菜单使用；LaTeX 编译失败时可将编译日志交给 AI 分析，修复结果走同一套逐处确认流程；回答类结果可插入光标处或复制。**行内补全**（AI 面板可开关）：输入停顿后（防抖时长 1~3 秒可选），AI 在光标处给出灰色续写建议，Tab 一键接受（作为一步可撤销编辑），Esc 或继续输入即消失；`Alt+\`、工具条「补全」按钮或右键菜单可随时手动触发；建议支持多行（首行后显示 …+N 行标记，Tab 接受全部）；仅使用 OpenAI 兼容后端（独立轻量参数：短超时快速失败），短小建议、静默失败、不打扰输入。
- **可控上下文**：默认只发送当前选区；可勾选附带其他已打开标签页、当前查找 / 目录搜索结果，发送前显示本次包含的内容与大小估算，过长内容会标注截取位置。内嵌 AI 操作与编译错误修复不会带入这些额外来源。
- **隐私与安全**：服务商 API Key 输入框以圆点显示（可点眼睛临时查看）；Windows 下用 DPAPI 按当前用户加密后写入配置，旧版明文配置自动迁移，换机器需重新填写。工作区根目录的 `.aiignore` 可排除私密文件，使其不进入语义搜索、目录搜索与 AI 上下文，支持 `#` 注释、`dir/`、`*.ext` 和 `*`/`?` 通配符。
- **AI 语义搜索**：在「在文件中查找」对话框用自然语言描述想找的内容，AI 从工作区候选文件里筛选相关位置。结果沿用「文件搜索」面板，显示真实文件名、原文代码行和可跳转行号（原文从本地文件读回，不用 AI 编造的片段）；支持停止、失败提示，三种 AI 后端通用。**工作区问答**：切到问答模式后，AI 会基于同一批候选文件直接回答问题，回答下方列出带行号的引用、点击即跳转，防幻觉机制与搜索一致。
- **智能粘贴**：编辑器工具条 / 右键菜单一键识别剪贴板中的表格（Excel 单元格 / CSV / TSV，按文档生成 Markdown 或 LaTeX 三线表）、JSON（格式化缩进）与日志（提取错误 / 警告摘要）；转换前可预览，随时选择「原样粘贴」。
- **草稿恢复**：未保存文本定期写入配置目录，下次启动自动恢复；命名文件保留原路径并继续标记为未保存。
- **可靠性**：单次 AI 请求只替换「实际发送给 AI 的那段文本」，超过 64 KB 的内容不会被删掉尾部；「补全续写」始终插入到光标处；主动「停止」与超长输出截断会给出准确状态，不误报为失败；后台任务不再累积系统句柄。自定义服务商在获取模型 / 测试连接期间可安全编辑表单，旧请求结果不会写错到别的条目。**AI 改写与智能粘贴都可被 `Ctrl+Z` 撤销**（作为一次正常编辑步骤写入编辑器历史，不会污染撤销偏移）；逐处 Diff 的接受 / 拒绝按钮在新生成结果上正常可用；草稿快照与其原始路径 / 编码一起保存，异常退出后也能正确恢复而不会写错文件；AI 语义搜索运行期间清空结果不会引发两轮搜索并发写坏数据。
- **编辑器字体**（视图 → 编辑器字体）：内置 5 款 SIL OFL 开源编程字体（JetBrains Mono、Cascadia Code / Mono、Source Code Pro、IBM Plex Mono），也可加载本机任意 TTF/OTF 字体文件；切换即时生效、中文 CJK fallback 自动保留，选择随配置持久化。
- **十六进制编辑**（010 Editor 风格）：二进制文件自动以十六进制模式打开（含 NUL 或大量控制字节即判定），偏移列 + 16 字节行 + ASCII 三段视图；点击 / 拖选字节、方向键与翻页移动、直接键入十六进制覆盖光标字节、`Ctrl+C` 复制选区为十六进制 / `Ctrl+Shift+C` 复制为 ASCII、`Ctrl+G` 转到偏移；文本 ↔ 十六进制模式可随时互切（「文件」菜单或工具栏），保存原样写回字节。  
  - **字节序列查找**（`Ctrl F`）：按十六进制（`1F 2E CC`）或 ASCII 文本查找，命中自动滚动到视野并选中，`F3` 找下一个。
- **在文件中查找**（`Ctrl+Shift+F`）：对整个目录递归搜索（支持 `*.c;*.h` 文件类型过滤；大小写敏感与全词匹配开关与编辑器查找共享并持久化），结果按文件 / 行号列出，点击直接打开并跳转；自动跳过 `.git`、`node_modules`、`build` 等构建目录。
- **文本转换**（编辑 → 转换）：选区大小写转换（无选区时转光标所在词）、选区行排序、去除相邻重复行、删除空行、剪裁行尾空白、Tab 转空格、行首空格转 Tab、合并选中的行、转到匹配的括号。
- **Cliptext 片段库**：侧栏「片段」页预置通用 / HTML / C / Python / Shell 常用代码片段，点击插入光标处。
- **配置持久化与会话恢复**：主题、字号、面板、查找词、最近文件等自动保存到用户配置目录，退出时记录打开的文件与各自的光标位置，下次启动自动恢复（未保存的内容不会被记录）。自定义服务商还可为每个服务商配置 temperature 与 max_tokens（留空使用服务默认值）。
- **安全与工具**：保存时可选保留 `文件名.bak` 备份；「工具」菜单可用系统默认程序打开当前文件、定位文件所在目录。

## 从源码构建

需要 CMake ≥ 3.16 与 C99 编译器（Windows: MSVC 或 MinGW-w64；macOS: Clang/Xcode CLT；Linux: GCC + X11/OpenGL 开发库）。

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
# 产物: build/RapidEditor(.exe)
```

### Android 平板（实验性）

同一份 C99 核心，经 SDL2 运行在 NativeActivity 上，面向平板外接键鼠的 Dex / PC 模式（触摸由 SDL 自动映射为鼠标，文本框自动拉起软键盘）。触摸/键鼠之外的平台差异：HTML 预览走 `SDL_OpenURL` 系统浏览器；CJK 字体回退取 `/system/fonts/NotoSansCJK*`；文档默认存放在应用私有目录 `files/docs/`（首次启动自动播种示例文件）。

本地构建 APK 需要 Android NDK + SDK + Java 17：

```sh
# 1) 下载 SDL2 源码（作为子项目参与每个 ABI 的构建）
curl -fLo .cache/android/SDL2.tar.gz https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.tar.gz
tar xzf .cache/android/SDL2.tar.gz -C .cache/android/

# 2) 生成 APK（默认 arm64-v8a；-PRE_ABIS=x86_64 可加模拟器架构）
cd android
../.cache/android/gradle-8.9/bin/gradle :app:assembleDebug --no-daemon
# 产物: app/build/outputs/apk/debug/app-debug.apk
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
| Windows | NSIS `.exe`（MUI2 标准安装向导） | `packaging/windows/build_installer.bat` | `windows-universal` 一个包内含 x64 + ARM64 两份二进制；安装时用 `IsNativeARM64` / `RunningX64` 自动选 | `HKCR\*` + `HKCR\SystemFileAssociations\.ext`（89 种常用文本/代码类型）+「打开方式」注册：「用 RapidEditor 打开」 |
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
git tag v1.1.3 && git push origin v1.1.3
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
