# macOS 预览版第三方组件

- Qt 6.11.2 官方 macOS 二进制包：Core、Gui、Widgets、Sql、Multimedia、Concurrent 与相应插件，使用 `macdeployqt` 动态部署。Qt 开源许可证文本随应用放在本目录。来源：https://download.qt.io/archive/qt/6.11/6.11.2/
- FFmpeg 7.1.3：使用 Qt Multimedia 官方 macOS 包中动态链接的 FFmpeg 库，应用本身也链接相同主版本。Qt 声明在线安装器提供的 FFmpeg 二进制为 LGPL v2.1 或更新版本，未启用 GPL 组件。许可证文本随应用放在本目录。来源：https://doc.qt.io/qt-6/qtmultimedia-attribution-ffmpeg.html ；源代码：https://ffmpeg.org/releases/ffmpeg-7.1.3.tar.xz
- SQLite：通过 Qt SQLite 插件提供。来源：https://sqlite.org/copyright.html

应用不复制或上传用户音乐。发布给公司多人使用前，还需按实际部署包复核 Qt 内嵌第三方组件清单和许可证，并完成 Mac 实机验收。
