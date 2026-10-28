# 0.12.5 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans for root integration. Independent core and queue files may use superpowers:dispatching-parallel-agents. Steps use checkbox syntax.

**Goal:** 首次校验复用并自动修复全库路径异常，验证安装后缓存全库并私人备份源码。
**Architecture:** 独立本机验证记录；WaveQueue通过可注入回调挂起/重试同一作业；父进程原生SMB协调器与运行时登录。
**Tech Stack:** Qt6.11.2/C++20/SQLite/libsmb2, macOS27 arm64.
**Spec:** docs/superpowers/specs/2026-10-29-auto-wave-repair-0125-design.md

## Global Constraints
- 不删除/重命名/写入公司原音频，不清空索引、异常账和有效波形。
- 一处正式NAS入口+一处本机缓存；测试只建一个NAS临时目录并清理。
- 凭据只保留父进程运行时；不得入库、入日志或GitHub。
- 保留官方Cocoa修复、指定完整构建、最终实际插件加载验证。
- 首次真实验证通过再升级和运行当前两库全量波形。

## Review Focus
- 同size/mtime的内容/文件身份变化必须使验证凭据失效；损坏/不可写缓存不能绕过完整SHA。
- 4并发首次同曲验证、SQLite损坏/竞争及未知文件ID保持结果正确。
- 暂停、切模式、关闭、迟到异步修复回调不能跳过游标或把完成信号给新曲。
- 认证缺失与连接丢失不可变成数千条新失败；登录只询问一次并正确恢复原模式。
- 旧游标前的历史路径异常必须通过既有重试范围补齐，保持原件版本与参考波形标记。

### Task 1 Verification core
Files: src/media_source.h/.cpp, optional new verification-store unit, tests/media_verification_tests.cpp.
Interface: verifiedMediaPath(track,error,progress,verificationCachePath={}); cache hits report no positive read bytes.
- [x] RED first/repeated/cross-process, changed bytes/inode/timestamps/expectedSHA, corrupt/unwritable cache.
- [x] Implement reliable fingerprint and per-profile persisted receipt, fail-closed verification.
- [x] GREEN focused suite, report exact tests and paths. Root wires CMake/callers.

### Task 2 WaveQueue automatic integration
Files: src/source_repair.h, wave_queue.h/.cpp, wave_parallel.cpp, tests/auto_source_repair_tests.cpp.
Interface: SourceRepairReply {Track mapped; QString error; bool paused=false,needsConnection=false;}; SourceRepairCompletion=std::function<void(SourceRepairReply)>; SourceRepairHandler=std::function<void(const Track&,SourceRepairCompletion)>.
WaveQueue setSourceRepairHandler(handler), resumeAfterSourceRepair(); sourceRepairNeedsConnection(Track,QString), sourceRepaired(Track). Root supplies handler.
- [x] RED source fail→handler→retry success without false failure, one repair per job, genuine failure advances, serial/4parallel, pause/late reply and auth pause/resume.
- [x] Intercept before outcome/cursor, keep job identity and one-attempt marker; refresh mapped snapshots; root handles child pause/auth forwarding.
- [x] Include verificationCache path in wave configs from configurePreparation, propagate to child helpers.
- [x] GREEN focused suite. Root owns CMake/worker changes.

### Task 3 Native coordinator and Window
Files: new source_repair_coordinator unit, window_path_repair.cpp/window.h, workers.cpp/media_drag callers/CMake/version/settings.
- [x] RED runtime auth/no-secret persistence/session reuse, single NAS root, native repair and UI auth-once.
- [x] Background serial coordinator wraps nativeSMB backend and existing runPathRepair; coalesces same key; holds credentials in parent only.
- [x] Wire automatic handler, authorize-only dialog, resume unchanged batch/retry, mapped model/player refresh; pass verification cache to worker/player/drag.
- [ ] Bump0.12.5, preserve data/rollback; full suite and proper build.

### Task 4 Private backup, verification and full cache run
- [ ] Audit all pushed Git blobs/history for credentials/private keys/real media; create private repo, verify owner/visibility, connect remote, baseline tag.
- [ ] Fresh whole-feature review, fix important findings via RED→GREEN, run full suite.
- [ ] Prescribed build, final installed plugin12/12 and actual mapped libraries; independent realNAS small auto flow including existing failures and repeated fingerprint reuse.
- [ ] Install validated preview, source/DMG/logs/SHA, commit/tag/push private remote; verify unchanged formal libraries/favorites/tags.
- [ ] Start existing failures retry then original full preparation automatically with runtime session; monitor until completion or explicit blocker, report actual cached/remaining counts.

