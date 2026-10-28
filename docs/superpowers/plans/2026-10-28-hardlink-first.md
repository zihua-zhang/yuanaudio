# Hardlink First Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 异常入口优先建立零重复音频数据的兼容硬链接，单项不支持时缓存到本机。
**Architecture:** 原生 libsmb2 后台访问原始名称；PathRepairBackend 统一文件身份、链接、读取和回退；Index 保留原曲身份并指向已验证入口。
**Tech Stack:** Qt6.11.2, C++20, libsmb2 static, SQLite, macOS27 arm64.
**Spec:** docs/superpowers/specs/2026-10-28-hardlink-first-design.md

## Global Constraints

- 不覆盖已有文件，不重命名或写入原音频；不对硬链接 chmod/chown。
- 不开启 SSH、不安装 NAS 服务，不依赖系统 Python/Homebrew/Codex。
- 不将参考波形直接改标准确；不清空用户数据库、缓存和异常记录。
- 账号、密码、认证数据仅在后台会话内存中使用。
- GUI 主动连接和命令行诊断的权限验收分别报告，不绕过系统权限。

## Review Focus

- 同名规范化冲突拒绝猜选。
- 跨卷、只读 NAS、未知 inode 或未确定完成的链接转本机缓存。
- 检查点不能跨服务器/共享或原件版本复用。
- 本机缓存失效或磁盘写满保留证据并暂停，不反复下载整批。
- 原件 inode nlink/ctime 的预期变化不可误判内容变化，也不可因此忽略真正的 size/mtime/inode 变化。

### Task 1 Native SMB
**Files:** third_party/libsmb2; smb_client.h/.cpp; smb_client_tests.cpp; CMakeLists.txt.
- [ ] RED API/path-boundary and bounded failed-session tests.
- [ ] Vendor pinned source and implement background connect/list/fresh stat/link/NewOnly streamed download, typed output failure.
- [ ] Verify native library dependency footprint, unit tests and opt-in controlled NAS integration using the same client code.

### Task 2 Repair strategies
**Files:** path_repair.h/.cpp; path_repair_tests.cpp.
- [ ] RED real fixture hardlink with no download; unsupported/cross-volume/readonly fallback; changed inode; resume/config binding; local disk failure.
- [ ] Implement preferHardLinks/localCacheRoot/serverIdentity and result hardlinked/cached/reused counters, fresh stat proof and original-version mapping.
- [ ] Verify old copy mode and full strategy suite, preserve earlier good evidence.

### Task 3 Chinese UI
**Files:** window_path_repair.cpp; failure_ui_tests.cpp; version metadata.
- [ ] RED dialog includes separate NAS link directory and local cache, explains shared content and cache fallback.
- [ ] Use native SMB account session; remove Linux filesystem path requirement; progress counters distinguish outcomes.
- [ ] Verify UI controls, no account saved, restored 35,723-track profile unchanged.

### Task 4 Review and release
- [ ] Independent whole-branch review and fixes.
- [ ] Complete prescribed Mac build, full CTest, final deployed Cocoa native regression and actual loaded plugin.
- [ ] Controlled real NAS hardlink and local fallback using final app code; GUI permission result explicit.
- [ ] APP/DMG/source/logs/SHA and source Git state; upgrade only validated candidate, retain rollback.
