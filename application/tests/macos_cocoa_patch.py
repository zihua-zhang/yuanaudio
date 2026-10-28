"""The official patch must apply even when the Mac layout is inside a Git repo.

Runs the real bootstrap through its patch step, then stops before compilation.
Requires an existing verified Qt source archive; never downloads or runs GUI tests.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--qt-dir", required=True, type=Path)
args = parser.parse_args()
layout = Path(__file__).resolve().parents[2]
recipe = layout / "handoff/源音效管理工具-Mac闪退修复资料-0.12.0/qt-upstream"
with tempfile.TemporaryDirectory(prefix="cocoa-patch-nested-git-") as temporary:
    root = Path(temporary)
    subprocess.run(["git", "init", "-q", str(root)], check=True)
    sources = root / "sources"
    sources.mkdir()
    shutil.copy2(layout / "sources/qtbase-v6.11.2.tar.gz", sources)
    shutil.copytree(recipe / "baseline", sources / "qtbase-6.11.2")
    destination = root / "handoff/源音效管理工具-Mac闪退修复资料-0.12.0/qt-upstream"
    shutil.copytree(recipe, destination)
    scripts = root / "application/scripts"
    scripts.mkdir(parents=True)
    shutil.copy2(layout / "application/scripts/rebuild-qt-cocoa-fix.sh", scripts)
    stop = root / "stop-before-build"
    stop.write_text("#!/bin/sh\nexit 17\n")
    stop.chmod(0o755)
    environment = dict(os.environ, CMAKE_BIN=str(stop))
    environment.pop("GIT_DIR", None)
    environment.pop("GIT_WORK_TREE", None)
    patch = destination / "qt-8148c48.patch"
    source = sources / "qtbase-6.11.2"
    for run in range(2):
        result = subprocess.run(["bash", str(scripts / "rebuild-qt-cocoa-fix.sh"),
                                 str(args.qt_dir.resolve())], env=environment,
                                capture_output=True, text=True)
        assert result.returncode == 17, (result.returncode, result.stderr)
        verify = subprocess.run(["git", "-C", str(source), "apply", "--reverse", "--check", str(patch)],
                                env=dict(environment, GIT_CEILING_DIRECTORIES=str(sources)),
                                capture_output=True, text=True)
        assert verify.returncode == 0, "Official patch absent after bootstrap run %d: %s" % (run + 1, verify.stderr)
        assert "selection" in (source / "tests/auto/other/qaccessibilitymac/tst_qaccessibilitymac.mm").read_text()
print("PASS: official Cocoa patch applies inside a parent Git repo and remains idempotent")
