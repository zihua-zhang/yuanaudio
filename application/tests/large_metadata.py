"""Paced large cover probing reports real read progress before duration is known."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time

with tempfile.TemporaryDirectory(prefix='audio-large-cover-') as directory:
    root = Path(directory)
    source = root / 'cover.mp3'
    original = (Path(__file__).parent / 'fixtures/sine-with-cover.mp3').read_bytes()
    tag_end = 10 + sum((original[6+i] & 127) << (21-7*i) for i in range(4))
    pos = original.find(b'APIC', 10, tag_end)
    assert pos >= 0
    size = int.from_bytes(original[pos+4:pos+8], 'big')
    if original[3] == 4:
        size = sum((original[pos+4+i] & 127) << (21-7*i) for i in range(4))
    payload = original[pos+10:pos+10+size]
    payload += bytes(18*1024*1024 - len(payload))
    frame = b'APIC' + len(payload).to_bytes(4, 'big') + b'\x00\x00' + payload
    n = len(frame)
    header = b'ID3\x03\x00\x00' + bytes((n >> shift) & 127 for shift in (21, 14, 7, 0))
    source.write_bytes(header + frame + original[tag_end:])
    config = dict(path=str(source), relative='cover.mp3', library='cover', local=str(root/'cache'), readBytesPerSecond=1048576)
    started = time.monotonic()
    process = subprocess.Popen([sys.argv[1], '--worker', 'wave'], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.DEVNULL, text=True, encoding='utf-8')
    readings = []
    complete = False
    try:
        process.stdin.write(json.dumps(config)+'\n')
        process.stdin.close()
        for line in process.stdout:
            event = json.loads(line)
            if event.get('event') == 'reading':
                readings.append(time.monotonic()-started)
            complete |= event.get('event') == 'wave' and event.get('complete', False)
        assert process.wait(timeout=5) == 0
        assert readings and readings[0] < 3, ('no early read progress', readings)
        assert all(b-a < 3 for a, b in zip(readings, readings[1:])), readings
        assert time.monotonic()-started > 15 and complete, 'large metadata fixture did not exercise watchdog interval'
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        process.stdout.close()
    print(f'PASS: first read progress {readings[0]:.2f}s, {len(readings)} progress events; long probing completed')
