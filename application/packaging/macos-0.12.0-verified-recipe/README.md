# 0.12.0 Mac 修复版构建记录

这里保存 2026-10-27 Mac 回传的原始构建脚本与 Qt 工程，供维护和重建使用。`application/` **只有构建脚本，不是完整应用源码**。本目录不是让用户双击的安装包。

已验收镜像和边界见 [修复核验记录](../../docs/mac-fix-validation-0.12.0.md)。回传原包归档为 `dist/mac-fixed-0.12.0/源音效管理工具-macOS27-0.12.0-Mac修复复测-回传.zip`。`returned-file-sha256.json` 对原样复制的材料逐项记录 SHA-256。

## 重建必须保留的布局

在 macOS 27 Apple Silicon 上创建全新的独立工作目录：

```text
repair/
  application/                 完整 0.12.0 应用源码，再覆盖本目录中的四个脚本
    scripts/
  qt-cocoa-project/             本目录提供的 CMakeLists.txt
  handoff/源音效管理工具-Mac闪退修复资料-0.12.0/qt-upstream/
                               本目录提供的补丁、NOTICE 与 licenses
  sources/                     由重建脚本下载并展开完整 Qt 源码
  qt-cocoa-build/               重建插件及其身份清单
  qt-native-tests/              原生回归
```

完整应用源码以原始构建包为基线（SHA-256 `76550bf393a5c906a670de27fb2069c96e2ebe8e8f667002e12eab7c0f3eb197`）。应用测试中的 `/var` 临时目录物理路径修复应沿用主工程已审核的 `tests/source_file_removal_tests.cpp`；不得改变原件删除的生产保护。原始包由 `dist/源音效管理工具-macOS27-同事构建包-0.12.0.zip` 保留。

先准备精确 Qt 6.11.2 macos SDK、FFmpeg 7.1.3 头文件、CMake、Ninja、Python 3 和 Xcode 27 工具，使用此前的同事构建说明完成依赖准备。不要把该说明中的旧构建结果当作修复版产物。配置正确 SDK 路径后，从 `repair` 运行：

```bash
bash application/scripts/rebuild-qt-cocoa-fix.sh /absolute/path/to/Qt/6.11.2/macos
bash application/scripts/build-macos-fixed.sh /absolute/path/to/Qt/6.11.2/macos
```

两步需串行完成，并保留完整日志。原生 GUI 测试不与另一套 GUI 自动化并行运行。最终 DMG 名称可能不含“Mac修复复测”；分发前仍需重新核验镜像、签名、实际加载插件和最终原生测试。新构建的哈希不要求等于旧镜像哈希，但必须与该次实际产物记录对应。

## 已知脚本限制

- 返回的脚本按上述父目录布局寻址，不可直接散拷到任意工作区运行。
- 重建脚本发现已有 `sources/qtbase-6.11.2` 时不会重新展开归档。不要预先复制回传的三份 Qt 源文件到这个路径，否则得到不完整源码树；完整源归档应由脚本下载、校验再解压。
- 不复用来源不明的源码树或旧 CMake 构建缓存。更换 SDK 时使用另一个全新工作目录。本次已验收产物不受此限制影响，后续重建仍需验证。
- `build-macos-fixed.sh` 从重建清单取得插件哈希，在 `macdeployqt` 后、签名前替换 Cocoa 插件；部署脚本校验哈希、arm64 和精确 Qt 6.11.2。不要跳过门禁或只替换官方 SDK 外部的文件。
- 这里保留的是经过 Mac 执行的原始脚本，不宣称已将主工程的通用 Mac 构建入口改成等效修复流程。
