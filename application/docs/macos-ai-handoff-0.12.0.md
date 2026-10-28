# 源音效管理工具 0.12.0｜交给 Mac 上的 AI 执行的构建说明

更新日期：2026-10-27。目标设备：M 系列芯片、macOS 27 或更新版本。

## 同事只需要做这几步

1. 把本说明和 `源音效管理工具-macOS27-同事构建包-0.12.0.zip` 下载到 Mac 本机。若收到的是“Mac构建资料”压缩包，先解压外层，里面就是这两个文件。
2. 使用能读取本地文件、执行终端命令的 AI 开发工具；如需检查界面，它还要能查看或操作 Mac 桌面。只有文字聊天能力的 AI 无法替你完成实际构建。
3. 把这两个文件交给 AI，复制下面这句话：

> 请完整阅读《源音效管理工具 0.12.0｜交给 Mac 上的 AI 执行的构建说明》，按说明在这台 Mac 上完成环境检查、构建、测试和 DMG 打包。遇到可在项目内修复的问题请直接修复并记录；需要我操作系统界面或提供权限时再具体说明。不要只给操作建议，最终把可回传的文件集中打成一个 ZIP，告诉我它在哪里。

4. 如果 AI 提示缺少开发工具或系统要求不符，按它给出的具体原因处理。Apple 账户登录、密码等由本人在系统界面完成，不发给 AI。
5. AI 完成后，把它生成的 `源音效管理工具-macOS27-0.12.0-回传开发包.zip` 发回原开发同事。失败时也回传包含完整日志的失败包。

以下内容是给执行 AI 的工作指令。

---

## 1. 任务目标和现状

你要基于附件中的 **0.12.0 原始源码包**，在当前 Mac 原生构建一个可安装的 Apple Silicon DMG，并给出真实、可复查的验证结果。

软件名：源音效管理工具。主要用途是多音乐库搜索、文件夹定位、行内波形试听，以及把整首服务器原音频拖到达芬奇。0.12.0 包含清透浅色、石墨深色和紧凑播放条。

- 当前 0.12.0 已完成 Windows 验证，Mac 端需要本次实际构建与验证。
- 之前 0.10.0 的 Mac 回传结果仅供排错参考，不能当作本次的验证结果，也不能改名成 0.12.0 交付。
- 本任务以构建、兼容性修复、验证、打包为范围，不新增功能或重做界面。
- 默认先产出可供内部测试的预览 DMG。没有开发者发行证书不必卡住整个构建，但必须注明实际签名、公证状态。
- 不要修改公司的音乐原文件、现有索引、正式共享缓存或当前安装的旧应用。GUI 验证使用独立测试资料目录和自己生成的测试音频。

## 2. 确认输入和工作目录

原始源码包文件名：

```text
源音效管理工具-macOS27-同事构建包-0.12.0.zip
```

该原始 ZIP 的 SHA-256 应为：

```text
76550bf393a5c906a670de27fb2069c96e2ebe8e8f667002e12eab7c0f3eb197
```

请用 `shasum -a 256` 核对这个原始 ZIP，不是外层“Mac构建资料”ZIP。校验不匹配时先报告，不直接执行来源不一致的脚本。

解压到一个新的、本机独立工作目录，不覆盖已有开发目录。项目根目录名为：

```text
YuanAudio-macOS27-arm64-source-0.12.0
```

后文所有相对路径都以这个目录为起点。自行定位实际路径，命令中的路径需要正确引用；不要原样执行“实际路径”等占位文字。

开始前读取：

- `CMakeLists.txt`
- `scripts/prepare-macos.sh`
- `scripts/build-macos.sh`
- `docs/macos-release.md`
- `docs/mac-return-validation-0.10.0.md`

确认 CMake 项目版本为 `0.12.0`。保留一份未修改源码或文件摘要，便于回传修复差异。

## 3. 检查运行环境

进入项目根目录后执行并记录实际输出：

```bash
mkdir -p dist
{
  date -u '+%Y-%m-%dT%H:%M:%SZ'
  uname -s
  uname -m
  sw_vers
  xcode-select -p
  xcrun --sdk macosx --show-sdk-version
  xcrun clang --version
  python3 --version
  df -h .
} > dist/macos-environment.log 2>&1
cat dist/macos-environment.log
```

逐项判断，不能仅凭这一组命令的最后退出码判断环境合格：

- 系统必须是 Darwin，原生运行架构应为 `arm64`。若 M 系列 Mac 的终端运行在 Rosetta 下，应改用原生终端/进程，再检查。
- 系统版本需为 macOS 27 或更新版本，当前选择的 macOS SDK 也需至少 27。
- 需要可用的 Apple 开发工具和 Python 3。仅“装过 Xcode”不代表当前选中的 SDK 合格。
- 检查本地磁盘空间和网络，下载 Qt、Python 构建依赖、FFmpeg 头文件需要联网。

如果开发工具缺失、许可未完成或 SDK 不足，请向同事报告实际检测值和具体需要处理的事项。优先使用已经安装的合适工具；如需选择其他 Xcode，可为当前进程设置 `DEVELOPER_DIR`。不要靠降低 macOS 版本要求或改成 x86_64 绕过检查。

