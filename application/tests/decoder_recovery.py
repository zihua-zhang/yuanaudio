"""Recovery is bounded by progress, not a whole-file error counter.

Synthetic edge cases supplement (do not replace) scripts/probe-nas-audio.py.
"""
import base64
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

exe = str(Path(sys.argv[1]).resolve())
fixture = (Path(__file__).parent / "fixtures/sine-with-cover.mp3").read_bytes()
with tempfile.TemporaryDirectory(prefix="decoder-recovery-") as directory:
    root = Path(directory)
    def run(path, tag):
        cache = root / tag
        config = dict(path=str(path), library="recovery", relative=path.name,
                      local=str(cache), shared="", readBytesPerSecond=0)
        result = subprocess.run([exe, "--worker", "wave"], input=json.dumps(config)+"\n",
                                capture_output=True, encoding="utf-8", errors="replace", timeout=20)
        events = [json.loads(line) for line in result.stdout.splitlines() if line.startswith("{")]
        return result, events, cache

    source = root / "recover-many-packets.mp3"
    # Separated junk regions produce independent invalid packets. One long tail
    # can be one packet regardless of its byte size and does not cover this bug.
    source.write_bytes((fixture + b"junk" * 1024) * 40)
    before = hashlib.sha256(source.read_bytes()).hexdigest()
    result, events, cache = run(source, "many-errors")
    assert result.returncode == 0, (events, result.stderr[-500:])
    diagnostic = next(e["diagnostics"] for e in events if e["event"] == "decode-diagnostics")
    assert diagnostic["issues"] > 32, diagnostic
    assert diagnostic["reachedEnd"] and not diagnostic["stoppedEarly"], diagnostic
    full = next(e for e in events if e.get("complete"))
    assert full["stored"] and full["approximate"] and full["failureKind"] == "decode_warning", full
    data = base64.b64decode(full["data"])
    _, _, rate, _, frames, _, _ = struct.unpack("<IIiiqqi", data[:36])
    assert 79 < frames/rate < 83, "All forty usable segments must survive"
    view = next(cache.rglob("*.view")).read_bytes()
    assert view[:4] == b"AWV3"
    size = struct.unpack(">I", view[12:16])[0]
    metadata = json.loads(view[16:16+size])
    assert metadata["issues"] == diagnostic["issues"] and metadata["decodedMs"] > 1900
    assert not list(cache.rglob("*.json")), "Uncertain waveform must not become a shared accurate artifact"
    assert hashlib.sha256(source.read_bytes()).hexdigest() == before

    # No samples is a decoding result; absence/permissions are a file-access result.
    invalid = root / "invalid.mp3"
    invalid.write_bytes(b"not-audio" * 8192)
    result, events, _ = run(invalid, "invalid")
    assert result.returncode != 0
    assert next(e for e in events if e["event"] == "error")["kind"] == "invalid_audio", events
    result, events, _ = run(root / "missing-가-e\u0301.mp3", "missing")
    assert result.returncode != 0
    assert next(e for e in events if e["event"] == "error")["kind"] == "source_unavailable", events
    assert not any(e.get("event") == "duration" for e in events), "Missing source must not reach decoder"
print("PASS: >32 recoverable errors retain valid PCM; diagnostic cache; no shared publication; access separated")
