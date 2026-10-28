# Qt macOS accessibility patch

Original upstream commit: 8148c48deb8e4345bacb797cb75c011df6f797fa
Source: https://github.com/qt/qtbase/commit/8148c48deb8e4345bacb797cb75c011df6f797fa.patch
Issue: https://qt-project.atlassian.net/browse/QTBUG-149612
Retrieved: 2026-10-27
Patch SHA-256: 494948a1d45b96c0ff16a3432cfde0f0ed205fa56ad923af1ab6c84aa07309b9

The patch is unmodified upstream material, including its native regression test.
Its contexts apply to the corresponding Qt v6.11.2 source files (git apply --check passed).
It is NOT yet compiled or verified for YuanAudio on macOS. Merely storing this patch
in this repository does not alter the Qt runtime used by the application.
Apply and rebuild matching Qt dependencies on the Mac as described in
../先读我-给MacAI的闪退修复续作.md, then verify the actual deployed plugin.

Qt source files retain their original licensing. The production files carry
LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only;
test files retain their own original SPDX identifiers. Refer to the upstream
files and the included Qt license texts. Preserve Qt license/source obligations
when redistributing a rebuilt Qt runtime.