脚本会在项目内创建 `.venv-macos` 和 `.tools`。优先使用这些局部依赖，不必另外安装全局 Qt、FFmpeg 或 Homebrew。

## 4. 构建并保存从头到尾的日志

在项目根目录执行：

```bash
mkdir -p dist
bash -o pipefail -c 'bash scripts/prepare-macos.sh 2>&1 | tee dist/macos-bootstrap.log'
```

记录这个命令的退出码。必须保留 `pipefail`，否则 `tee` 成功可能掩盖前面的构建失败。

新增的外层 `macos-bootstrap.log` 包括依赖安装和下载阶段；现有脚本的 `macos-build.log` 主要记录进入正式编译后的过程。早期失败时可能没有后者，所以不能只回传后者。

现有脚本会完成：

1. 准备 Python 虚拟环境、aqtinstall、CMake、Ninja。
2. 下载 Qt 6.11.2 和 Qt Multimedia；准备并校验 FFmpeg 7.1.3 源码头文件。
3. 使用 Release 配置和 `arm64`、macOS 最低版本 `27.0` 构建。
4. 运行 CTest 全部测试。未修改的当前源码应注册 27 项，以本次实际列表和输出为准。
5. 部署 Qt、SQLite 插件和 FFmpeg 等应用依赖，检查架构、动态库引用和运行时搜索路径。
6. 执行应用签名校验、打包后的解码回归，制作 DMG、校验镜像并生成 SHA-256 文件。

注意：下载命令里 Qt 的包名 `clang_64` 不是改用 Intel 构建的理由。最终以构建参数和 `lipo` 对每个实际二进制的检查为准。

日志有输出或子进程仍正常工作时让它继续，不要仅因下载或编译耗时就反复中止。若确实停滞，先确认停在哪一阶段、子进程状态和最后输出，再决定如何恢复。

## 5. 失败时如何处理

- 找到最早的实质错误，区分网络下载、开发环境、编译、测试、依赖部署和签名问题。
- 网络中断可重试。重试前把已有日志另存为带轮次或时间的文件，避免覆盖第一次故障证据。
- 缓存损坏时只处理项目内部、已确认损坏的下载或构建目录；路径必须先确认。不要清理用户其他工程或系统目录。
- 可以做必要的最小 Mac 兼容性修复。每项修改说明原问题、原因、修改文件和验证结果，不顺便改产品功能。
- 不删除测试、弱化断言、伪造成功记录或跳过签名/动态库检查来让流程“通过”。若确定测试本身有跨平台假设问题，可以修正测试，但保留它真正要验证的行为并写明原因。
- 修改后先运行相关检查，最终交付前让完整构建和全部测试在最终源码上通过。
- 无法解决的环境或测试问题要如实报告；可回传失败材料，但不要把未通过验证的文件称为最终安装包。

若改了源码或脚本，回传 `source-changes.patch`，并附上修改后的源文件与改动清单。源码 ZIP 不一定带 Git 历史：没有 Git 仓库时，可与保留的原始源码比较生成差异，或提供带原相对路径的修改文件；不要因此丢失修复内容。

## 6. 核对最终产物

脚本成功后，检查这些路径确实存在：

```text
dist/源音效管理工具-macOS27-Apple芯片-0.12.0.dmg
dist/源音效管理工具-macOS27-Apple芯片-0.12.0.dmg.sha256
dist/YuanAudio-macOS27-arm64-0.12.0.app
dist/macos-build.log
dist/macos-bootstrap.log
dist/macos-environment.log
```

执行并把输出保存到报告或独立日志：

```bash
MAC_APP='dist/YuanAudio-macOS27-arm64-0.12.0.app'
MAC_DMG='dist/源音效管理工具-macOS27-Apple芯片-0.12.0.dmg'
lipo -archs "$MAC_APP/Contents/MacOS/AudioLibrary"
/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$MAC_APP/Contents/Info.plist"
/usr/libexec/PlistBuddy -c 'Print :LSMinimumSystemVersion' "$MAC_APP/Contents/Info.plist"
codesign --verify --deep --strict --verbose=2 "$MAC_APP"
hdiutil verify "$MAC_DMG"
(cd dist && shasum -a 256 -c '源音效管理工具-macOS27-Apple芯片-0.12.0.dmg.sha256')
```

逐项核对退出码、版本和架构。应用内部主可执行文件叫 `AudioLibrary`，这不是错误；用户看到的应用名称是“源音效管理工具”。

现有脚本默认使用 ad hoc 签名，`codesign --verify` 成功不等于 Apple 公证通过或可以无提示分发。没有发行证书时，在报告中写“内部预览包，ad hoc 签名，未公证”。如果同事已有合适的 Developer ID 证书和钥匙串中的公证配置，可以按现有脚本的 `APPLE_CODESIGN_IDENTITY`、`APPLE_NOTARY_PROFILE` 接入，完成后再写实际结果。不要请求同事把密码发到聊天中，也不要关闭全局 Gatekeeper。

