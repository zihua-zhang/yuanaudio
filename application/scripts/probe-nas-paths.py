"""Read-only NAS access diagnosis, separate from audio decoding. Never renames files.

Accepts a UTF-8 JSON array of absolute paths, or the existing waveform-failures.sqlite.
Writes JSON to a NEW file; probes at most --limit entries (default 12), no recursion.
"""
import argparse
import json
import os
from pathlib import Path
import platform
import sqlite3
import unicodedata


def attempt(path):
    result = {"path": os.fsdecode(path)}
    try:
        stat = os.stat(path)
        result.update(size=stat.st_size, modified_ns=stat.st_mtime_ns)
        with open(path, "rb") as source:
            result["head_bytes_read"] = len(source.read(4096))
            source.seek(max(0, stat.st_size - 4096))
            result["tail_bytes_read"] = len(source.read(4096))
        result["readable"] = True
    except OSError as e:
        result.update(readable=False, errno=e.errno, winerror=getattr(e, "winerror", None), message=str(e))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--paths-json", type=Path)
    group.add_argument("--failure-db", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--limit", type=int, default=12, choices=range(1, 231), metavar="1..230")
    args = parser.parse_args()
    if args.paths_json:
        paths = json.loads(args.paths_json.read_text(encoding="utf-8-sig"))[:args.limit]
    else:
        with sqlite3.connect(args.failure_db.resolve().as_uri()+"?mode=ro", uri=True) as db:
            snapshots = db.execute("SELECT snapshot FROM failures WHERE kind='source_unavailable' ORDER BY id LIMIT ?",
                                   (args.limit,)).fetchall()
        paths = [str(Path(s["root"]) / s["path"]) for (raw,) in snapshots for s in [json.loads(raw)]]
    report = {"platform": platform.platform(), "scope": "read-only path probes; no audio decoding, no rename or cache changes", "files": []}
    for value in paths:
        path = Path(value)
        if not path.is_absolute():
            raise ValueError("Expected an absolute path")
        if args.output.absolute() == path.absolute():
            raise ValueError("Output cannot be a source path")
        item = {"indexed_path": str(path), "basename_codepoints": [f"U+{ord(c):04X}" for c in path.name],
                "exact": attempt(str(path)), "enumerated_candidates": []}
        # Bytes names from readdir preserve the OS spelling. A matching entry may
        # still fail to open on SMB; report that result without guessing a remedy.
        try:
            parent = os.fsencode(path.parent) if os.name != "nt" else str(path.parent)
            for name in os.listdir(parent):
                decoded = os.fsdecode(name)
                if unicodedata.normalize("NFC", decoded) == unicodedata.normalize("NFC", path.name):
                    candidate = attempt(os.path.join(parent, name))
                    candidate["name_codepoints"] = [f"U+{ord(c):04X}" for c in decoded]
                    candidate["name_bytes_hex"] = os.fsencode(name).hex()
                    item["enumerated_candidates"].append(candidate)
                    if len(item["enumerated_candidates"]) >= 8:
                        break
        except OSError as e:
            item["directory_error"] = {"errno": e.errno, "message": str(e)}
        report["files"].append(item)
    # Exclusive create prevents accidental overwrite of any existing user file.
    with args.output.open("x", encoding="utf-8") as output:
        json.dump(report, output, ensure_ascii=True, indent=2)
    print(f"Wrote {len(paths)} read-only probes to {args.output}")


if __name__ == "__main__":
    main()
