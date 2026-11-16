# macOS 打包（.pkg）

```sh
packaging/macos/build_pkg.sh 1.0.0              # 默认：universal（x86_64 + arm64）
packaging/macos/build_pkg.sh 1.0.0 auto        # 只编译本机架构
packaging/macos/build_pkg.sh 1.0.0 arm64
```

产物：`dist/RapidEditor-<版本>-macos-universal.pkg`。

## 系统版本要求（macOS 15+）

最低系统版本与构建 runner 都锁定在 **macOS 15**：

- **runner**：CI 用 `runs-on: macos-15`（固定标签，不会降级到 14/13 或随 `macos-latest` 漂移）。
- **最低部署版本**：`build_pkg.sh` 显式设置
  `MACOSX_DEPLOYMENT_TARGET=15.0` / `-DCMAKE_OSX_DEPLOYMENT_TARGET=15.0`，
  因此生成的二进制与 `.pkg` 只能在 macOS 15 及以上安装运行。

两者都写死而不是依赖 runner SDK 默认值，保证 `.pkg` 的兼容性可复现、不会因 CI 镜像更新而改变。

## 架构自适应

`CMAKE_OSX_ARCHITECTURES="x86_64;arm64"` 一次编译出通用二进制（`lipo -archs` 可核对），
同一个 `.pkg` 在 Intel 与 Apple Silicon Mac 上都能原生运行，无需用户选择架构。
`auto` 模式由 `scripts/arch.sh` 读取 `uname -m` 自行判定。

## 右键菜单（两层保险）

1. **打开方式**：`Info.plist` 的 `CFBundleDocumentTypes` 声明 `public.data`、
   `public.text`、`public.source-code` 等 UTI 和常见扩展名，`LSHandlerRank=Alternate`。
   安装后 LaunchServices 注册 App，Finder 右键任意文件 → **打开方式 → RapidEditor**。
   不会抢走用户默认的打开程序。
2. **快速操作 / Quick Action**：pkg 额外安装
   `/Library/Services/Open with RapidEditor.workflow`，右键菜单的「快速操作 / 服务」里出现
   **用 RapidEditor 打开**。它执行 `open -a "RapidEditor" "$@"`，由 LaunchServices 投递
   `openFiles` Apple Event。

### 为什么需要改 CUI 的 Cocoa 后端

macOS 的「打开方式」是 Apple Event，不是命令行参数。上游 CUI 只处理了拖放，
所以本仓库的 `vendor/CUI` 打了第三个补丁：

- `cui_app_post_open_file()` / `cui_app_pending_file_count()` 等：app 层的全局待打开队列；
- `CUIPlat` 实现 `application:openFile:` / `application:openFiles:`，并在
  `finishLaunching` **之前**设置 delegate（否则启动时的 Apple Event 会被丢弃）；
- `src/rapideditor.c` 每帧排空该队列并调用 `np_open_path()`。

因此即使程序已经在运行，右键打开文件也会新开一个标签页，而不是只把窗口切到前台。

## 安装后的注册

pkg 的 `postinstall` 会 `lsregister -f /Applications/RapidEditor.app`、`plutil -lint` 校验，
并以当前登录用户身份写入 `pbs` 的 `NSServicesStatus` 可见性标志后 `pbs -flush`。
如果「快速操作」没有立刻出现，可在
**系统设置 → 键盘 → 键盘快捷键… → 服务** 里勾选「用 RapidEditor 打开」；
「打开方式」不受此影响。

## 未签名说明

CI 没有 Apple Developer 签名证书，pkg 是未签名的。首次安装时 macOS Gatekeeper 会拦截，
请右键 pkg → **打开**，或 `sudo xattr -d com.apple.quarantine <pkg>` 后再安装。
如需正式签名，在 CI 里加 `productsign --sign "Developer ID Installer: ..."` 即可。

## 卸载

```sh
sudo rm -rf /Applications/RapidEditor.app \
           "/Library/Services/Open with RapidEditor.workflow"
/System/Library/CoreServices/pbs -flush
```
