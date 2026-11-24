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
