# Windows 打包（NSIS）

## 产物

| 文件 | 内容 | 适用机器 |
| --- | --- | --- |
| `RapidEditor-<ver>-windows-x64-setup.exe` | 仅 x64 | x64 Windows |
| `RapidEditor-<ver>-windows-arm64-setup.exe` | 仅 ARM64 | ARM64 Windows |
| `RapidEditor-<ver>-windows-universal-setup.exe` | 同时包含两种二进制 | 自动判断，推荐 |

`installer.nsi` 的 `.onInit` 用 NSIS 的 `IsNativeARM64` / `RunningX64` 检测本机架构：
通用安装包会把匹配的二进制改名为 `RapidEditor.exe`，单一架构包装错机器时直接给出提示并退出，不会留下半安装状态。

## 右键菜单与文件关联

安装时写入（卸载时全部删除）：

```
HKCR\*\shell\RapidEditor                        (默认) = 用 RapidEditor 打开
HKCR\txtfile\shell\RapidEditor                  ...
HKCR\SystemFileAssociations\.txt\shell\RapidEditor     ...   （每个常用文本类型一条）
HKCR\SystemFileAssociations\.md\shell\RapidEditor      ...
HKCR\SystemFileAssociations\.json\shell\RapidEditor    ...
... （共 89 种扩展名）
HKCR\Applications\RapidEditor.exe               FriendlyAppName / shell\open\command
```

每条右键项都包含：

```
(默认) = 用 RapidEditor 打开
Icon    = <安装目录>\RapidEditor.exe,0
command = "<安装目录>\RapidEditor.exe" "%1"
```

- `*` 覆盖所有文件，`txtfile` 覆盖文本文档类。
- `SystemFileAssociations\<扩展名>` 为常用文本/配置/代码文件逐个加上右键项，
  覆盖 txt、log、md、json、yaml、toml、ini、csv、html、css、js、ts、c/c++、
  py、go、rs、java、sh、bat、sql、diff 等 89 种扩展名；完整列表见 `installer.nsi`
  里的 `TEXT_EXTS`。
- `Applications\RapidEditor.exe` 让 RapidEditor 出现在 Windows“打开方式”列表中。

这些都**不会修改默认打开方式和文件图标**。扩展名列表只维护在 `TEXT_EXTS` 一处，
安装和卸载共用同一个遍历函数。主程序用 `CommandLineToArgvW` 读取宽字符命令行，
中文路径也能正确打开。

## 本地构建

需要 CMake ≥ 3.16、Ninja、MinGW-w64/MSVC，以及 [NSIS 3.x](https://nsis.sourceforge.io/)（把 `makensis.exe` 加入 PATH）。

```bat
packaging\windows\build_installer.bat 1.0.0
```

脚本按 `PROCESSOR_ARCHITECTURE` 自动判定 x64 / ARM64（可用 `set RE_ARCH=x64` 覆盖），
编译、暂存载荷并输出到 `dist\`。CI 里改用 `make_installers.bat <版本> <x64 exe> <arm64 exe>`，
一次产出上面三个安装包；两份载荷都由 llvm-mingw 编出，分别使用
`llvm-mingw-x86_64.cmake` 与 `llvm-mingw-aarch64.cmake` 工具链文件，不依赖 Visual Studio。

> `installer.nsi` 必须是 **带 BOM 的 UTF-8**：NSIS 3 默认按系统 ANSI 代码页读脚本，
> 没有 BOM 时中文菜单文字会报 `Bad text encoding`。

PowerShell 下也可以直接调用 NSIS：

```powershell
makensis "-DPRODUCT_VERSION=1.0.0" "-DOUTFILE=$PWD\dist\setup.exe" `
         "-DPAYLOAD_X64=$PWD\build-win-x64\RapidEditor.exe" `
         packaging\windows\installer.nsi
```
