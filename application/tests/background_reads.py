"""Cold background decoding is paced; selected decoding remains fast and identical."""
import json
import subprocess
import sys
import tempfile
import time
import wave
from pathlib import Path

with tempfile.TemporaryDirectory(prefix='audio-bandwidth-') as root:
    source = Path(root) / 'large.wav'
    with wave.open(str(source), 'wb') as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(48000)
        out.writeframes(b'\x00\x20' * 48000 * 32)
    def decode(limit, cache, background=False):
        start = time.monotonic()
        result = subprocess.run([sys.argv[1], '--worker', 'wave'], input=json.dumps(dict(
            path=str(source), library='test', relative='large.wav', local=str(Path(root) / cache),
            readBytesPerSecond=limit, background=background)) + '\n', capture_output=True, text=True, encoding='utf-8', timeout=15)
        assert result.returncode == 0, result.stderr
        events = [json.loads(line) for line in result.stdout.splitlines() if line.startswith('{')]
        complete = [e for e in events if e.get('event') == 'wave' and e.get('complete')]
        assert len(complete) == 1, events[-2:]
        return time.monotonic() - start, complete[0]['data']
    limited, slow_wave = decode(1048576, 'background')
    assert limited >= 2.5, ('background source reads exceeded budget', limited)
    fast, fast_wave = decode(0, 'selected')
    assert slow_wave == fast_wave, 'pacing changed waveform content'
    assert fast < limited / 2, ('selected track still throttled', fast, limited)
    unlimited, unlimited_wave = decode(0, 'unlimited-background', background=True)
    assert unlimited_wave == slow_wave, 'unlimited background changed waveform content'
    assert unlimited < limited / 2, ('unlimited background still throttled', unlimited, limited)
    cached, _ = decode(1048576, 'background')
    assert cached < limited / 2, ('cache hit unnecessarily throttled', cached, limited)
    print(json.dumps(dict(background_seconds=round(limited, 3), selected_seconds=round(fast, 3),
                          unlimited_background_seconds=round(unlimited, 3),
                          cached_seconds=round(cached, 3), unchanged_waveform=True)))
