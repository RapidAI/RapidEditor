# Linux 打包（AppImage）

```sh
packaging/linux/build_appimage.sh 1.0.0
# -> dist/RapidEditor-1.0.0-linux-x86_64.AppImage  (或 -aarch64.AppImage)
```

依赖：`cmake ninja-build gcc libx11-dev libgl1-mesa-dev libxcursor-dev libxinerama-dev libxi-dev`，
以及 `curl`（用于下载 appimagetool）。OpenGL 是运行时 `dlopen("libGL.so.1")`，
不打进 AppImage；`bundle_libs.py` 会递归收集 libX11 及其依赖，同时排除 glibc、
动态加载器和显卡驱动库，避免污染宿主系统。

## 架构自适应

AppImage 的运行时本身是单一 ELF 架构，所以 CI 在 `ubuntu-24.04`（x86_64）和
`ubuntu-24.04-arm`（aarch64）上各产一个包。自适应由安装脚本完成：

```sh
curl -LO https://github.com/RapidAI/RapidEditor/releases/latest/download/install-linux.sh
chmod +x install-linux.sh && ./install-linux.sh
```

`install.sh` 读取 `uname -m`，优先使用旁边同目录匹配的 AppImage；否则通过 GitHub API
解析该架构的 release 资产并下载。也支持 `--file`、`--dir`、`--tag v1.0.0`。
安装位置全部在用户目录（`~/.local/bin`、`~/.local/share`），**不需要 root**。

## 右键菜单

1. `.desktop` 注册 `MimeType=text/plain;...`，所有桌面环境的
   **右键 → 打开方式 → RapidEditor** 都会出现；`install.sh` 把 `Exec` 写成实际 AppImage 路径。
   不修改 `xdg-mime default`，不抢默认程序。
2. 额外安装 `~/.local/share/file-manager/actions/rapideditor-open.desktop`，
   对支持该规范的文件管理器（Nemo / Caja / 装了 filemanager-actions 的 Nautilus）
   直接提供右键动作 **用 RapidEditor 打开**。GNOME 原生 Nautilus 若没有显示，
   第 1 条的「打开方式」仍然有效。

`uninstall.sh` 会移除上述所有文件并刷新 desktop database。

## 运行

```sh
chmod +x RapidEditor-1.0.0-linux-x86_64.AppImage
./RapidEditor-1.0.0-linux-x86_64.AppImage 文件1.txt 文件2.c
```

AppRun 设置 `LD_LIBRARY_PATH` 后 `exec` 真正的可执行文件，命令行参数（含中文路径）
原样传给主程序；右键菜单的 `%F` 也是走这条路径。
