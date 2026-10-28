# NAS Path Repair Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 原件保持不变，在软件异常窗口批量创建并使用校验过的 NAS 兼容副本，普通文件失败不中断批次。

**Architecture:** Qt/C++ 后台任务结合原生 UGOS 客户端；版本绑定映射与原始曲目身份分开。Swift CryptoKit/Security 提供公开系统加密 API，不依赖外部浏览器或开发工具。

**Tech Stack:** Qt 6.11.2 / C++20 / SQLite / Swift CryptoKit / macOS 27 arm64.

**Spec:** docs/superpowers/specs/2026-10-28-nas-path-repair-design.md

## Global Constraints

- 原始音频永不修改；路径失败逐项记录并继续，不再因连续八次失败暂停。
- 不把账号密码或令牌写入配置、日志、源码或检查点。
- 保留索引 user_version=6；保留 Qt 6.11.2 官方修复及部署校验。
- 映射建立不等于波形成功；参考波形始终保留参考标记。

## Review Focus

- 规范化重名或目录多义时拒绝猜选；测试匹配冲突。
- NAS 源文件处理中变更，拒绝登记映射；测试版本核对。
- 副本丢失或同大小变更，不能静默使用错误文件；测试完整哈希。
- 中断/重启保留已成功项但不保存凭据；检查点续跑测试。
- 工作区旧快照仍应取得新的映射；测试拖出和试听刷新。

### Task 1: Queue continuation
**Files:** wave_queue.cpp/.h, queue_policy_tests.cpp.
- [ ] RED: 十个缺失文件后有效 WAV，串行/2/4 并行及失败重试均完成；storage_full 仍暂停。
- [ ] GREEN: 删除按次数自动暂停，保留持久化/磁盘保护与用户暂停。
- [ ] Verify: queue_policy_tests 全部通过，记录 RED/GREEN。

### Task 2: Version-bound compatible sources
**Files:** index.h/.cpp, compatible_sources_tests.cpp, workers.cpp, window.cpp, workspace_panel.cpp, failure_store.cpp.
**Interfaces:** Track::sourcePath/sourceMatches; Index::setCompatibleSource(original,path,sha256,error).
- [ ] RED: 哈希不符、原件版本/根变化拒绝；三个查询保留原身份并读取副本映射。
- [ ] GREEN: 额外表及 JOIN；播放/波形/拖出使用验证路径，原路径操作保持原语义。
- [ ] Verify: compatible_sources、worker integration 和原文件删除测试。

### Task 3: Native UGOS client
**Files:** ugos_client.h/.cpp, ugos_crypto.swift, ugos_client_tests.cpp, CMakeLists.txt.
**Interfaces:** background-thread login/call/download; runtime credentials only.
- [ ] RED: 加密已知向量与回环服务请求/响应校验。
- [ ] GREEN: RSA PKCS1 + AES256GCM，QtNetwork 限定源站请求、超时、下载流哈希。
- [ ] Verify: ugos_client_tests；真实 NAS 只读登录/列表抽样。

### Task 4: Batch repair and UI
**Files:** path_repair.h/.cpp, window_path_repair.cpp, path_repair_tests.cpp, window_failures.cpp.
**Interfaces:** repair options/candidates/checkpoint; background progress/result, cooperative pause.
- [ ] RED: 路径边界、歧义、copy-only、校验失败、检查点及继续下一项测试；入口存在。
- [ ] GREEN: 原生客户端匹配和复制、命名、哈希与映射，异常窗口入口和进度/暂停。
- [ ] Verify: 新测试、真实受控 NAS 定点、最终 GUI 入口与使用副本。

### Task 5: Release validation
- [ ] 全套 CTest；独立检查最终 APP 的 Qt Cocoa 修复插件和原生回归。
- [ ] 备份正式资料，升级安装验证通过的新版本，不清库不全库重扫。
- [ ] 产出 APP/DMG、源码和 Git 状态、SHA256、测试日志和限制说明。
