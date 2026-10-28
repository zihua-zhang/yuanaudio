# macOS 0.12.2 定点修复与验证计划

> For agentic workers: use superpowers:executing-plans inline. The user explicitly authorized execution; no additional plan approval is required.

**Goal:** 在 macOS 27 Apple Silicon 上交付可复现构建、真实 NAS 定点结果和安全隔离的 APP/DMG。
**Architecture:** 保留交接 source 快照及恢复仓库；在独立 mac-build 基线后验证现有 0.12.2，只有复现的缺陷才定点修复。
**Tech Stack:** Qt 6.11.2 / FFmpeg 7.1.3 / C++20 / Python / Apple clang21。
**Spec:** 交接根 00-先读我-接手AI.md、handoff/02、04、05、07 及本次用户明确要求。

## Global Constraints
- 不写公司原音频，不清正式库/缓存/异常，不全库重扫。
- 保留 Qt 官方补丁、原生测试和最终部署/实际加载证据。
- 独立 --data-dir、本机缓存；参考波形不自动变准确。
- 不上传音乐或凭据；不默认升级 /Applications。

## Review Focus
- 有效 PCM + 尾部错误应保留参考波形及诊断。
- no-audio、ENOENT、读取中断分类必须分开。
- Unicode raw/NFC/NFD 均失败时不能伪装修复。
- 最终签名插件身份可能与未签名构建哈希不同，需部署与加载链路。
- 旧 profile / 波形不能因隔离测试写入或丢失。

### Task 1: 基线与正式构建
- [x] 核验1152文件并恢复包含未提交内容的源码；独立mac-build初始commit。
- [x] 正常退出0.12.1；完整profile含外置peaks备份及旧APP归档。
- [x] bash 开始构建.command，保留全输出；应用CTest及原生12项分别通过。

### Task 2: NAS音频与路径证据
**Files:** application/scripts/probe-nas-audio.py、probe-nas-paths.py、src/decoder.cpp、src/workers.cpp。
- [x] 用最终APP执行两首原件probe；与Windows完整SHA对比，保留各自解码诊断、缓存和哈希前后。
- [x] 从已备份异常库抽取8个跨语言样本；系统raw/规范化路径与Qt worker分类对照，并添加同目录可读控制。
- [x] 只对复现且能定位的问题写先失败后修复的回归；如是挂载层限制保留证据，不改音频名或自动重挂载。

### Task 3: 最终GUI与插件
- [x] 最终APP插件原生测试串行；无覆盖环境启动最终APP，核实实际加载的bundle内插件。
- [x] 独立profile仅收录两首原件（或只含这两行的隔离索引），验证波形/时长、搜索切换、定位及播放进度；无听审就标未听审。
- [x] 副本验证旧profile打开兼容性和旧视图，不启动正式profile；不向共享缓存写入。

### Task 4: 交付
- [x] 复查差异及自动化日志；必要时独立代码审查。
- [x] 生成APP/DMG/SHA、验证报告、可回退commit和本地修改补丁；区分通过/失败/未测。
