# 实现说明

## vendored CUI

`vendor/CUI/` 是 [yappologistic/CUI](https://github.com/yappologistic/CUI) 的一个子集（只保留构建
RapidEditor 所需的 `CMakeLists.txt`、`cmake/`、`include/cui/`、`src/`、`third_party/`、`LICENSE`、
`THIRD_PARTY.md`）。相对上游有 5 处本地修改：

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

4. **Markdown 数学公式 / Mermaid**（`src/cui_markdown.c`）：
   - TeX 子集先解析为保留结构的 IR（`md_math_ir`：TXT/GAP/FRAC/SQRT/SCRIPT/BINOM/MAT/
     ROWBR/COLBR）。行内 `$...$` 把 IR 压平回 Unicode 近似（与旧版行为一致）；
   - **块级 `$$...$$` 走真实排版**（`md_m_layout` + `md_math_paint_cb`，CUI canvas 自绘）：
     分数上下堆叠 + 分数线（`\frac/\dfrac/\tfrac`）、上下标用 0.7 倍字号真实升/降
     （不再出现 `^(...)` 记法）、`\sqrt` 带覆盖被开方数的顶线、`\binom` 上下堆叠 + 括号、
     矩阵环境（`pmatrix/bmatrix/vmatrix/Vmatrix/cases/align/aligned/array`）列对齐 +
     跨全部行的定界符（括号用折线画，`|`/`‖` 用直线）、`\\` 分行 `&` 对齐、
     字母（ASCII + 希腊）用 `md_shear` 合成斜体；超宽时按 item 折行、每行居中；
   - 符号映射逐项核对过内嵌 Inter 与 CJK 回退字体（msyh）的 cmap，两边都没有字形的
     命令退化为可读的 ASCII/文字（如 `\Rightarrow`→`=>`、`\nabla`→`grad`），不会出现
     空白方块；重音（`\hat/\bar/\tilde/\vec/\dot` 等）优先用预组合字符（`\hat y`→`ŷ`）；
   - 行内多字符上标：逐字符映射上标字形（`x^{n+1}`→`xⁿ⁺¹`），无字形才退回 `^(...)`；
     `\left(x\right)` 不再丢弃定界符；
   - ```` ```mermaid ```` 代码块：解析 `graph/flowchart TD|LR` 的节点声明（`[]`、`()`、`{}`、
     `(())`、`[[]]` 子例程）和连线（`-->`、`---`、`-.->`（虚线）、`==>`，支持 `|标签|` 与
     链式 `A --> B --> C`、自环 `A --> A`），用 CUI canvas 自绘。布局按破环后的最长路径
     分层并做重心排序减少交叉；正向边走正交折线并按车道错开，同层边直连，回边走图外
     侧通道，自环画在节点空侧；节点带阴影、起点节点用强调色；边标签不骑线：水平线段
     的标签整体放在线上方、垂直线段放右侧、斜线段沿法线方向整体偏移（都带不透明底板，
     任何线条都不会穿过文字）；无法识别的内容回退为普通代码块。

5. **编辑器字体 API**（`include/cui/cui.h`、`src/cui_text.c`）：
   - `cui_font_load_deflated()`：从 raw DEFLATE 流加载 TrueType 字体，内部复用
     `cui__inflate()` + `cui_font_load_memory()`，解压缓冲由 context 持有；
   - `cui_font_role_font()`：返回某个角色当前解析到的具体字体 id，让 app 在
     临时切换角色后能恢复原字体（编辑区「默认 Geist Mono」选项需要）。

第 3 点是右键菜单在 macOS 上真正能打开文件的必要条件：Finder 的「打开方式」、
Quick Action 和拖到 Dock 图标都是 Apple Event，不是命令行参数。上游 CUI 只处理了窗口内拖放
（`cui_input_file_drop()`），无法接收这些事件。

五处改动相对上游 CUI 的完整 diff 保存在 `docs/patches/`：
`CUI-cjk-caret.patch`、`CUI-cjk-largefile.patch`、`CUI-openwith.patch`、
`CUI-markdown-math-mermaid.patch`、`CUI-editor-font.patch`。上游升级时可以按这些 patch 重新套用。

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

llvm-mingw 的 `wrl.h` 只提供 `ComPtr`，没有 MSVC 的 `Microsoft::WRL::Callback`。
`src/np_webview2_bridge.cpp` 在 `__MINGW32__` 下用两个手写 COM 回调对象替代，
MSVC 构建仍走标准 WRL；不影响运行时行为。

## NSIS 编码

`packaging/windows/installer.nsi` 必须是 **带 BOM 的 UTF-8**。NSIS 3 默认按系统 ANSI
代码页读取 `.nsi`；没有 BOM 时中文菜单文字会报 `Bad text encoding`，在英文 runner 上同样如此。
`.bat` 文件则刻意保持纯 ASCII，避免 cmd 代码页问题。

## EditPlus 风格增强（第二轮）

### 配置文件

位置：Windows `%APPDATA%\RapidEditor\config.ini`；macOS
`~/Library/Application Support/RapidEditor/config.ini`；Linux
`$XDG_CONFIG_HOME/rapideditor/config.ini`（缺省 `~/.config/rapideditor`）。
`np_config_load()` 在 `main()` 里任何 UI 初始化之前调用；`np_config_save()` 在
`cui_app_run()` 返回后调用，所以菜单退出、窗口关闭（含 macOS 直接点红点）都会保存。
`dark_valid` 标记配置里是否记录过主题：记录过就覆盖系统主题，否则跟随系统。

会话恢复：保存时只记录**有路径且不脏**的文档；启动时命令行没有文件才恢复，
命令行文件优先。所以未保存的内容永远不会被"恢复"出来，也不会丢。

### 在文件中查找

目录遍历直接用 CUI 的 `cui_fs_list`（它内部已处理 Windows 宽字符路径），不再自写
dirent / FindFirstFileW。BFS 队列上限 64 个目录、每目录 512 项；结果上限 400 行、
64 个文件、扫描 4000 个文件、单文件 2 MB；含 NUL 的文件按二进制跳过。每行只记第一条
命中。点击结果 → `np_open_path`（已打开就切标签）→ `cui_code_editor_goto` 跳行。

### 文本转换

全部通过 `np_splice()`（一次 memmove + memcpy + `np_text_changed`）完成，CUI 的行索引
与撤销栈由 revision 机制重建。行管线在 `d->tmp` 里拆分 NUL 结尾的行指针数组，
排序 / 去重 / 剪尾 / 删空行后重组。`TR_SORT` / `TR_UNIQ` 需要选区；
`TR_STRIP` / `TR_NOEMPTY` 无选区时作用于全文。大小写转换只改 ASCII 字节，
多字节 UTF-8 原样保留。

### 快捷键语义调整

`Ctrl+Shift+F` 从「当前文件列出匹配」改为 EditPlus 的「在文件中查找」；
原功能移到 `Ctrl+Shift+L`。帮助对话框已同步。

## 十六进制编辑（010 Editor 风格）

核心是 CUI 内置的 `cui_hex_view`（vendor/CUI/src/cui_hex.c）：虚拟滚动（只构建可见行）、
点击 / 拖选 / 全部键盘导航 / Ctrl C 复制都是控件自带能力。`.edit` 传入可写缓冲后，
键入十六进制数字直接覆盖光标字节（高位在前），**编辑不改变文件大小**——所以没有
插入 / 删除字节，也没有撤销栈（这是与 010 Editor 的主要差距）。

RapidEditor 侧的接线：

- **判定**：`np_sniff_binary` 打开文件读前 8 KiB——出现 NUL 字节，或控制字节
  （0x00-0x08、0x0E-0x1F，排除 \t \n \f \r \e）超过 1/10 即按二进制处理；
  `np_open_path` 据此分流到 `np_load_binary`（原样读入，不做编码转换 / 行归一化）。
- **存储**：二进制内容仍放在 `np_doc.text`（`d->len` 为字节数，末尾补 NUL），
  `d->is_hex = true`。所有文本编辑器路径（语法、统计、查找替换、转换）都按此标记跳过。
- **面板**：`np_hex_pane` = 偏移跳转输入框 + 提示行 + `cui_hex_view`；
  `r.edited` 时置 dirty 并刷新标题。视图 id 与文本编辑器相同（`d->editor_id`），
  所以每个标签的滚动位置 / 光标独立保存。
- **保存**：`np_save` 对 `is_hex` 文档直接 `fwrite` 原始字节，跳过 BOM / CRLF / ANSI 转换；
  `.bak` 备份逻辑照常生效。
- **模式切换**：「文件 → 以十六进制/文本模式重新打开」与工具栏按钮（CPU 图标）。
  有未保存修改时拒绝切换，避免丢失；`np_load_into` 会清掉 `is_hex` 标记。
- **偏移解析**：`np_hex_parse_offset` 只接受十六进制（`1A2B` / `0x1A2B`，容忍首尾空白），
  超出文件末尾时跳到最后一个字节并提示。已单测：解析 12 个用例、嗅探启发式 7 个用例。

已知的边界：无字节插入 / 删除（hex view 不改长度）、无撤销、无查找字节序列（可用
「在文件中查找」按文本搜索）、会话恢复时二进制文件会以文本模式重开。

## 界面现代化

CUI 的主题结构体可以通过 `cui_theme_get()` 直接改。`np_apply_theme()` 每帧写入圆角、
控件高度、字号刻度和四级阴影——每帧写入是刻意的：系统主题切换或 `cui_set_theme()` 可能
重置主题，这样无需在每个主题切换点重复调用。三档密度只改 `control_sm/md/lg`，
不触碰布局比例，标签 / 面板 / 状态栏自动适配。

顶栏是 brand row + menu bar + toolbar 三层共用一块 `CUI_COLOR_SURFACE` 容器；
菜单与工具栏都用 `no_border` 去掉各自的分隔线，避免视觉碎片化。文档面包屑放在编辑器
面板内部（文本与十六进制共用），显示目录 / 文件名、未保存与模式徽标。输出面板的查找
数量改为 `cui_badge` 数字徽标，状态栏左侧新增 TEXT/HEX 模式项。

空白会话的欢迎页由 `np_welcome_pane()` 绘制：仅在「唯一标签 = 空白未命名文档」且未被
关闭时显示；「直接开始输入」只是设置 `welcome_dismissed` 并聚焦编辑器，不会写入配置。
「十六进制打开」走 `np_open_hex_dialog()`：普通文件对话框 → `np_open_path` →
`np_hex_reopen(true)`，因此已打开过的文件也能强制切到 hex 模式。

`system_theme` 配置项解决了一个旧问题：`dark` 总是被写入配置，导致「深色」字段永远
被读回并覆盖「跟随系统」。现在保存时写 `system_theme=1`（跟随系统），加载时它在
`dark` 之后解析并清掉 `dark_valid`。

F1 现在绑定帮助对话框（顶栏和欢迎页都提示了它）。

## 编辑器字体（自定义字体 + 预置开源字体）

编辑区组件 `cui_code_editor` 的所有文字都用 `CUI_FONT_MONO` 角色绘制（`vendor/CUI/src/cui_code.c`），
所以换编辑器字体 = 换这个角色的字体。实现位于 `src/rapideditor.c` 的「Editor fonts」小节：

- **预置字体**：5 款 SIL OFL 开源等宽字体（JetBrains Mono、Cascadia Code/Mono、
  Source Code Pro、IBM Plex Mono）以 raw DEFLATE 压缩内嵌进 exe。数据在
  `src/np_font_bundle.c/.h`，由 `scripts/make_font_bundle.py` 从 `assets/fonts/*.ttf` 生成；
  CMake 把 `np_font_bundle.c` 编入目标。压缩后约 600 KB（原 1.37 MB），随 exe 一体分发，
  三个平台的打包脚本都无需改动。字体许可证在 `assets/fonts/`。
- **自定义字体**：视图菜单「自定义字体文件…」用原生文件对话框选 `ttf/otf/ttc`，
  经 `cui_font_load_file()` 加载。仅接受 TrueType 轮廓——CUI 用 stb_truetype，不支持 CFF/OTF 轮廓。
- **切换机制**：`cui_font_set_role(MONO, font)`。关键陷阱：CJK fallback 挂在**字体对象**上而非
  角色上，换角色会丢失中文 fallback；因此每次切换后用 `np_font_attach_cjk()` 把启动时
  加载的系统 CJK 字体（微软雅黑/苹方/Noto CJK/文泉驿）重新挂到新字体。
- **恢复默认**：启动时 `cui_font_role_font(MONO)` 缓存内置 Geist Mono 的字体 id，
  「默认」选项直接 set_role 回它。
- **持久化**：`config.ini` 的 `editor_font=`（预置 id / `default` / `custom`）和
  `editor_font_path=`（自定义字体路径）；启动时 `np_font_restore()` 恢复，若自定义文件已丢失
  则退回默认。状态栏显示当前字体名。

验证：临时 headless 程序链接 `libcui.a` 调用两个新 API + 字体 bundle，5 款字体全部
解压/加载/切换/恢复成功，`cui_measure_text` 对含中文的文本返回有效宽度；`python3` 逐字节
校验 5 个 DEFLATE 流解压后与源 TTF 完全一致。启动冒烟无崩溃、中文无乱码。

## AI 编辑流增强

- **文件类型自适应**：`np_ai_context_label()` 把当前文档标成 LaTeX / Markdown / 代码 /
  JSON / 纯文本，三种后端 worker 共用该标签；`np_ai_default_inst(task, doc)` 按类型返回
  默认指令，不再把通用文档当成 LaTeX。
- **流式与停止**：`np_net_post_json_stream()` 在 WinHTTP 层逐块回调，
  `np_openai_chat_stream()` 解析 SSE `data:` 帧并把 `delta.content` 追加到结果；
  UI 在生成过程中显示部分内容。`np_ai_exec_cancel()` 让本地 CLI 每 100 ms 检查一次取消，
  Maclaw 在响应返回后检查。取消状态用 `ai_done == 5` 与 `ai_cancel` 表示，可重试。
  POSIX 的 JSON POST 仍走原有非流式兼容路径。
- **逐处 Diff**：`ai_staged` 保存“准备写入”的文本。`cui_diff_compute()` 给出 hunk 后，
  接受会把 staged 向 AI 结果推进，拒绝会把结果向 staged 回退；最终写入的是 staged，
  而不是整个 AI 回答。
- **选区工具条**：编辑区在检测到选区时显示一行 AI 快捷按钮，复用原来的
  `np_ai_quick()`，不需要先打开右侧面板。
- **文档级历史**：每个 `np_doc` 保存最近 4 条 AI 结果、原上下文和目标区间，可在
  AI 面板恢复；历史是会话级的，不写入 config.ini。
- **草稿恢复**：`np_save_drafts()` 每 60 秒及退出时把未保存文本写入配置目录的
  `draftN.txt`，`config.ini` 记录原路径 / 编码 / CRLF。启动时 `draft:N` 会调用
  `np_restore_draft()` 恢复为未保存文档；十六进制文档不参与文本草稿。

MSVC / Ninja 配置（`build-msvc`）已编译通过；编译时需让 `cl.exe` 能看到 MSVC 与
Windows SDK 的 `INCLUDE` / `LIB`（通常在 Visual Studio 开发者环境中完成）。

## AI 编辑流增强 · 第二批

- **编译错误修复**：`np_ai_fix_compile()` 在 LaTeX 预览失败的面板中提供「让 AI 分析编译错误」，
  把 `tex_job.log` 写入 `ai_extra`，用 `np_ai_build_prompt()` 统一拼进三种后端（本地 CLI、
  Maclaw、OpenAI 兼容）的提示词。任务设为 4（自定义指令、Diff 展示）并强制全文上下文，
  修复结果仍需逐处接受才写入。`ai_extra` 在 AI 面板可查看和手动移除，重试时保留。
- **自定义 AI 动作**：`np_ai_action`（名称 + 指令，最多 8 条）持久化到 `config.ini` 的
  `aiact<N>_name` / `aiact<N>_inst`。任务下拉索引 >= 7 即动作序号，右键菜单也列出前 4 条。
  `np_ai_default_inst()` 接受 notepad 指针以读取动作指令；回答类结果新增「插入到光标处」
  （`np_ai_insert_here()`，与原文目标区间隔离，避免误替换全文）与「复制结果」。
- **智能粘贴**：`np_smart_paste_open()` 从剪贴板取内容，按首字符和逐行分隔符计数判定
  JSON / 表格（Tab/CSV/分号）/ 日志 / 纯文本，转换结果写入 `sp_out` 供对话框预览。
  表格按当前文档类型生成 Markdown 或 LaTeX 三线表；日志提取含 ERROR/WARN/FATAL/异常
  关键词的行并去重（最多 40 行）。「转换并插入」与「原样粘贴」都在 `np_smart_paste_insert()`
  里按光标位置插入并刷新行索引，不自动改写剪贴板内容。
- 新增 `np_ai_prompt_size()` / `np_ai_build_prompt()` 统一三种后端的提示词拼装，
  避免为附加上下文重复修改多处 worker。

构建陷阱：对话框函数在前、实现在后时必须加前向声明，否则 MSVC 按隐式 `int` 推断，
链接前的编译即报 `C2371 redefinition; different basic types`。链接时若旧 exe 正在运行，
`LNK1104 cannot open file` 需先结束占用进程。MSVC 构建命令同上一节。

## 可控上下文引用 · 第三批

- **来源开关**：`ai_ctx_tabs` / `ai_ctx_finds` 两个布尔值（持久化到 `config.ini`），
  在 AI 面板以复选框呈现。默认均为 false，即和以前一样只发选区 / 全文，不会把
  其他标签页或搜索结果悄悄上传。
- **统一拼装**：`np_ai_refresh_sources()` 在 UI 线程把勾选的来源拼进 `ai_ctx_sources`
  （16 KB 上限），单个标签页只取前 4000 字节，查找结果最多 40 条，截断时置 `cut`
  并在摘要里显示「过长内容已截取」。`np_ai_prompt_size()` / `np_ai_build_prompt()`
  把该块和编译日志一起送进三种后端，worker 侧无需感知来源差异。
- **发送前预览**：`ai_ctx_summary` 显示「本次发送：选区 + 其他标签页(3) + 查找结果(12)
  + 编译日志 · 约 N KB」。勾选变化或切换标签时刷新；`np_ai_start()` 在发请求前再
  快照一次，保证预览和实际发送一致。`ai_ctx_src_doc` 记录预览对应的标签索引。
- **隔离**：`np_ai_quick()`（选区内联 / 右键一键操作）和 `np_ai_fix_compile()`
  会重置两个开关，避免快捷操作意外带上无关标签页或搜索结果。

## 工作区语义搜索 · 第四批

- **入口**：「在文件中查找」对话框 footer 的「AI 语义搜索」，用 `ff_query` 作为自然语言描述，
  复用 `ff_dir` / `ff_pattern` 和 `np_ff_run` 的 BFS 扫描规则（NP_FF_SKIP 目录、
  文件数 / 扫描数上限）。
- **提示词**：`np_sem_build_prompt()` 扫描候选文件并把每个文件按编号 + 路径 + 前 6 行摘录
  写进 `sem_prompt`（32 KB 上限），要求 AI 只返回 `编号|行号|理由`，最多 12 条。
- **防幻觉**：`np_sem_apply()` 只信任 AI 给的文件编号和行号，展示的原文片段由
  `np_sem_line_text()` 从本地文件第 N 行真实读回；编号越界 / 二进制文件不会产生结果。
  结果写入既有 `ff_hits` / `ff_paths`，复用「文件搜索」面板和 `np_open_fhit()` 跳转，
  因此每条命中都有文件名、原文行和行号。
- **worker**：`np_sem_worker()` 复用三种后端（本地 CLI / MaClaw / OpenAI 兼容），
  与 AI 面板状态完全隔离（独立 `sem_*` 字段与临时文件路径），支持 `sem_cancel` 停止。
- **状态卫生**：普通关键词搜索和清空「文件搜索」面板都会重置 `sem_done`，避免旧的
  AI 语义结果状态残留；输出面板在 `sem_done` 变化时只 apply 一次。

MSVC 构建通过（显式 INCLUDE/LIB 环境，命令同上一节）。

## 隐私与配置体验 · 第五批

- **API Key 遮罩**：`cui_input(..., .password = true)`，眼睛按钮可临时查看。
- **DPAPI 加密存储**：`np_secret_save()` / `np_secret_load()` 用 `CryptProtectData` +
  `CryptBinaryToStringA` 把密钥写成 `llm_p<N>_key=dpapi:<base64>`；读取时无前缀的值按
  旧明文处理，解密失败（换用户 / 换机器 / 配置被篡改）则留空要求重新输入。
  `CRYPTPROTECT_UI_FORBIDDEN` 避免后台弹 UI，明文缓冲区在释放前 `SecureZeroMemory`。
  CMake 的 WIN32 分支新增 `crypt32` 链接；非 Windows 构建降级为明文并在 README 注明。
  临时测试程序验证：密文不含明文、往返解密一致、旧明文兼容、损坏密文返回空。
- **.aiignore**：`np_ignore_load()` 从扫描根目录读取，`np_ignore_check()` 匹配；
  支持 `#` 注释、末尾 `/` 表示仅目录、`*.ext` 后缀、`*`/`?` 通配符与含 `/` 的相对路径
  （裸名字任意层级匹配）。接入 `np_ff_run()`、`np_sem_build_prompt()` 与
  `np_ai_refresh_sources()` 的「其他标签页」；摘要分别显示「.aiignore 已生效」和
  「.aiignore 排除 N」。缓存按加载根目录失效。
- **构建陷阱（复发）**：`np_ai_refresh_sources()` 调用的 ignore helper 定义在文件后部，
  未加前向声明被 MSVC 隐式推断为 `int`，报 `C2371 redefinition; different basic types`。
  已在该函数前补 `static` 前向声明——这是本项目第二次踩同一坑。

MSVC 构建通过（显式 INCLUDE/LIB 环境，命令同上一节）。

## 代码审查修复 · 第六批

这一轮没有新增功能，而是审查前五批实现后修复的真实缺陷：

- **AI 写入错文档 / 版本冲突（严重）**：`np_ai_apply()` 原来靠 `ai_target_path` 匹配文档，
  但所有未命名标签的 `path` 都是 `""`，两个未命名标签会互相错写；且注释声称检查长度
  未变，实际只 clamp 偏移。现在用 `np_doc.uid` 定位发起请求时的文档（新增
  `ai_target_uid` / `ai_target_rev` / `ai_target_len`），要求仍是当前标签且
  `content_rev` 与长度均未变，否则 toast 拒绝写入。AI 历史也按 uid 归属并记录快照。
- **.aiignore 在 Windows 多级目录失效**：`np_dir_join` 产生反斜杠，与规则里的 `/`
  比较失败；且大小写策略不一致。新增 `np_ignore_norm()` 把路径和规则统一为小写 +
  正斜杠，所有比较分支一致不敏感。缓存失效改为按 `.aiignore` 的 `st_mtime`，原地
  编辑后下次扫描即生效。
- **语义搜索编号错配**：`np_sem_apply()` 原来用共享 `ff_paths` 解析 AI 返回的编号，
  若期间跑过关键词搜索，编号会指向别的文件。新增 `sem_paths` / `sem_count` 快照，
  结果解析前把文件表恢复为该快照，并把「搜索」按钮在语义搜索运行时禁用。
- **流式输出边界**：SSE 缓冲满时回调返回 false 被 `np_net.c` 统一报「已取消」，
  掩盖了真正的截断原因。现在只有 cancel 标志置位才报「已停止」，截断提示在
  通用失败分支之前写出。`np_openai.c` 的 `\uXXXX` 解码新增 UTF-16 代理对合并，
  `\ud83d\ude00` 现在生成合法的 4 字节 `F0 9F 98 80`，不再是非法 UTF-8。
  临时测试程序链接 np_openai.c.obj 验证：emoji 与 BMP 字符解码均通过。

MSVC 构建通过（显式 INCLUDE/LIB 环境，命令同上一节）。

## 稳定性与边界修复 · 第七批

沿用第六批的「先审查、逐条核实、再动手」流程；子代理报的 15 条里先对关键项
逐字读源码确认，剔除 3 条夸大的/非问题项后再修：

- **大文件 AI 结果删尾（数据丢失，严重）**：`np_ai_start()` 把 `ai_orig` 硬截到 64 KB，
  但 `ai_target_e` 仍记完整 `d->len`；对超过 64 KB 的文档/选区做改写，接受 Diff 会把
  AI 根本没看到的那部分正文删掉。现在截断时把 `ai_target_e` 收紧到实际发送的末尾
  （选区 `s0+n`、全文 `n`），并弹提示「仅发送前 64 KB，其余内容保留」。
- **补全续写插错位置**：任务 5 原来用选区起点 `s0` 作为插入点，反向/正向选中后再
  续写会把结果插到选区前端。改为用真正的光标 `d->caret`，上下文也围绕光标取
  （前 3000 / 后 1000 字节），不再依赖选区两端。
- **线程句柄泄漏**：Windows 下 `NP_BEGIN_THREAD` 丢弃 `_beginthreadex` 的返回值，
  每次 AI / 语义搜索 / 模型列表请求都漏一个内核 HANDLE。宏改为创建后立即
  `CloseHandle`（线程自身 `_endthreadex` 不会释放句柄），POSIX 分支不变。
- **停止 / 截断被误报为失败**：`np_ai_finish()` 原来只认 `strcmp(err,"已停止")`，
  WinHTTP 层写的「已取消」会显示成红色「AI 调用失败」；`np_openai_chat_stream()`
  在缓冲满时 `return ok && out[0] && !truncated`，把「部分内容有效」判成失败。
  现在两种停止措辞都归到 `ai_done=5`，截断返回部分成功并在完成态顶部显示原因。
- **maclaw 后端 emoji 乱码**：`np_maclaw.c` 的 `json_unescape()` 未合并 UTF-16 代理对，
  `\ud83d\ude00` 被编成非法的 `ED A0 BD`。补齐与 `np_openai.c` 一致的高/低代理项
  合并与 4 字节编码。`scripts/test_maclaw.c` 新增代理对回归断言，MSVC 编译运行
  `ALL TESTS PASSED`。
- **服务商配置跨线程写**：`np_llm_models_worker` / `np_llm_test_worker` 原来直接读写
  `llm_prov[idx]`（URL/key/model/models），请求未返回时编辑或删除服务商会把结果写进
  已被 `memmove` 复用的槽位。现在 start 时在 UI 线程快照到专用字段，worker 只读快照；
  新增 `llm_prov_gen` 代次，表单 / 列表一变动就递增，worker 完成时丢弃过期结果。
  顺带把「测试连接」的耗时从 `clock()`（CPU 时间）改为 `difftime()`（墙钟时间）。
- **密钥解密失败无提示**：`dpapi:` 值在换账户 / 换机器后静默变空，用户只看到 401。
  配置加载时标记 `secret_load_failed`，「LLM 服务」对话框给出明确提示，重新保存后清除。

MSVC 构建通过（显式 INCLUDE/LIB 环境，命令同上一节）。

## 数据完整性与撤销 · 第八批

同样先只读审查、逐条核实，剔除了「ai_result strlen 竞态」等夸大项，修复 6 条真实缺陷：

- **配置保存字段互相覆盖（严重，长期存在）**：`np_cfg_one_line()` 返回它写入的 buffer
  指针，而 `np_config_save()` 在同一个 `fprintf` 里把 `cfgline1`/`cfgline2` 各用了两次
  （maclaw_centers 与 maclaw_token、llm 的 name 与 model、url 与 models）。C 的实参都在
  调用前求值完，两个 `%s` 拿到同一指针 → 都打印最后一次写入的值。**每次退出保存都在
  损坏配置**：重启后中心地址变成 token、服务商 URL 变成模型串。改为每个字段一条
  `fprintf`、各用独立 buffer；`replace=`/`workdir=` 也补上换行折叠。
- **AI 改写无法撤销且污染撤销栈（严重）**：CUI 的 delta 撤销用绝对字节偏移，而
  `np_ai_apply()` / `np_ai_insert_here()` 直接 `memmove` 正文、只 bump revision；AI 改写
  改变长度后，下一次 Ctrl+Z 会拿过期偏移改错字节。给 vendor/CUI 新增公开 API
  `cui_code_editor_replace_external()`，复用内部 `cui__ed_replace` 把外部改写记为一次
  正常撤销步骤；两处 AI 写入改走该 API。用户现在可 Ctrl+Z 撤销 AI 改写，偏移一致，
  帧结果 `r.changed` 仍会把 `d->len`/`content_rev` 同步回来。注意：改了 CUI 头文件触发
  全库重编，只有既有 warning。
- **草稿崩溃恢复元数据错配（中）**：`np_save_drafts()` 每 60 秒写快照，却只在退出时写
  config.ini；异常退出后新快照配旧 path/enc，显式保存可能覆盖错误文件。每次快照同时
  写 `draft<N>.meta`（path/enc/crlf），`np_restore_draft()` 优先读 meta，与字节同生共死。
- **模型列表混入假条目（低）**：`np_openai_list_models()` 对整段响应搜 `"id"`，会把网关
  的顶层 request id 当模型。改为只在 `"data"` 之后扫描。`scripts/test_openai.c` 补回归
  断言（顺带补 `np_net_post_json_stream` 桩）。
- **healthz 重定向误判（中）**：`np_net_get_json` 把 3xx 判失败，导致会跳转到 https 的
  中心被跳过。`np_maclaw_pick_center` 在 resp 为空但 status 是 3xx 时仍视为可达。
- **取消路径内存泄漏（低）**：`np_net_request_win` 在取消 / 回调 false / realloc 失败时
  `goto done` 没有释放 `buf`。done 处统一 `if (buf && result != buf) free(buf)`。

`scripts/test_maclaw.c`、`scripts/test_openai.c` 均 `ALL TESTS PASSED`；MSVC 构建通过。
注意 CUI vendor 此次被修改（新增一个 API），升级 vendor 时需保留该函数。

## 逐处 Diff 与撤销完整性 · 第九批

继续只读审查 + 逐条核实（剔除了夸大项），修复 4 条真实缺陷并清理 1 个死配置：

- **逐处 Diff 整体失效（严重，第一批引入）**：`np_ai_finish()` 把 `ai_staged` 初始化成了
  `ai_result`，而 diff 视图比较的正是 staged vs result——两边相同，`dr.changes == 0`，
  「接受当前处 / 拒绝当前处」永远禁用，「逐处确认后才写入」名存实亡（点「全部应用」直接
  写整段）。按设计意图（历史恢复路径 `np_ai_restore_history` 正是把 staged 设为原文
  `h->orig`），改为 `ai_staged = ai_orig`。
- **智能粘贴污染撤销栈（高）**：`np_smart_paste_insert()` 裸 `memmove` 插入正文，与第八批
  修复前的 AI 写入同源——Ctrl+Z 会拿过期字节偏移改错位置。改走
  `cui_code_editor_replace_external()`（空区间替换 = 插入），粘贴成为一次可撤销的编辑步骤。
- **逐处 hunk >128 时越界读（中）**：`cui_diff_compute()` 返回真实 hunk 总数但只填充前
  `NP_AI_HUNK_MAX`(128) 项，`np_ai_hunk_decision()` 的 `idx` 却 clamp 到 `n-1`；经历史恢复
  入口制造 >128 个 hunk 再点选后面的项，会读到未初始化栈内存并传给补丁函数。改为 clamp 到
  `min(n, 128) - 1`。
- **语义搜索「清空」引发并发 worker（中）**：输出面板垃圾桶在 `sem_done==1` 时直接把状态机
  清零，旧 worker（detach、外部进程耗时）仍在写共享的 `sem_ctx_path/sem_out_path` 与
  `sem_result`，此时可再启动一轮搜索——两轮交错写坏临时文件，旧结果还会配上新一轮路径
  快照。改为运行中点清空只置 `sem_cancel` 并清显示文本，保留运行状态；结束后再点才真正清零。
- **死配置 `ai_show_diff` 清理**：该字段只在配置读写与「清空对话」置 false 中出现，从未被
  读取控制任何行为。删除字段与相关读写。

MSVC 构建通过；`scripts/test_maclaw.c`、`scripts/test_openai.c` 均 `ALL TESTS PASSED`。

## 版本 1.3.0

九批累计改动（AI 全流程、可靠性、撤销完整性、配置/草稿安全）发布为 v1.3.0。

## 第一组：程序化编辑撤销统一 + AI 围栏剥离 + 编码提示（v1.3.0 后）

按方向评审实施的第一组（正确性收尾）：

- **程序化编辑统一走撤销栈**：`np_splice()` 改为通过
  `cui_code_editor_replace_external()` 写入（一处修改、行变换类全部受益）；
  `np_insert_at_caret`（LaTeX 块/表格/时间戳/菜单插入，非打字路径）、
  `np_latex_wrap`、`np_latex_ensure_package`、`np_latex_preamble_line`、
  `np_md_set_heading`、Markdown 行重排、`np_md_toggle_task`（勾选/取消也可
  撤销了）、`np_md_insert_para` 全部改为调用 `np_splice`。grep 确认
  `memmove(d->text` 清零——正文不再有任何绕过编辑器历史的写路径。
- **AI 结果代码块剥离**：`np_ai_strip_code_fence()` 在任务 0/1/4/5/6（结果
  将写入文档）完成时，剥掉单个包裹性 ```` ```lang ... ``` ```` 围栏；任务
  2/3（问答）保留原样。保守策略：仅当整段是单一围栏块（首行开、末行关、
  中间无任何 ``` 开头的行）才剥，markdown 文档本身不受影响。
  `scripts/test_fence.c` 用 awk 从源码提取真实函数做回归（12 用例），测试
  抓到过「``` mid」行未被识别为围栏的漏洞后收紧了条件。
  **坑：re.sub 的 repl 字符串会转义 `\n` 等——用 lambda 作 repl 传入。**
- **编码风险提示**：`np_utf8_to_ansi()` 增加 `lossy` 出参（WinAPI
  `lpUsedDefaultChar` 检测），保存 ANSI 文档有字符被替换为 ? 时 toast 建议
  另存 UTF-8；打开 UTF-16 文件转换时 toast 明确「保存时将写为 UTF-8」。

MSVC 构建通过；`test_fence`(12)、`test_maclaw`、`test_openai` 全绿。

## 第二、三组：会话光标恢复 + AI 参数 + 工作区问答（v1.3.0 后）

按方向评审实施的第二批与第三批：

- **会话恢复补光标位置**：核实发现会话恢复（`session<N>`/`session_restore`）
  此前已存在——上一轮评审说「没有会话恢复」是 grep 关键词不对导致的误判。
  增量补齐：config 保存 `session_cur<N>`（各文件退出时的光标偏移），启动
  恢复时按 startup 平行数组 `startup_cur` 回填 caret 并 select 定位滚动。
  命令行打开的文件不恢复光标（-1）。**教训：判断「某功能不存在」前先全面
  grep 功能关键词本身（session），而不是只搜自己猜的 API 名。**
- **AI 采样参数**：`np_llm_provider` 加 `temperature`/`max_tokens`（0=不发送），
  `np_openai_set_params()` 全局下发，chat/stream 的 JSON body 按需插入字段；
  服务商编辑表单加两个输入（进入编辑时格式化、保存时解析 clamp），配置
  持久化为 `llm_p<N>_temp`/`llm_p<N>_maxtok`。主 AI 与语义搜索 worker 在
  使用快照前设置参数（`sem_prov` 为整体赋值快照，自动携带新字段）。
- **工作区问答模式**：`sem_mode` 开关（查找对话框底栏切换按钮）。问答模式
  的 prompt 要求「只依据清单内容回答、引用用 [编号] 标记，末尾输出
  引用：列表（编号|行号|理由）」——现有 `np_sem_apply` 的逐行解析天然兼容
  （命中行照常入库供跳转），其余行进 `sem_answer` 作为回答正文显示在
  引用列表上方。防幻觉机制不变：跳转目标与行内原文始终从本地文件读回。

MSVC 构建通过；三组回归全绿。改动为 v1.3.0 之后的常规提交，未打新 tag。

## 行内补全（Copilot 式 ghost text）

给 CUI 编辑器与编辑器面板加上 AI 行内续写建议：

- **CUI 侧**（vendor，升级需保留）：`cui__ed_state` 新增 owned 的 ghost 缓冲与
  `ghost_cursor`；公开 `cui_code_editor_ghost(ui, id, text)`。编辑器自动管理失效：
  任何编辑（`cui__ed_replace` 内统一清除）、光标移动、Esc 都会清掉建议；Tab 在
  `cui__edf_edit_keys` 中把建议作为一步撤销编辑插入（走既有撤销栈）；渲染在光标
  绘制块中以 muted 色画在光标右侧（焦点内才显示）。
- **应用侧**：AI 面板新增「行内补全」开关（默认关，配置键 `ai_inline`）。开启且
  后端为 OpenAI 兼容服务商时：编辑/移动光标后静置 ≥1 秒且无选区、无在途请求时，
  取光标前后各约 2000/400 字节（按 UTF-8 边界取整）构造续写提示词，worker 快照
  provider 配置发起 `np_openai_chat`；结果按代次（gen）校验后设为 ghost。结果
  >600 字节或失败一律静默丢弃，不打扰输入。
- 失效与防错位：gen 在每次编辑/移动光标时递增，worker 结果带 gen 快照，不匹配
  即丢弃；ghost 显示期间不再发新请求，Tab 接受后立即进入新一轮防抖。

MSVC 构建通过；test_openai / test_maclaw / test_fence 三组回归全绿。

## 全部替换 / LaTeX 整理纳入撤销栈

排查 `memcpy(d->text` 时发现两个漏网的整缓冲裸写（与第九批 np_splice 同源）：

- **全部替换**（`np_replace_all`）：原来 `memcpy(d->text, d->tmp, o+1)` 直接覆盖
  整个缓冲，CUI 撤销栈里的 delta 全部变成过期偏移，Ctrl+Z 会改坏无关文本。
  改走 `cui_code_editor_replace_external`（0..len 全区间、单步撤销）——现在
  Ctrl+Z 一次性还原整个替换。
- **LaTeX 整理**（`np_latex_tidy_apply`）：同样整缓冲裸写，同法修复。

`np_doc_from_latex_template` 的 memcpy 是新建文档且随后 `np_bump_editor`，
无旧撤销栈，无需处理。至此 `memmove(d->text` 与 `memcpy(d->text` 的非初始化
路径全部清零：所有对既有文档正文的写入都经过编辑器撤销栈。

MSVC 构建通过；三组回归全绿。

## 行内补全打磨（第十二批）

审查第十批的 ghost text 实现后发现一个停摆 bug 与几个打磨点：

- **停摆 bug**：应用侧用 `inline_ghost_shown` 标志记住"建议已显示"，但编辑器
  会在 Esc、光标移动时自行清除 ghost，应用侧标志却永远不复位 → 行内补全在该
  文档永久停摆（Esc 一次就中）。修法：CUI 新增查询 API
  `cui_code_editor_ghost_active(ui, id)`，应用侧 idle 触发条件与工具条提示
  都改为实时查询，删除 `inline_ghost_shown` 字段。教训：**跨层共享的"显示中"
  状态必须以持有真相的一方（编辑器）为准做查询，不要在另一方镜像一份**。
- **ghost 单行化**：ghost 渲染在光标所在行右侧（单行绘制），含 `\n` 的建议
  画出来是坏的。两处防御：CUI 端 `cui_code_editor_ghost` 截断到首行；应用端
  worker 结果先截首行再剥模型可能输出的单行围栏（```` ```xxx``` ````），
  再做 600 字节可用性判断。
- **手动触发**：工具条新增「补全」按钮（选中后端时显示）+ 编辑器右键菜单
  「行内补全（光标处）」。`np_inline_manual` 只在无在途请求时发起（两个
  worker 会竞态写共享结果缓冲，1 秒防抖窗口内等待无害）。
- **可见性提示**：ghost 显示期间工具条右侧显示 muted 文本「Tab 接受补全 ·
  Esc 忽略」，同样基于 ghost_active 查询。

MSVC 构建通过；test_openai / test_maclaw / test_fence 三组回归全绿。

## 行内补全快捷键 / 防抖可配 / 一键回归（第十三批）

- **Alt+\ 手动触发**：CUI 键枚举加 `CUI_KEY_BACKSLASH`（加在 `CUI_KEY_COUNT`
  之前，不影响既有键值），五个平台层各加一行映射（win32 `VK_OEM_5`、cocoa
  `'\\'`、glfw/sdl2/x11 各自常量）。应用层在 `np_frame` 主快捷键区注册
  `Alt+\`：仅当前文档非 hex 且编辑器有焦点时调用 `np_inline_manual`。
  `cui_shortcut` 是精确 mods 匹配（`(mods & relevant) == mods`），Alt+Shift+\
  不会误触发。
- **防抖时长可配**：`inline_delay` 字段（1..5 秒，配置键 `ai_inline_delay`，
  load 时缺省/clamp 为 1），AI 面板「行内补全」同行的下拉可选 1/2/3 秒，
  idle 触发条件改用该值。
- **一键回归**：新增 `scripts/run_tests.ps1`——自动探测 MSVC/SDK 版本
  （PS 5.1 的通配符 `*\Community\...` 会把 MSVC 目录本身当结果，必须逐层
  循环展开），编译并运行 test_fence / test_openai / test_maclaw，任一失败
  退出码非零。测试编译要点保留：`cl /I src`，链接对应实现文件（np_openai.c
  / np_maclaw.c），多源文件不能带 /Fo。

MSVC 构建通过；三组回归全绿。

## 行内补全请求参数独立 + 连续失败提示（第十六批）

- **np_openai_chat_ex**：`np_openai_chat` 的实现搬入新变体，增加
  per-call 的 temperature / max_tokens / timeout_sec（沿用 0=省略字段的
  约定，timeout<=0 回落 120s）；`np_openai_chat` 变成包装（全局参数 +
  120s）。注意 `g_temperature/g_max_tokens` 是**全局静态**（非
  thread-local，头文件旧注释有误导），所以轻量调用者必须走 chat_ex，
  不能在线程里 set_params——那会污染之后所有请求。
- **行内补全改走 chat_ex**：temperature 0（省略字段）、max_tokens 400
  （提示词要求 ≤200 字，中文 1 字约 1~2 token）、timeout 30s 快速失败。
  之前继承全局参数：用户若设 max_tokens=4096，一次续写又慢又贵。
- **连续失败提示**：inline_fail_streak/notified——连续 3 次失败 toast
  一次（配置检查提醒），成功即清零复位；日常偶发失败仍然静默。
- test_openai.c 新增 5 个 chat_ex 用例（body 携带 max_tokens/temperature、
  0 值省略字段）。

MSVC 构建通过；四组回归全绿。

## ghost 多行建议（第十七批）

行内补全此前把建议截断到首行，续写类多行建议（整段 / 整个代码块）价值折损。

- **CUI 端**：`cui_code_editor_ghost` 不再截断，保存完整文本（cap 2048 不变）；
  paint 增加 `ghost_extra`（首行以外行数），渲染仍是光标行内单行绘制——
  画首行后用 `cui__text_width` 接一个 muted 标记「 … +N」。Tab 接受路径
  `cui__ed_replace(..., st->ghost, st->ghost_len, 1)` 本就按完整长度插入
  （replace 会清理 \r），多行插入零改动即生效。
- **app 端 `np_inline_clean` 重写**：CRLF 归一为单个 LF（旧写法 `\r`→`\n`
  会把 CRLF 变成两个换行）；围栏剥壳支持多行——剥壳**前**判断 opening
  fence 是否为独立行（首个换行在 closing fence 之前且行内无反引号），
  是则整行丢弃（含语言标签）；` ```text``` ` 这类内容和 fence 同行的单行
  输出仍只剥首尾标记。
- **教训（测试抓到两个真实 bug）**：①"bare CR normalized"用例暴露 CRLF
  双换行瑕疵；②"tagless fence"用例暴露剥壳后按首行判断语言标签会把内容
  行丢掉（```` ```\nprint(1)\n``` ```` 变 NULL）。语言标签判定必须基于
  剥壳前的 fence 行结构，剥壳后的首行无法区分标签与内容。

MSVC 构建通过；四组回归全绿（test_inline 13 用例）。

## 多行能力参数链对齐（第十八批）

第十七批放开了 ghost 的多行支持，但下游参数链没跟上，本批对齐：

- prompt 规则 2：「最多两行、不超过 200 字符」→「最多四行、不超过 300
  字符；需要多行时直接用换行」。
- worker：`np_openai_chat_ex` max_tokens 400→600（300 字中文约需 500~600
  token），响应缓冲 out 1024→2048。
- 可用性阈值：`strlen(s) < 600` → `< 1100`——**原先 600 字节会把 prompt
  允许的中文多行建议（300 字 ≈ 900 字节）静默丢弃**，用户白等一次请求。
- 尺寸链核对：inline_result 1024→1280 > 阈值 1100 > prompt 预算 900+；
  CUI ghost cap 2048 > 1280。教训：**放开一处能力时要顺着整条链路核对
  每一级缓冲和阈值，链条最细处决定实际能力**。

MSVC 构建通过；四组回归全绿。

## 行内建议全文预览（第十九批）

多行 ghost 在编辑器里只渲染首行 +「 … +N」标记，接受前看不到全文（盲赌）。
AI 面板新增预览块：

- CUI 新增查询 `cui_code_editor_ghost_text(ui, id)`——返回编辑器持有的
  ghost 内部指针（仅同帧有效，cap+1 存储保证可安全加 NUL）。与
  ghost_active 一样走「以持有真相的一方查询」的模式，不在 app 侧镜像。
- AI 面板「行内补全」设置行下方：ghost 活跃时显示「行内建议预览（Tab
  接受 · Esc 忽略）」+ 全文（muted、自动换行）。单行建议同样受益——
  编辑器里被截断的长首行也能读全。
- 预览随编辑/Esc 实时消失（编辑器清 ghost 后查询返回 NULL），无需 app
  侧状态管理。

MSVC 构建通过；四组回归全绿。

## 外部文件修改检测（第二十批）

编辑器此前对磁盘上的外部改动（git checkout、其他工具、构建生成）完全无
感知，保存会静默覆盖外部内容。本批补齐：

- `np_doc` 增加 `disk_mtime`（加载/保存时记录，含 hex 加载与文本保存的
  两条路径）与 `disk_mtime_seen`（已处理过的外部 mtime）。
- `np_file_mtime()`：UTF-8 路径的 mtime——**Windows 上 stat() 走 ANSI
  代码页，中文路径会失败，必须 MultiByteToWideChar + _wstat**（与
  np_fopen_utf8 同一套约定）。
- `np_disk_check()` 每 2 秒轮询全部文档（挂在 np_frame 的 draft 节流块旁）：
  - clean 文本文档 → 自动重载（quiet 抑制内部「已打开」toast，改弹
    「已重新加载」），恢复光标与选区（editor_id 已换代，需
    cui_code_editor_select 回写）；
  - dirty 文档 → 警告「保存将覆盖外部内容」，不自动动作（本地修改是
    用户的工作，删除与否由用户决定）；
  - hex 文档 → 只警告不自动刷新（解析视图/模板状态不宜静默重置）；
  - 文件被删除 → 警告一次。
- mtime 去重：每次变化只处理一次（seen 记录），自己保存不触发自己。

MSVC 构建通过；四组回归全绿。

## 文件删除状态呈现（第二十一批）

第二十批的 disk check 在文件被删除时只弹一次 toast，tab 与标题毫无体现，
用户无从得知「此时保存会以当前内容重建文件」。本批补齐状态呈现：

- `np_doc.disk_missing`：由 `np_disk_check` 维护——mtime 查询失败置 true，
  文件恢复（下次轮询查到 mtime）即清除，随后照常走「外部修改」逻辑
  （clean 自动重载 / dirty 警告）。
- 呈现三处：tab 标题前缀「⚠ 」、tab tooltip 追加「磁盘文件已删除」、
  窗口标题后缀「 [文件已删除]」。`np_update_title` 每帧带 strcmp 缓存调用，
  状态变化自动反映。
- 删除 toast 描述改为「保存本文档将以当前内容重建该文件」——把静默重建
  行为提前告知。
- 顺带补「磁盘内容已变化」tooltip 标记：dirty/hex 文档的外部修改不自动
  处理（seen 停留在新 mtime 上），`seen != mtime && seen != 0` 即为
  未处理的陈旧状态，保存/重载后自然消失。

MSVC 构建通过；四组回归全绿。

## 手动「从磁盘重新加载」（第二十二批）

外部修改闭环的最后一环：dirty / hex 文档的外部变化只能警告，用户想放弃
本地改动拿磁盘版本只能关标签重开。

- 文件菜单「从磁盘重新加载」（有路径时可用，CUI_ICON_REFRESH_CW）+
  `Ctrl R`（未占用；F5 已被插入时间戳占用）。
- `np_request_reload`：clean 直接重载；dirty 弹专用确认框
  （dlg_reload，复用 confirm_desc 显示文件名），确认后
  `np_do_reload`。
- `np_do_reload`：hex 走 np_load_binary、文本走 np_load_into（quiet
  抑制内部 toast，改弹「已从磁盘重新加载」）；光标/选区恢复仅文本
  模式（hex 的 caret 语义不同）；失败弹「重新加载失败」。
- F1 帮助面板补 Ctrl+R 条目。

MSVC 构建通过；四组回归全绿。

## 空闲自动保存（第二十三批）

draft 镜像只是崩溃恢复网（配置目录里的临时副本），真实缺口是 opt-in 的
真·自动保存。VS Code 的 files.autoSave afterDelay 行为：

- 文件菜单「自动保存（空闲 5 秒，文本模式）」checkbox，配置键
  `auto_save`（默认关）。
- `np_auto_save_tick`：复用 `inline_last_edit`（每次编辑/移动光标都刷新，
  本就是行内补全的空闲时钟）做 5 秒空闲判定——不新增任何时间跟踪字段。
- **两个安全边界**（比功能本身更重要）：
  1. hex 文档排除——静默写回二进制风险太大；
  2. 磁盘有未处理的外部修改（seen != mtime）时跳过——此时自动保存正是
     np_disk_check 警告要防止的「覆盖外部内容」，决定权必须留给用户。
  disk_missing（文件被删除）不排除：自动保存按已告知的规则重建文件。
- 保存失败时把 inline_last_edit 推到当前时刻（退避一个空闲窗口再重试）。
- 顺带修复：np_any_dialog 漏了第二十二批新增的 dlg_reload。
- 调用点在 np_disk_check 之后（保存会更新 mtime，顺序不影响检测）。

MSVC 构建通过；四组回归全绿。

## 自动保存全覆盖 / 查找自动填充 / 警告引导（第二十四批）

- **自动保存扩展到全部 dirty 文本文档**：全局空闲 5 秒后遍历所有标签
  （不只是活动的），汇总 toast「已保存 N 个文档（M 个因外部修改跳过）」。
  任一失败 → 全局退避一个空闲窗口。安全边界不变（hex / seen != mtime）。
- **Ctrl F 选中即查**：np_open_find 把当前选区（≤127 字节、不跨行）填入
  查询框——选完词直接 Ctrl F 就能搜。hex 排除（选区语义不同）。
- **外部修改警告加引导**：dirty 分支补「Ctrl R 重新加载（放弃本地修改）」；
  hex 分支描述从路径改为「Ctrl R 可从磁盘重新加载」——把第二十二批的
  手动出口在警告里点明。

MSVC 构建通过；四组回归全绿。

## 配置键一致性检查（第二十五批）

连续多批加配置键后做一次脚本化审查：np_config_save 写出的每个键必须
有 load 分支，反之亦然。

- 结果：**主配置 51 load / 54 save 完全配对**，无漂移（save 多出的
  path/enc/crlf 是 draft 文件头，由 np_load_draft 专用解析，列入
  白名单）。session 光标键实际为 session_cur%d（read 用
  strncmp 前缀匹配），配对正确。
- 固化为 `scripts/check_config.py`（纯文本静态检查，不需要编译器），
  并入 run_tests.ps1 作为第一项——今后任何配置键失同步在回归阶段
  即被抓到。负向验证过：注入假保存键可被检出。

MSVC 构建通过；五项回归全绿（check_config + 4 组单测）。

## 自动保存与关闭/退出守卫协同（第二十六批）

开了 auto_save 的用户预期是「永远不被问要不要保存」，但退出/关闭 tab
时守卫仍逐个弹窗——与 VS Code autoSave 的行为预期不一致。

- `np_try_autosave_close`：auto_save 开启且文档在自动保存能力范围内
  （文本、有路径、无未处理的外部修改）→ 直接保存；否则返回 false 走
  原守卫。保存失败也返回 false（fallback 到询问，不静默丢工作）。
- `np_request_close`：dirty tab 关闭时先试自动保存，成功即直接关闭。
- `np_request_quit`：先 flush 所有可自动保存的 dirty 文档，再对剩余
  （未命名、hex、外部冲突、保存失败）逐个询问——范围外文档的询问
  语义保持不变。

MSVC 构建通过；五项回归全绿。

## 查找栏区分大小写贯通（第二十七批）

Ctrl F 打开的编辑器内嵌查找栏此前永远不区分大小写，而「查找和替换」
对话框有「区分大小写」checkbox（配置键 match_case）——两个查找入口对
「什么是匹配」的答案不一致，勾选后用户在查找栏得不到一致行为。

- vendor/CUI：`cui__ed_hit` / `cui__ed_find` 增加 match_case 参数
  （命中时 memcmp 直比；默认 false 保持历史的不区分行为）。匹配跳转、
  计数徽标（"3 of 7"）、正文高亮共用同一参数——三种呈现不会分叉。
- 新 API `cui_code_editor_find_case(ui, id, query, match_case)`；旧的
  `cui_code_editor_find` 保持兼容（等价 find_case(false)）。
- app 层 `np_open_find` 改传 `a->match_case`：查找栏与替换对话框、
  在文件中查找、np_replace_all 全部共用同一开关、同一配置键。
- 回归新增第 6 项 `test_match`（提取 np_match 真实现，15 用例）：
  两种大小写模式、UTF-8 续字节不得被 ASCII 折叠误伤（0xC3 0x89 vs
  0xC3 0xA9 在 ci 模式必须不命中）、起点语义（np_match 是逐位置谓词，
  用例从匹配偏移起传）与终止边界。顺带踩坑：MSVC 十六进制转义贪心
  吞字符，"\xC3\x89a" 会被解析成 \x89a——用字符串拼接断开。

MSVC 构建通过；六项回归全绿（check_config + 5 组单测）。

## 查找栏 Aa / 全词切换按钮（第二十八批）

第二十七批把 match_case 贯通到查找栏后，遗留问题：用户在查找栏内
没有开关可按——只能去替换对话框勾选。本批给查找栏加两个即时切换
按钮，并把「全词匹配」作为新语义同时落进 app 层。

- vendor/CUI：查找栏加「Aa」（区分大小写）与「W」（全词）两个
  toggle 按钮（selected + accent 色），翻转后立即从选区重跑搜索，
  计数与高亮同步更新。`cui__ed_word_byte`（ASCII 字母数字下划线；
  UTF-8 续字节永不计入，非词查询在边界处自然不命中）。
- 新 API：`cui_code_editor_find_opts`（seed 两个开关）、
  `cui_code_editor_find_toggles`（读回开关状态——遵循「以持有真相
  的一方做查询」模式）。
- app 层：notepad 加 `find_word`（配置键 find_word，load/save 各一）；
  主循环每帧读回当前文档的开关状态，变化即写回 match_case/find_word
  并立即 np_config_save——查找栏与替换对话框互相镜像，永不分叉。
- np_replace_all / np_count_matches / np_ff_run 三处接入
  `np_word_edge`（word=false 恒真，调用方无脑 gate）；matches_key
  哈希掺入 find_word。替换对话框加「全词匹配」checkbox。
- test_match 增补 np_word_edge 用例（8 个）：两端边界、'-' 不算
  词字符、'_' 算、UTF-8 字节不算词字符、ww=false 恒真。
- 踩坑：提取式测试要求签名单行（签名折行会让 \s*\{ 匹配失败）；
  np_word_edge 引用 np_word_char，两者都要提取。

MSVC 构建通过；六项回归全绿（test_match 现含 23 用例）。

## 查找开关全面统一（第二十九批）

「在文件中查找」此前有独立且不持久化的 ff_case——重启即丢，且与其
他查找入口（match_case）语义分叉。统一后全部查找入口共用同一组
持久化开关：

- 删除 ff_case 字段；np_ff_scan_file 改用 a->match_case 并接入
  np_word_edge（a->find_word），目录递归扫描与编辑器查找行为一致。
- ff 对话框的「区分大小写」checkbox 直接绑 a->match_case，新增
  「全词匹配」checkbox——与替换对话框、查找栏 Aa/W 三入口完全
  镜像：任何一处改开关，其余两处立即生效并持久化。
- ff_query 保持独立（目录搜索的查询词不与当前文档查找串扰）。

MSVC 构建通过；六项回归全绿。

## Hex 字节序列查找（第三十批）

hex 视图此前只有「转到偏移」——Ctrl F 在 hex 模式下是对 code editor
的静默无效调用（hex 用 cui_hex_view 组件，不读 find bar 状态）。

- `np_hex_parse_bytes`：查询解析，两种模式——hex（"1F 2E CC" /
  "1F2ECC" / "0x…" / 逗号分隔，奇数位或非 hex 字符报错）与 ASCII
  （整串去首尾空白逐字节）。
- `np_hex_find_run(next)`：首字节 memchr 式门 + memcmp，两段扫描
  支持 wrap（next 从上次命中之后继续，绕回开头再找一次；全新查找
  不绕）。命中走 cui_hex_view_goto——vendor 的 goto 自带选中与
  滚动到视野（居中），vendor 层零改动。
- Ctrl F 在 hex 文档改为打开「查找字节」对话框（修掉静默无效）；
  F3 重复下一个；np_any_dialog 补 dlg_hexfind。
- 回归新增第 7 项 test_hexfind（提取 np_hex_parse_bytes，15 用例）：
  三种 hex 形式、小写、逗号、trim、奇数位/非法字符/空查询/超容量
  拒绝、ASCII 与 trim。
- 踩坑重申：提取式测试签名单行（np_hex_parse_bytes 初版折行，
  提取失败后改单行）。

MSVC 构建通过；七项回归全绿（check_config + 6 组单测）。

## Hex 查找命中计数（第三十一批）

np_hex_find_run 命中后补一遍全扫统计：toast 文案升级为
"2 of 7 · 0x1F2E 处命中 4 字节"——与文本查找栏的 "2 of 7" 口径
一致。首字节门 + memcmp 的单次 O(n) 扫描，240 MB 上限文件一次
约 0.2 s，只在命中时做，可接受。

MSVC 构建通过；七项回归全绿。

## 替换下一个（第三十二批）

「查找和替换」对话框此前只有「全部替换」——逐个确认的替换流程
缺失（EditPlus / VS Code 的标配）。

- `np_replace_next`：从光标处向后找第一个匹配（尊重 match_case /
  find_word，与全部替换同一谓词），命中即 np_splice 替换——splice
  写入编辑器 undo 历史，一次 Ctrl Z 回退一处替换；替换后光标落在
  插入文本之后，下一次调用自然找到再下一个。两段扫描支持 wrap
  到文首。
- 对话框 footer 在「查找」与「全部替换」之间加「替换」按钮
  （ghost，查询为空时禁用）；找不到匹配 toast 提示。
- 定义位置注意：np_splice 的前向声明在 np_replace_all 之后，
  np_replace_next 必须放在声明之后（否则 implicit declaration）。

MSVC 构建通过；七项回归全绿。

## Tab↔空格互转与合并行（第三十三批）

转换菜单补齐 EditPlus 两个标配（此前只有大小写/排序/去重/删空行/
剪裁/括号跳转）：

- `np_row_tab2sp`：tab 展开到下一个 4 列制表位。列按字节计，与
  编辑器 cui__ed_col 语义一致（CJK 一个汉字占 3 列）。
- `np_row_sp2tab`：仅行首空白压缩，空格凑满一个制表位即得 tab；
  遇已存在的 tab 时吸收其前的不足一个 stop 的空格串（最终对齐
  相同），末尾不足一个 stop 的余量还原为空格。行身（非行首）的
  空格一律不动。
- `TR_JOIN`（合并选中的行）：去每行首尾空白后用单空格合并，空行
  丢弃；选区末尾若带换行则结果保留行尾 \n。需选区（与排序/去重
  同门槛）。sep 泛化后注意边界：行间用空格、末行 had_nl 用 \n。
- np_line_transform 的行指针仍指向 work，tab 转换可能变长，故
  写入 scratch 缓冲后替换指针（rows 数组按新指针重排）。
- 回归新增第 8 项 test_transform（提取两个纯函数，18 用例）。
  出题时自己也错了一次："\t    \t" 实际进位 4+4+4=12 列 =
  3 个 tab，不是 2 个——用例期望值要以进位推导为准。
- 踩坑：cui_menu_item_opts 没有 tooltip 成员；测试 .impl 必须
  在调用点之前 include（C2039/C2371 经典顺序错误）。

MSVC 构建通过；八项回归全绿。

## 复制行与移动行（第三十四批）

VS Code / EditPlus 的行级编辑标配，此前缺失：

- `np_duplicate_line`（Ctrl D / 菜单「编辑」）：把光标所在行或选区
  覆盖的整行复制一份到下方，走 np_splice 一步可撤销；末行无换行时
  以 "\n+内容" 插入（不改变文件是否以换行结尾的事实）；复制后
  光标落在副本行首。
- `np_move_lines`（Alt Up / Alt Down / 菜单）：整行块与相邻行块
  交换，np_splice 单步 undo；选区/光标形状保持并随块平移
  （caret/anchor 在块内的偏移 + delta 后重设 select）。首行上移、
  末行下移为静默 no-op。仅编辑器聚焦且非 hex 时响应。
- 菜单「编辑」新增三项（复制当前行 / 上移 / 下移选中行），F1 帮助
  同步更新。

MSVC 构建通过；八项回归全绿。

## 缩进参考线（P1 #1）

UI_PLAN P1 第一项落地：

- vendor：`cui_code_editor_opts.no_indent_guides`（默认开，opt-out）。
  行循环里按行扫描行首空白（空格 +1 列、Tab 进到下一 stop），每个被
  覆盖的 stop 画 1px 竖线，最内层贴着首个内容字符左缘；光标行且编辑
  器聚焦时整行参考线用强调色（guide_on）。列语义为字节列，与
  cui__ed_span / Tab 展开一致，中文混排不错位。
- 配色：guide = fg 低 alpha（深 0.11 / 浅 0.08），guide_on = accent
  alpha 0.45/0.38——随 9 风格预设与深浅自动适配。
- app：配置键 indent_guides（load/save 配对，check_config 验证），
  默认开；视图菜单「缩进参考线」开关（hex 下禁用——hex 视图无此概念）。
- 位置注意：参考线画在选区之后、marks / find 高亮 / 正文之前——
  垫底不被正文遮挡，选区下依然可见。

MSVC 构建通过（vendor 全量重编）；八项回归全绿。

UI_PLAN P1 第二项落地（空白字符显示，三档）：

- vendor：`cui_code_editor_opts.show_whitespace`——0 关 / 1 仅缩进 /
  2 全部（默认 0，app 按配置传入）。`cui__ed_paint_ws()` 在行循环里
  与正文同帧绘制：空格画中点（U+00B7，格内水平居中）、Tab 画箭头
  （U+2192，单元格左缘）；行尾空白（最后一个非空白字节之后，含纯
  空白行整体）用暖色 ws_end（oklch 0.62/0.16/35，alpha 0.45/0.40）
  起到「行尾标记」作用；\r 永不显示（CRLF 缓冲安全）。
- 坐标系教训（调试实锤）：minx/maxx 是「行内偏移」窗口（cui__ed_run
  的 `x < maxx` 不含 x0），第一版误用 `cx = x0 + x` 去比较，导致
  x > maxx-x0 的所有符号被静默裁掉——行尾空白与远端 Tab 全消失而
  行首符号正常。**偏移量窗口必须用偏移量比较。** x 推进逻辑与
  cui__ed_span 完全一致（Tab snap + 分段 text_width），符号与正文
  同格对齐。
- app：配置键 whitespace（load/save 配对，0-2 夹取），默认 1（仅
  缩进）；视图菜单三态循环项「空白字符：关/仅缩进/全部」（hex 下
  禁用）。绘制顺序：缩进参考线之后、marks/find 高亮之前。

MSVC 构建通过；八项回归全绿。视觉验证受限于 RDP 会话断开（DWM
停合成、窗口抓取全白），以插桩输出 + 已验证符号集合外推确认。

UI_PLAN P1 第三、五项落地（行号区打磨 + 选区/高亮层级）：

- 行号降饱和（vendor，VS Code 行为）：p->num / p->num_on 随既有的
  st->focus_t 缓动（边框用同一个 tween，更新时序在 build_lines 之前，
  每帧有效）。未聚焦：num 0.7→0.45 alpha；num_on 只在聚焦态用到，
  过渡期 0.5→1 平滑衔接。聚焦后恢复原亮度，当前行号仍为
  TEXT_SECONDARY。
- 层级重排（vendor）：每行绘制顺序从「选区 → 参考线 → 空白 → marks →
  查找高亮」改为「marks → 查找高亮 → 选区 → 参考线 → 空白」——
  半透明选区覆盖查找高亮与 marks（VS Code 式「选区压高亮」），
  marks 与查找高亮的相对关系保持不变（高亮仍压 marks）。
- 选区圆角逐行修正：cui_corners 逐角半径——仅选区起止行圆角
  （起行 top 2px、止行 bottom 2px、单行四角），多行选区中间行平角
  衔接，消除原来逐行 CUI_CORNERS(2) 的锯齿缺口。跨行起止判定：
  top = s0 > als || als == 0；bot = s1 <= ale + 1（含选中换行的情形）。

MSVC 构建通过；八项回归全绿。RDP 仍断开，视觉验证受限（同上批）。

UI_PLAN P1 第四项落地（语法配色审查，P1 全部完成）：

- 新增 scripts/audit_contrast.c：离线审查工具，进程内对 9 套风格
  预设逐一套 cui__theme_build_ex 构建主题、按 cui_editor.c 同款公式
  推导编辑器表面色，再调 cui__syntax_colors_ex 取真实 token 色，
  计算 WCAG 对比度（阈值 4.5:1）与 token 间 OKLab 距离（阈值 0.07，
  number/const 同色是设计意图、排除）。退出码 = 违规数，可挂 CI。
- cui__syntax_colors 重构出 ui 无关的 _ex 版本（单一事实来源，审查
  工具与运行时永不漂移）；ui 版变成一行包装。
- 审查发现并修复（浅色 5 套风格各 3 项不达标，深色全部通过）：
  - number/const：oklch 0.58→0.55 L（4.16-4.22 → 4.75-4.88:1）
  - comment：浅色从沿用 TEXT_MUTED（4.19-4.22:1）改为专属灰蓝
    oklch(0.53, 0.06, 255) = #546e8e（4.82-4.88:1）；与 punct 的
    OKLab 距离 0.042→0.08+，可辨性同步解决。深色 comment 仍用
    TEXT_MUTED（5.38-5.42:1 达标）。
- 终态：9 套风格 × 10 种 token 0 failures、0 too-close pairs。
  密度三档只改字号/行距，调色板同一套，可辨性结论直接适用。
- 工具构建：cl /MD scripts/audit_contrast.c /I vendor/CUI/include
  /I vendor/CUI/src /link cui.lib + win32 系统库（cui.lib 静态库按
  /MD CRT 构建，缺 /MD 会报 __imp_cbrtf 未解析）。

UI_PLAN P2 第七、八项落地（文件类型图标着色 + 标签页语言图标）：

- app 单一映射（np_file_family / np_lang_family / np_doc_family）：
  12 个粗粒度家族（C/C++、Web、Python、Rust/Go、JSON/SQL、Shell/
  配置、Markdown、TeX、HEX、纯文本）；图标 NP_FK_HEX 用 CPU、
  文本/MD 用 FILE_TEXT、其余 CODE；着色复用主题分类色板
  SERIES_1..6（与编辑器 marks 同族，所有风格深浅两态均可辨），
  MD 用 FOREGROUND（文档读起来「亮」），HEX 用 55% 前景灰，
  纯文本不着色走默认。
- vendor 三个组件加 icon_color（alpha 0 = 行为不变，默认渲染不破坏
  其他宿主）：cui_list_item_opts、cui_doc_tab、cui_menu_item_opts。
- app 接入五处：侧栏目录列表（目录保持文件夹图标不着色）、侧栏
  文档列表、文档标签、面包屑（无着色时退 TEXT_MUTED）、最近打开
  菜单。状态栏 chip 已有 tone 语义，未动。
- 标签溢出审查（#8 要求）：cui_doc_tabs 本就走横向滚动条 + 标题
  max_w 200 截断 + 拖拽重排，行为合格，无需改动。

MSVC 构建通过；八项回归全绿。注意：CUI_C 是 vendor 内部宏，app 层
读具体主题色用 cui_theme_get(ui)->colors[...]。

UI_PLAN P2 第六项落地（侧栏目录树，P2 主体完成）：

- 「目录」tab 从平面列表 + 「.. 上级」改为 cui_tree 递归树。固定
  节点池 NP_TREE_MAX 3072（约 245KB 名字块），子目录懒加载——只在
  展开时 cui_fs_list 一次（400 上限，复用 np_dir_cmp 排序）；空闲帧
  只画开路径上的节点，零扫描成本。
- 展开态按「相对根的路径」记忆（np_open_set，160 条上限），根变化
  时清空；重建（刷新/换目录/文件操作）后从集合恢复，无需重扫。
- 当前文件自动展开定位（reveal）：按路径逐级确保已加载并展开，
  选中该文件。触发点两处：tab 切换（follow_active 时还会把根重定向
  到文件目录）与启动（此前 last_active 在就绪块被同步、follow 块
  永不触发——启动也要显式 reveal）。
- 右键菜单绑定在树行上（cui_context_menu 紧跟 cui_tree_begin，
  last_id 即行）：新建文件/新建文件夹/重命名（对话框复用
  cui_dialog + cui_input，Enter 提交）/在系统中打开/打开所在文件夹。
  新建与重命名走 _wmkdir/_wrename/_wfopen（UTF-8 全程安全）；重命名
  后同步更新持有旧路径的打开文档。
- vendor：cui_tree_opts 加 icon_color（alpha 0 默认），树文件图标与
  #7 家族着色一致。
- 路径语义修复（调试实锤）：np_tree_root 此前从未赋值——np_tree_path
  返回空串，cui_fs_list("") 列出进程 CWD；且文档路径按原样存相对
  路径（命令行 "src\main.c"），follow/reveal 全部错位。两修：
  np_tree_build 显式落地 np_tree_root；np_open_path 入口 np_abs_path
  绝对化（文档路径从此全绝对）。
- 顺手修面包屑 "###crumb" 后缀泄漏（cui_text 不剥 ###id）。
- 验证：MSVC 构建 + 八项回归全绿；RDP 恢复后截图实证——树形层级、
  懒加载展开、follow 重定向 + reveal 选中高亮、家族图标着色全部
  正常。右键菜单代码审查通过（与编辑器菜单同机制），屏幕抓取随后
  在环境层失效未能截图。

## UI_PLAN P2 #9：输出面板打磨（空态统一 + 消息徽标）

- 「消息」tab 补计数徽标（与查找结果/文件搜索同款 CUI_TONE_INFO badge）。
- 输出面板四处空态 + 侧栏目录树空态全部换用 vendor cui_empty_state：
  查找结果未设词（含主行动「开始查找 Ctrl+F」→ np_open_find）、
  查找无匹配、文件搜索未设词（行动「打开文件搜索 Ctrl+Shift+F」→
  dlg_find_files）、文件搜索无匹配（AI 语义失败态保留自定义错误样式）、
  消息空、目录树空。
- vendor cui_empty_opts 兼容扩展三个字段（memset 0 默认，不破坏宿主）：
  action_icon（主行动按钮图标，默认 plus）、fill（占满高度并居中）、
  compact（紧凑变体：图标 14px 与标题同行、text_body/text_sm、
  pad 16/12、SM 按钮）。
- 紧凑变体是被截图逼出来的：输出面板内容区 ~160px，标准空态
  （pad 40 + 28px icon + text_lg 标题 + 按钮）超高被裁；先压缩间距仍
  裁 ~20px，图标与标题同行后 ~90px 完整放下。
- 视觉验证改走 CUI_SCREENSHOT 自绘链路（CUI_SCREENSHOT=path +
  CUI_SCREENSHOT_FRAMES=90，程序渲染 N 帧后自行存 PNG 退出），
  彻底绕开窗口存活问题；out_tab 用临时 main 初始化值切换三个 tab，
  验完恢复。三个空态 + 消息徽标「7」截图实证通过。
- 插曲：本机存在间歇性外部 WM_CLOSE（~40% 概率，20 秒内）杀死 GUI
  子进程——trace 实锤非程序内部路径（无 quit 调用、无崩溃、退出码 0），
  基线同样暴露；Visualizer 截图链路不可靠时 CUI_SCREENSHOT 是稳定
  替代。g.out_tab 默认值 1（消息 tab）在 main 初始化块，勿当 bug 排查。
- 验证：MSVC 构建 + 七项回归全绿。

## UI_PLAN P3：命令面板与键盘流（#10 #11 #12）

- 命令面板（Ctrl+Shift+P）启用 vendor cui_command_palette：静态命令表
  np_cmds（63 条，enum 索引即分发契约）+ 每帧重建的最近文件条目（组名
  「最近文件」，标题「打开 xxx」）；动态标题（界面密度/空白字符档位）
  每帧覆写。np_cmd_run 单点分发，文本模式专属命令内部 guard is_hex。
- 改键：Ctrl+Shift+P → 命令面板；预览面板 → Ctrl+Alt+P（菜单、工具栏
  tooltip、状态栏 tooltip、帮助对话框四处文案同步）。
- F1 → 同一面板（#11）：快捷键列 + fuzzy 搜索替代静态帮助列表；「快捷键
  速查」对话框保留在帮助菜单与工具栏按钮。
- vendor cui_palette_opts 兼容扩展（ex 变体，旧签名 wrapper 保 ABI）：
  placeholder/hint_nav/hint_select/hint_close/no_results 五条本地化，
  app 全部中文化。
- #12 沉浸模式缓动：cui_animate_spring(id, zen?0:1, 0.15s, bounce 0) 驱动
  chrome（菜单+工具栏列）与状态栏的 max_height 折叠 + clip；自然高度用
  cui_get_rect(.id 显式命名) 在自由渲染帧测量（cui_last_rect 不可靠——
  容器不更新 last_id）。折叠完成后（vis≤0.01）整块跳过渲染。
- 视觉验证走 CUI_SCREENSHOT 自绘链路：面板布局、中文占位符、分组标题、
  图标+快捷键行、中文 footer、消息徽标全部截图实证；临时 palette_open
  初始值验后清除。
- 教训：嵌套 scope 改造（状态栏外包 cui_box）要数清 opens/closes——
  少一个收括号导致 np_frame 吞掉后续函数，C1075 报在函数开头，用
  python 花括号深度扫描逐行定位（3 分钟）比肉眼快。
- 验证：MSVC 构建 + 七项回归全绿。

## UI_PLAN P4：颜色 token 清查（#13）与滚动条命中刻度（#16）

- #13 清查结论：app 层零改动达标。全部颜色走 token（CUI_COLOR_*、
  CUI_TONE_*、theme->colors、SERIES_*）；raw 色仅三处功能性使用——
  QR 码黑白（扫描语义）、图片绘制白色 tint（无调制）、HEX 图标
  foreground 0.55 alpha（随主题）。vendor 层唯一例外是取色器预设色板
  （展示可选颜色本身，属功能定义）。清查方法：grep CUI_RGB/cui_rgba/
  cui_oklch/0xRRGGBB 定长模式。
- #16 滚动条命中刻度，两条数据通路：
  - 文本编辑器：st->query（cui_code_editor_find）全文档扫描建刻度缓存
    （st->tk_line 持久 owned 数组，dtor 释放；query/case/word/rev/len 任一
    变化重扫；扫描上限 2MB、刻度上限 1024）。cui__edf_ticks 在 build_lines
    调用（E 全量行索引可用），paint 在 thumb 之下画 4x2px 刻度。
  - hex 视图：np_hex_find_run 成功时顺带收集全部命中（cap 400，记
    hexfind_doc_uid 防跨文档错位）合并进 cui_hex_view 的 ranges（tone
    INFO）；vendor hxf_visible 从 ranges 全量构建 cui_scroll_tick
    （at=row/rows，cap 4096），hex 自绘滚动条段画刻度。
  - 设计决定：刻度常驻（不乘 bar_t）——覆盖式滚动条平时隐藏，若刻度
    跟随淡出，用户滚动前看不到命中分布，功能价值尽失；透明度 0.6~0.8
    保持安静。
  - 共享滚动条（cui__paint_scrollbars）也加了 ticks 支持（cui_scroll_state
    新增 ticks/nticks，cui_scroll_tick{at,c}），供通用滚动容器未来使用；
    hex 走自己的绘制路径。
- 视觉验证（CUI_SCREENSHOT）：编辑器刻度（query=int，1024 个，密集连片
  符合预期）+ hex 刻度（"This program" 2 命中，INFO 蓝清晰可辨）截图实证。
- 教训：hex 与编辑器一样自绘滚动条——cui__paint_scrollbars 只服务走
  CUI_NF_SCROLL 的通用容器；给「自绘滚动条的组件」加刻度必须进它自己的
  绘制段。验证时的临时注入要在 a->ui 赋值之后（先崩过一次空指针），
  且文档存在后才触发（首帧 np_cur 为 NULL）。
- 验证：MSVC 构建 + 七项回归全绿。

## UI_PLAN P4：统一空态样式（#14）与动效一致性（#15）

- #14 AI 面板空态：idle 无结果时由 4 行纯文本说明改为 cui_empty_state
  （compact，SPARKLES 图标 +「还没有结果」+ 一句描述 +「Ask AI」主行动
  按钮）；空态接管主行动后，原按钮行在 idle 态整行隐藏，避免两个
  「Ask AI」并存（st==0/2 且无结果时 ai_idle 判定）。任务说明 4 行保留
  在空态下方。顺手把两个对话框内小空态换统一组件：自定义 AI 动作
  （SLIDERS_HORIZONTAL 图标，无 action——下方表单即主行动）、LLM 服务商
  （CLOUD 图标）。至此空态点位全覆盖：目录树/查找结果×2/文件搜索/
  消息/AI 面板/动作对话框/服务商对话框。
- #15 动效一致性清点结论：vendor 已有集中 token 体系（cui_internal.h，
  「One value per job」），CUI__T_HIGHLIGHT/HOVER/FOCUS/STATE 与
  SPRING_SMOOTH/SNAPPY/MORPH/EXPAND/ENTER/DIALOG/EXIT 覆盖绝大多数
  调用点；漏网四处归一：
  - tooltip（0.2/0.12 硬编码）→ CUI__SPRING_ENTER / CUI__SPRING_EXIT
    （token 注释本就声明 tooltip 归 ENTER）；
  - FAB 弹出菜单（open 0.42+bounce 0.18 / close 0.24，close 反而比
    open 慢，违反「exit 快于 enter」体系）→ CUI__SPRING_EXPAND /
    CUI__SPRING_EXIT；
  - sheet（0.42 硬编码）→ 新 token CUI__SPRING_SHEET 0.42f,0（值不变，
    提为 token 并注明行程长所以慢）；
  - toast 进出场（0.38/0.22）→ 新 token CUI__SPRING_TOAST 0.34f,0 进、
    CUI__SPRING_EXIT 退。
  其余散落 spring 属组件个性物理（tab 指示器拉伸、滚动条、counter
  pop、hold-button arm）或图表入场/进度类，有注释设计意图，不属
  弹层/折叠范畴，保留。app 层仅 #12 两处 0.15s spring（计划值）。
  ⚠️ spring 宏含逗号（"0.42f, 0.0f"），只能作为实参列表尾部展开，
  不能嵌进三目/表达式——分支动画一律写 if/else 两行。
- 截图实证（CUI_SCREENSHOT 链路）：AI 面板空态完整渲染（图标+标题+
  描述+绿色 Ask AI 主行动），无重复按钮行。临时初始化值注意：show_ai
  在 config 中有持久化键，临时值必须放 np_config_load 之后（放前面
  会被 config 覆盖，第一次截图白跑）。
- 验证：MSVC 构建 + 七项回归全绿。UI_PLAN #14/#15 标 ✅，P4 仅剩
  #17 连字评估（L，评估类）。

## 第三十二批（P4 #17 连字评估 + 实验开关，vendor L 评估类）

- 交付：CUI_TEXT_SHAPING 编译开关（CMake option，默认 OFF，顶层 CMakeLists
  强制 ON 供本仓构建）+ 视图菜单「编程连字（实验）」勾选（editor_lig
  持久化，默认关）+ cui_code_editor_opts.ligatures opt-in。
- shaper 本体正确性回归（scripts/test_lig.c，run_tests 第八项）：内置
  JetBrains Mono / Cascadia Code 两款字体的 -> => != === && abc 序列
  glyph id 与总 advance 与 HarfBuzz（uharfbuzz 0.57）逐一对照断言，
  十二项全 ok。两款字体的连字规则全部只在 calt（type 6 chained
  context），JB 走 1:1 状态替换（SPC 占位 + lig glyph），Cascadia 走
  .seq 渐进替换（2 glyph）——shaper 的 context fmt1/2/3 路径均正确。
- 排查「代码区画不出连字」根因（探针法定位）：
  - cui__tokenize 对标点逐字节发 token（EMIT(i,1,TK_PUNCT)），绘制段
    每个 run 只见 1 字节，needs_shaping 的连字扫描（要求 len>=2）永不
    触发；注释/字符串是整段 token 所以能 shape——与探针记录完全吻合。
  - 修复：绘制循环里相邻同类 TK_PUNCT token 合并成一次 run（仅
    p->lig 开启时，关闭零影响）。-> => != === <=> && || << >> :: 等
    经典序列都是纯标点连续段，合并后正好成对进 shaper。
  - TK_* 枚举从 cui_code.c 提升到 cui_internal.h（原有
    TK_COUNT==CUI__TK_COUNT 静态断言保序）。
- 截图实证（Cascadia Code + 开关开）：!= → ≠、=== → ≡、!== → ≢、
  <= → ≤、>= → ≥、-> → →、=> → ⇒、<< → ≪、>> → ≫、<=> → ⇔、&&/||/::
  全部连体；开关关逐字符独立，无回归。连字 glyph 总宽 = 字符宽度和
  （JB 1200/2cell、Cascadia 2400/2cell），caret/命中测试网格不变。
- 剩余缺口（后续可选）：整行 shape + cluster 定位绘制（L 级）才能
  覆盖跨 token 上下文（如字符串边界处的序列、跨空白序列）以及非
  内置字体；当前逐 token + 标点合并已覆盖内置字体全部常用连字。
- 教训：调试探针的 cap（pd<6/12）会先被高频调用点吃满，导致 paint
  阶段命中被静默漏记——排查绘制问题时先放大 cap 或按状态过滤。
- 验证：MSVC 构建 + 八项回归全绿（原七项 + test_lig）。UI_PLAN #17
  标 ✅，P4 全部完成。

## 第三十三批（UI_PLAN 全量验收，四项全过）

计划 17 项全部 ✅ 后，把 P1–P4 各阶段写明的验收标准逐项实测（2026-11-25）：

- P1 三极端场景缩进线：plain 文件（无语法着色）、紧凑密度 + 10pt 最小字号、
  中文混排（acc_cjk.c 全角标点 + 五层 tab 缩进），截图放大逐层核对——参考线
  与 tab stop 左缘对齐、与 tab 箭头/文本起始列一致、不穿字符。场景用 plain
  模拟「关闭语法高亮」（产品无该开关，tokenize 的 L->plain 分支即此状态）。
- P2 目录树性能 + 定位：构造 acc_bigtree（根 350 文件 + 30 子目录 × 90 文件 +
  sub_07/d01..d05 深链 + target.c，共 3051 文件）。启动带深链目标文件，耗时
  与小目录基线差 ≤40ms（噪声内）——np_tree_load 每目录仅扫一层、reveal 只
  装 6 个祖先目录。无闪烁验证：CUI_SCREENSHOT_FRAMES=1 vs 20 两帧树区域
  ImageChops 像素级相同，第 1 帧即最终状态（展开链 + 选中高亮 + 滚动定位）。
- P3 纯键盘流：真实窗口 + keybd_event/SendInput + PrintWindow（新工具
  scripts/acc_keys.py）。Ctrl Shift P 开面板 ✓；KEYEVENTF_UNICODE 注入
  「排序」精确过滤出「排序选中的行 (A→Z)」✓；回车后面板关闭 ✓。执行效果
  用「显示行号」开关命令验证（回车后行号列消失，状态切换可见）。
  ⚠️ 两个坑：① SendInput 的 INPUT 结构 union 必须含 MOUSEINPUT 凑满 40 字节，
  否则 cbSize 校验静默失败（返回 0）——第一轮输入没生效就是这个；
  ② 中文经 VK_PACKET → TranslateMessage → WM_CHAR 通路，CUI 的代理对
  处理正常接收。「排序」执行的 toast 提示未抓到（0.45s/1s 两拍都没有），
  不确定是时机还是 toast 渲染范围问题——命令本身已确认执行，留观。
- P4 视觉清单抽 4 组合：极光蓝/浅/舒适、苔绿/浅/宽松、深海/深/紧凑、
  炭火/深/宽松。临时 env 开关（NP_ACC_STYLE/NP_ACC_DENSITY，已删）一次
  构建跑四组：token 可辨、控件无错位重叠、中文正常。
- 产物：验收 demo 文件 build-msvc/acc_plain.txt、acc_cjk.c 保留（build-msvc
  不入库）；acc_keys.py 入库备用；大目录与全部截图已清理。
- 验证：构建 + 八项回归全绿。UI_PLAN 追加「验收记录」一节。

## 第三十四批（第三十三批留观项排查：toast「未出现」）

- 结论：**toast 功能完全正常，之前的失败是测量伪影**。留观项关闭。
- 证据链（探针 + 三种捕获手段交叉验证）：
  1) cui_toast_ex / cui__toasts_render 探针：面板路径执行「排序」确实创建
     toast（CREATE @t=6.83s），渲染正常，4.16s 后过期移除（TTL 4s ✓）；
  2) 真实窗口 GL framebuffer dump：toast 完整在画面里（右下角，探针样式）；
  3) 桌面 BitBlt（GetDC(0)，DPI-aware 物理像素）：toast 存活期 vs 过期后的
     差异 bbox 恰为右下角 toast 区域，卡片「先选中要处理的行 / 合并与排序、
     去重一样作用于选中的行」肉眼可见——面板命令路径在真实屏幕上工作正常。
- 根因：**PrintWindow（含 PW_RENDERFULLCONTENT）对本应用的 GL 窗口返回陈旧
  内容**——不仅 toast 抓不到，部分抓拍连状态栏都缺。此前 acc_keys.py 用它
  做 P3 验收截图，才产生了「toast 未出现」的假象。桌面抓屏是可信手段。
- 工具：scripts/acc_keys.py 的 capture 已改桌面 BitBlt + SetProcessDpiAware
  nessContext(PMv2)（物理像素对齐）；一次性诊断脚本 toast_probe.py 已删。
- 排查中顺带确认：cui__plat_present_part 在 win32 恒走全量 blit（damage 不
  是问题）；app 主循环按需渲染与 wakeup 均正常；按需渲染下 SIZES 类探针每
  60 帧才打一行，采样时要注意帧数稀疏。
- 验证：源码探针全部移除后 git diff 仅剩 acc_keys.py 与文档；构建 + 八项
  回归全绿。UI_PLAN 验收记录已加追记。

## 第三十五批（Android/平板自动横屏）

- 需求：手机/平板上自动横屏。此前 app 在系统关闭自动旋转时卡在竖屏
  （竖屏逻辑宽度低于 min_width 620，编辑器基本不可用）。
- 根因：SDL 在 Android 创建/切换窗口时调 Android_JNI_SetOrientation，
  未设 SDL_HINT_ORIENTATIONS 且窗口 RESIZABLE → 运行时
  setRequestedOrientation(FULL_USER)，覆盖 manifest 默认朝向；
  FULL_USER 服从系统旋转锁。
- 修复（双保险）：
  1) cui_app_sdl2.c（__ANDROID__ 分支）SDL_SetHint(SDL_HINT_ORIENTATIONS,
     "LandscapeLeft LandscapeRight") → SENSOR_LANDSCAPE，跟随传感器、
     横屏两方向均可，无视系统锁；
  2) AndroidManifest activity 加 android:screenOrientation="sensorLandscape"
     作基线；resizeableActivity=true 使 DeX/分屏/PC 模式忽略它，不受影响。
- 桌面不受影响：cui_app_sdl2.c 仅 CUI_PLATFORM=sdl2（Android）编译，
  Windows 走 win32 平台层。
