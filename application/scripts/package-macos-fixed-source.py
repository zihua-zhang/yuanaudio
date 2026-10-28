"""Package current app sources with the preserved, verified Qt Cocoa repair layout."""
from pathlib import Path
import hashlib
import json
import re
import zipfile

project = Path(__file__).resolve().parent.parent
version = re.search(r"project\(AudioLibrary VERSION ([\d.]+)",
                    (project / "CMakeLists.txt").read_text(encoding="utf-8")).group(1)
recipe = project / "packaging/macos-0.12.0-verified-recipe"
payload = {}
for directory in (".github", "assets", "docs", "licenses", "scripts", "src", "tests", "packaging", "patches"):
    for path in (project / directory).rglob("*"):
        if path.is_symlink():
            raise RuntimeError(f"Unexpected source link: {path}")
        if path.is_file() and "__pycache__" not in path.parts and path.suffix != ".pyc":
            payload["application/" + path.relative_to(project).as_posix()] = path.read_bytes()
for name in ("CMakeLists.txt", "README.md", "THIRD_PARTY_NOTICES.md", ".gitignore"):
    payload["application/" + name] = (project / name).read_bytes()

# Retain the exact Mac-tested scripts and their expected parent directory layout.
manifest = json.loads((recipe / "returned-file-sha256.json").read_text(encoding="utf-8"))
for relative, expected in manifest.items():
    source = recipe / relative
    data = source.read_bytes()
    if hashlib.sha256(data).hexdigest() != expected:
        raise RuntimeError(f"Verified Qt recipe changed: {relative}")
    payload[relative] = data

# Reuse dependency preparation, but rebuild Qt and deploy its fixed plugin before signing.
bootstrap = (project / "scripts/prepare-macos.sh").read_text(encoding="utf-8")
marker = 'mkdir -p "$project_dir/dist"'
if bootstrap.count(marker) != 1:
    raise RuntimeError("Dependency bootstrap layout changed; review the package entry point")
bootstrap = bootstrap.split(marker)[0] + '''mkdir -p "$project_dir/dist"
export PATH="$venv/bin:$PATH"
{
  bash "$project_dir/scripts/rebuild-qt-cocoa-fix.sh" "$qt_dir"
  bash "$project_dir/scripts/build-macos-fixed.sh" "$qt_dir"
} 2>&1 | tee "$project_dir/dist/macos-fixed-build.log"
echo '完成：请回传 dist 中的 DMG、校验文件和 macos-fixed-build.log。'
'''
payload["application/scripts/prepare-fixed-macos.sh"] = bootstrap.encode("utf-8")
payload["开始构建.command"] = b'''#!/usr/bin/env bash
set -euo pipefail
bundle_dir="$(cd "$(dirname "$0")" && pwd -P)"
exec bash "$bundle_dir/application/scripts/prepare-fixed-macos.sh"
'''
payload["给同事AI的说明.md"] = (project / f"docs/macos-decoder-handoff-{version}.md").read_bytes()
payload["source-sha256.json"] = (json.dumps({name: hashlib.sha256(data).hexdigest()
                                          for name, data in sorted(payload.items())},
                                         ensure_ascii=False, indent=2) + "\n").encode("utf-8")
prefix = f"YuanAudio-macOS27-arm64-fixed-source-{version}"
archive = project / "dist" / f"源音效管理工具-Mac解码修复构建包-{version}.zip"
archive.parent.mkdir(exist_ok=True)
if archive.exists():
    raise RuntimeError(f"Refusing to overwrite a published source archive: {archive}")
with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as output:
    for name, data in sorted(payload.items()):
        entry = zipfile.ZipInfo(prefix + "/" + name)
        entry.create_system = 3
        entry.external_attr = (0o100755 if name.endswith((".sh", ".command")) else 0o100644) << 16
        output.writestr(entry, data, compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
with zipfile.ZipFile(archive) as check:
    if check.testzip() is not None or len(check.infolist()) != len(payload):
        raise RuntimeError("Archive integrity failed")
    for name, data in payload.items():
        if check.read(prefix + "/" + name) != data:
            raise RuntimeError(f"Archive content mismatch: {name}")
digest = hashlib.sha256(archive.read_bytes()).hexdigest()
archive.with_name(archive.name + ".sha256").write_text(f"{digest}  {archive.name}\n", encoding="utf-8")
print(json.dumps({"archive": str(archive), "files": len(payload), "sha256": digest}, ensure_ascii=True))
