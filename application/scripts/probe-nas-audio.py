"""Read-only, two-file decoder probe. No catalog/shared cache or source writes.

python3 probe-nas-audio.py --app /path/to/executable --directory /Volumes/... --output /tmp/report
The output must be a new directory OUTSIDE the source directory. Run once per app version.
"""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import platform
import plistlib
import struct
import subprocess
import time

NAMES = ["14 - 少年强 - Cas14.mp3", "20 - 雄狮出场 - 栾慧.mp3"]


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    source_dir = args.directory.resolve()
    output = args.output.resolve()
    if output == source_dir or source_dir in output.parents:
        parser.error("Output must be outside the source directory")
    output.mkdir(parents=True, exist_ok=False)
    app = args.app.resolve()
    if app.suffix == ".app":
        with (app / "Contents/Info.plist").open("rb") as info:
            executable = plistlib.load(info)["CFBundleExecutable"]
        if Path(executable).name != executable:
            raise ValueError("Invalid CFBundleExecutable")
        app = app / "Contents/MacOS" / executable
    report = {"platform": platform.platform(), "executable": str(app),
              "executable_sha256": digest(app), "directory": str(source_dir),
              "scope": "two original files, isolated local cache, no listening or player validation",
              "files": []}
    # Preserve the exact enumerated spelling/code points, without renaming or normalization.
    try:
        names = os.listdir(source_dir)
        report["listed_names"] = [n for n in names if n.startswith(("14 - ", "20 - "))]
    except OSError as e:
        report["listing_error"] = {"errno": e.errno, "message": str(e)}
    for number, name in enumerate(NAMES, 1):
        path = source_dir / name
        item = {"name": name, "source": str(path)}
        report["files"].append(item)
        try:
            before = path.stat()
            item.update(size=before.st_size, modified_ns=before.st_mtime_ns, sha256=digest(path))
            config = dict(path=str(path), library="nas-diagnostic", relative=name,
                          local=str(output / f"isolated-cache-{number}"), shared="", readBytesPerSecond=0)
            start = time.monotonic()
            result = subprocess.run([str(app), "--worker", "wave"],
                                    input=json.dumps(config, ensure_ascii=False) + "\n",
                                    capture_output=True, encoding="utf-8", errors="replace", timeout=180)
            item.update(exit_code=result.returncode, elapsed_seconds=round(time.monotonic()-start, 3))
            (output / f"decoder-{number}.log").write_text(result.stderr, encoding="utf-8")
            events = []
            for line in result.stdout.splitlines():
                if not line.startswith("{"):
                    continue
                event = json.loads(line)
                data = event.pop("data", None)
                if data:
                    wave = base64.b64decode(data)
                    _, _, rate, channels, frames, _, _ = struct.unpack("<IIiiqqi", wave[:36])
                    event.update(decoded_seconds=frames/rate, channels=channels,
                                 checksum_valid=hashlib.sha256(wave[:-32]).digest() == wave[-32:])
                events.append(event)
            item["events"] = events
            after = path.stat()
            item["source_unchanged"] = (before.st_size, before.st_mtime_ns, item["sha256"]) == (
                after.st_size, after.st_mtime_ns, digest(path))
        except OSError as e:
            item["access_error"] = {"errno": e.errno, "message": str(e)}
        except (subprocess.TimeoutExpired, ValueError) as e:
            item["probe_error"] = str(e)
        (output / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(report, ensure_ascii=True, indent=2))
    return int(any(not f.get("source_unchanged", False) or f.get("exit_code") != 0 for f in report["files"]))


if __name__ == "__main__":
    raise SystemExit(main())
