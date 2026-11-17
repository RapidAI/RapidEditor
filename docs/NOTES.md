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
