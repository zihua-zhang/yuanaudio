# macOS 预览版第三方组件

- Qt 6.11.2 官方 macOS 二进制包：Core、Gui、Widgets、Sql、Multimedia、Concurrent 与相应插件，使用 `macdeployqt` 动态部署。Qt 开源许可证文本随应用放在本目录。来源：https://download.qt.io/archive/qt/6.11/6.11.2/
- FFmpeg 7.1.3：使用 Qt Multimedia 官方 macOS 包中动态链接的 FFmpeg 库，应用本身也链接相同主版本。Qt 声明在线安装器提供的 FFmpeg 二进制为 LGPL v2.1 或更新版本，未启用 GPL 组件。许可证文本随应用放在本目录。来源：https://doc.qt.io/qt-6/qtmultimedia-attribution-ffmpeg.html ；源代码：https://ffmpeg.org/releases/ffmpeg-7.1.3.tar.xz
- SQLite：通过 Qt SQLite 插件提供。来源：https://sqlite.org/copyright.html

异常路径处理仅按用户启动的任务建立兼容入口或本机缓存，不上传用户音乐。

## libsmb2

0.12.4 静态链接 libsmb2 7.0.0（提交 `727feaaa6d0f4fed1bae57745fbc2c5db9a30e64`），用于保留原始 Unicode 名称的 SMB3 访问和兼容硬链接。上游 https://github.com/sahlberg/libsmb2 ，许可证 LGPL-2.1-or-later。构建禁用环境凭据文件、Kerberos/GSSAPI 和独立工具，仅使用本次连接的内存凭据。

APP 的 `Contents/Resources/Legal` 包含许可证、上游及本地补丁说明，以及 `Relink-Sources.tar.gz` 完整库源码和本应用重新链接输入。解压后安装同版 Qt 6.11.2 macOS arm64 SDK，使用 CMake/Ninja 构建：

```bash
cmake -S . -B build-relink -G Ninja -DCMAKE_PREFIX_PATH=/path/to/Qt/6.11.2/macos -DFFMPEG_INCLUDE_DIR=/path/to/ffmpeg-7.1.3 -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=27.0
cmake --build build-relink
```

FFmpeg 头文件校验和及下载来源见 `scripts/build-macos.sh`。可修改并重新链接此库；本预览不采用阻止替换库的签名限制。完整 Mac 构建交付包另保留 Qt 官方 Cocoa 补丁、插件重建与验收流程。重新分发时继续携带上述许可证和源码材料。
