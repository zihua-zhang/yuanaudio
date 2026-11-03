# 0.12.7 Finder 定位与移除候选功能

日期：2026-11-03。基线：323ce287（0.12.6）。本机 Apple 芯片，构建测试系统实际为 macOS 27.2。

## 改动

- `application/src/window_workspace.cpp`：音乐行右键新增“在 Finder 中显示”，通过当前 library/relative 身份查索引原件；后台检查路径，使用独立参数调用 `/usr/bin/open -R`，不经 shell，不用兼容缓存替代原件。缺失或不可访问时提示失败。最近试听使用同一路径。
- `application/src/workspace_panel.*`：移除候选选择、管理、加入和移除入口；保留最近试听、常用目录。保存视图的原数值 1/2 保持兼容，旧值 0 转最近试听。
- `application/src/workspace_store.cpp`：停止自动建立或重建默认候选清单；历史候选表与记录保留，供回退读取，不清空用户数据。
- 版本号统一 0.12.7；补充工作区与菜单回归测试、使用说明。

## 验证

- 新菜单及旧候选视图迁移测试先在旧源码失败，再在新源码通过。
- 正式入口 `bash 开始构建.command` 完成：CTest 37/37；Qt 6.11.2 Cocoa 原生回归 12/12；打包后解码回归、arm64 依赖、ad hoc 签名校验和 DMG CRC 通过。
- 独立 `--data-dir` 的两首合成 WAV 载入成功，包含中文、韩文、日文、空格、单引号、井号、百分号与 &。源字节 SHA 不变。
- Finder 交互验收通过（用户实测）：2026-11-03，用户在安装版 0.12.7 中右键选择“在 Finder 中显示”，反馈“能正常打开并选中文件”。此前自动桌面操作未可靠调出菜单，此项结论来自用户实际操作确认；不扩大为所有特殊文件名或 NAS 路径均已实测。
- 安装版 0.12.7 已正常打开正式资料，实际加载安装包内修复的 Cocoa 插件。39763 首、1 个素材库；完整索引、素材库配置、收藏标签摘要与更新前完全一致。现有缓存文件 223509 个，原文件未缺失，大小及 mtime 未改变（不是有效波形数量宣称）。界面原有参考波形 747、待处理异常 4，本次没有重试或改写这些结论。

## 交付与回退

- 安装位置：`/Applications/源音效管理工具.app`。
- DMG：`application/dist/源音效管理工具-macOS27-Apple芯片-0.12.7.dmg`。
- DMG SHA-256：`a82311e4ee8fc22b0a6cd0ba38679f3643be3f5a6af4d9439fe486501dfe4a9f`。
- APP 主程序 SHA-256：`24956e599c91a515475a9272d5914e6640c83389e001295479bfa7113bf3300f`。
- 本机备份：`~/Library/Application Support/源音效安装备份/20261103-升级0.12.7`，含更新前 profile 与 0.12.6 APP。没有复制或上传公司音频。
- 本机证据：`logs/macos-fixed-0127-build.log`、`finder-0127-red-tests.log`、`finder-0127-targeted-tests.log`、`formal-before-0127.json`、`formal-after-0127.json`、`peaks-after-0127.json`、`install-0127.json`、`installed-0127-cocoa-loaded.txt`。
- 仅 ad hoc 签名，未完成 Developer ID 签名或公证。此前 NAS GUI 原生授权限制此次未复测，不宣称解决。Windows/Linux 文件管理器分支未实机验证。