## 7. 检查打包后的应用能否实际使用

先验证 `dist` 里的独立应用，不覆盖 `/Applications` 里的旧版。用独立资料目录启动可避免读取或修改同事已有的正式软件资料：

```bash
MAC_APP='dist/YuanAudio-macOS27-arm64-0.12.0.app'
TEST_PROFILE="$(mktemp -d "${TMPDIR:-/tmp}/yuanaudio-0.12-smoke.XXXXXX")"
printf '%s\n' "$TEST_PROFILE" > dist/macos-smoke-profile-path.txt
"$MAC_APP/Contents/MacOS/AudioLibrary" --data-dir "$TEST_PROFILE" > dist/macos-ui-smoke.log 2>&1 &
printf '%s\n' "$!" > dist/macos-smoke-process-id.txt
```

记录本次启动的进程，测试结束只正常退出这一个测试实例，不停止用户其他实例。不要把临时测试资料复制到正式配置目录。

在自己创建的两套本地测试音乐目录中，用生成的音频或明确用于测试的文件检查：

- 应用能启动，版本显示正确；清透浅色、石墨深色可切换，设置重启后保留。
- 音乐行内波形正常；点击波形可从对应位置试听；底部进度拖动后能定位，播放和音量可用。
- 添加两个音乐库，分别搜索音乐、定位左侧文件夹；当前目录和选曲在切换主题后保持。
- 仅从列表移除单曲/文件夹后，测试原文件仍存在；恢复排除项后可以重新收录。整库移除不影响另一个库。
- 查看设置中的波形缓存页，确保浅色和深色下文字、进度、按钮都可读。

如果没有 GUI 操作能力，或无人配合试听，只写“未完成实机交互/听音验证”，不能把进程存在、自动测试通过或截图中的图标当作已经听到声音。

有公司的测试 SMB 共享、达芬奇和操作条件时，再完成以下附加验收；没有就明确标记未测，不因此虚构结果或阻止交付已验证的内部预览包：

- Windows 的 `Z:\` 不能直接在 Mac 使用，先挂载 SMB，再选择 Mac 上实际挂载的测试目录。
- 用专门的空测试共享缓存目录验证首次初始化，再验证已有合法缓存目录的复用；不清理正式共享缓存。原 0.10.0 的 Mac SMB 首次初始化曾失败，新版后备路径需要本次实际确认。
- 从音乐名称和波形分别把整首文件拖入达芬奇媒体池、时间线，检查它引用的仍是服务器原文件。仅从 Finder 导入或执行拖拽单元测试不等于验证了软件直拖。
- 本次小样本不能证明 1 TB 全库性能、6–10 人实际并发或胜过其他软件。

## 8. 生成回传包

在 `dist` 下建一个单独的回传目录，只收集本次必要文件，再压缩为：

```text
源音效管理工具-macOS27-0.12.0-回传开发包.zip
```

成功回传至少包含：

```text
源音效管理工具-macOS27-Apple芯片-0.12.0.dmg
源音效管理工具-macOS27-Apple芯片-0.12.0.dmg.sha256
macos-bootstrap.log
macos-build.log
macos-environment.log
macos-ui-smoke.log                         # 若启动过 GUI 测试
macos-validation-report.md
source-changes.patch                       # 仅修改过源码/脚本时
changed-source/                            # 仅修改过源码/脚本时，保留原相对路径
screenshots/                               # 实际拍摄的界面截图；没有则注明
```

请额外计算回传 ZIP 的 SHA-256，作为同名 `.sha256` 文件放在 ZIP 外面。不要把 ZIP 打包进自己，也不要把 Qt SDK、`.venv-macos`、整个 `build-macos`、公司的音乐、完整个人索引或账户资料打进去。

`macos-validation-report.md` 至少写清：

1. 实际设备芯片、系统版本、SDK/编译器版本、原始源码 ZIP 哈希和产品版本。
2. 构建是否成功、完整测试总数/通过数/失败数、对应日志位置。
3. DMG 哈希、arm64 核对、签名与公证状态、依赖检查结果。
4. GUI、试听、多库、列表移除与恢复、SMB、达芬奇等分别标记“已测通过 / 失败 / 未测”，附真实证据。
5. 修改文件、修改原因、复验结果；没有修改则写“无源码修改”。
6. 仍未解决的问题和未验证范围。

如果构建失败，回传 `源音效管理工具-macOS27-0.12.0-失败回传.zip`：包含已有完整日志、`build-macos/Testing/Temporary/LastTest.log`（存在时）、失败分析、必要的修复差异。不要放入旧版 DMG 冒充本次产物。

## 9. 最后向同事报告

完成后用简短中文说明：

- 本次是否成功生成 0.12.0 DMG。
- 回传 ZIP 和外部校验文件的完整本地路径。
- 测试与签名的真实状态，哪些项目仍未测。
- 请同事把回传 ZIP 和 `.sha256` 两个文件发回原开发同事。

在原开发同事收到回传、核验修复差异和产物之前，不要声称 Windows 开发端已经收到或验收，也不要替同事擅自发邮件、上传云盘或发布到外部网站。
