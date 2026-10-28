# Windows 发布

从项目根目录运行 pwsh -File scripts/build.ps1 -Package。脚本使用现有 Qt 6.9.3 / MinGW 构建、运行完整测试，再从一个全新部署目录生成：

- dist/源音效管理工具-安装版-<version>.exe：当前用户安装，不要求管理员权限；
- dist/源音效管理工具-免安装版-<version>.zip：解压到本机磁盘即可运行；
- dist/YuanAudio-Windows-<version>-manifest.json：逐文件 SHA-256 清单。

安装版与免安装版使用同一个 YuanAudio.exe 和同一套 Qt/FFmpeg 依赖；安装器只负责复制文件、快捷方式及卸载。卸载会删除本次安装的程序文件，不删除 %LOCALAPPDATA%/AudioLibrary 下的索引、收藏、设置和波形缓存。安装升级会先检查现有程序文件是否可替换，再调用上一安装版的卸载程序清理旧程序文件，最后部署新版。旧程序或后台任务仍占用文件时会在任何卸载操作前停止；这项检查不等于断电、磁盘写满等故障下的完整自动回滚。

`build.ps1 -Package` 还会运行 `tests/installer_safety.ps1`。它在独立临时目录构建和运行安装器，验证旧版运行中升级、当前版运行中卸载、DLL 占用、目录链接、无关用户文件保留等行为，测试版本不会写入真实快捷方式或注册表安装记录。

打包需要 NSIS 3.12。开发机可将[官方便携 ZIP](https://sourceforge.net/projects/nsis/files/NSIS%203/3.12/nsis-3.12.zip/download)解压到 .tools/nsis-3.12/，使 .tools/nsis-3.12/nsis-3.12/makensis.exe 存在。ZIP 的 SHA-256 应为 56581f90db321581c5381193d796fffcf2d24b2f8fed2160a6c6a3baa67f2c4f。NSIS 的[许可条款](https://nsis.sourceforge.io/License)允许商业使用。也可将 NSIS 安装到默认的 Program Files 目录。

安装包尚未做代码签名。对外广泛分发前应使用发行者证书签名，并在一台未安装开发工具的 Windows 电脑上复核首次安装、升级和卸载。
