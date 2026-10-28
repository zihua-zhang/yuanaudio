# 第三方组件

本 Windows 预览包动态链接以下组件，没有修改相应库。应用源码和构建脚本位于本工程，可替换兼容动态库后重新构建。原音乐没有上传到在线服务。

- Qt 6.9.3：Core、Gui、Widgets、Sql、Multimedia、Network 及相应插件。来源为 Qt 官方发布仓库，经 aqtinstall 获取。Qt 开源许可证与组件归属见 `licenses/Qt-*` 和官方源码中的 LICENSES、qt_attribution.json。
  - https://download.qt.io/archive/qt/6.9/6.9.3/
  - https://code.qt.io/cgit/qt/qtbase.git/tree/?h=v6.9.3
  - https://code.qt.io/cgit/qt/qtmultimedia.git/tree/?h=v6.9.3
  - https://code.qt.io/cgit/qt/qtbase.git/tree/src/3rdparty?h=v6.9.3
- FFmpeg 7.1.1：由 Qt 媒体包提供，播放和波形均动态链接；开发头文件从同版本官方源代码提取并校验 SHA-256。通过所交付 avutil 动态库读取到的授权字符串为 `LGPL version 2.1 or later`。许可证见 `licenses/FFmpeg-LGPL-2.1.txt`；源码 https://ffmpeg.org/releases/ffmpeg-7.1.1.tar.xz。
- GCC / MinGW 13.1 运行时和 winpthreads：来自 Qt Tools MinGW 13.1.0 包，随附对应版权与许可文件。
- SQLite：通过 Qt SQLite 插件提供；源代码处于公有领域，https://sqlite.org/copyright.html。

FFmpeg 的本机库配置：

```
--prefix=/c/FFmpeg-n7.1.1/build/mingw/installed --disable-programs --disable-doc --disable-debug --enable-network --disable-lzma --enable-pic --disable-vulkan --disable-v4l2-m2m --disable-decoder=truemotion1 --enable-shared --disable-static
```

开发用 Python、aqtinstall、CMake、Ninja 和测试音频生成器没有包含在便携包中。正式向公司范围分发前还需完成包含 Qt 内嵌第三方组件在内的分发清单核验、签名和双平台验收；此包用于本机原型试用。
