"""Real muted player: a waveform click must cue before producing playback."""
import json
import queue
import struct
import subprocess
import sys
import tempfile
import threading
import time
import wave
from pathlib import Path

with tempfile.TemporaryDirectory(prefix='audio-cue-') as root:
    source = Path(root) / 'cue.wav'
    with wave.open(str(source), 'wb') as audio:
        audio.setnchannels(1)
        audio.setsampwidth(2)
        audio.setframerate(48000)
        audio.writeframes(struct.pack('<h', 3000) * 48000 * 8)
    player = subprocess.Popen([sys.argv[1], '--worker', 'player'], stdin=subprocess.PIPE,
                              stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                              text=True, encoding='utf-8')
    events = queue.Queue()
    def reader():
        for line in player.stdout:
            if line.startswith('{'):
                events.put(json.loads(line))
    threading.Thread(target=reader, daemon=True).start()
    def send(**message):
        player.stdin.write(json.dumps(message) + '\n')
        player.stdin.flush()
    def until(predicate, timeout=6):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                item = events.get(timeout=max(.01, deadline - time.monotonic()))
            except queue.Empty:
                break
            assert item.get('event') != 'error', item
            if predicate(item):
                return item
        raise AssertionError('player did not reach expected state')
    try:
        send(op='load', generation=1, path=str(source), volume=0,
             autoplay=True, startRatio=.6)
        playing = until(lambda e: e.get('generation') == 1 and e.get('event') == 'state' and e.get('playing'))
        assert 4750 <= playing.get('position', -1) <= 4900, ('played before seeking', playing)
        first = until(lambda e: e.get('generation') == 1 and e.get('event') == 'position'
                      and e.get('position', 0) > 0)
        assert 4750 <= first['position'] <= 5300, ('started before clicked location', first)
        send(op='pause', generation=1)
        until(lambda e: e.get('event') == 'state' and not e.get('playing'))
        # A paused player must not keep waking the UI with position polling.
        time.sleep(.2)
        while not events.empty():
            events.get_nowait()
        time.sleep(.7)
        idle = []
        while not events.empty():
            idle.append(events.get_nowait())
        assert not any(e.get('event') == 'position' for e in idle), idle
        assert player.poll() is None
        send(op='cue', generation=1, ratio=.25)
        until(lambda e: e.get('event') == 'state' and e.get('playing'))
        pos = until(lambda e: e.get('event') == 'position' and 1950 <= e.get('position', 0) <= 2500)
        send(op='load', generation=2, path=str(source), volume=0, autoplay=False)
        send(op='cue', generation=2, ratio=.2)
        send(op='cue', generation=2, ratio=.8)
        send(op='cue', generation=1, ratio=.1)  # obsolete click cannot affect the new track
        latest = until(lambda e: e.get('generation') == 2 and e.get('event') == 'position'
                       and e.get('position', 0) > 0)
        assert 6350 <= latest['position'] <= 7000, ('latest click lost', latest)
        send(op='load', generation=3, path=str(source), volume=0, autoplay=True, startRatio=.7)
        send(op='pause', generation=3)
        time.sleep(.5)
        while not events.empty():
            e = events.get_nowait()
            assert not (e.get('generation') == 3 and e.get('event') == 'state' and e.get('playing')), e
    finally:
        player.stdin.close()
        try:
            player.wait(timeout=5)
        except subprocess.TimeoutExpired:
            player.kill()
            player.wait()
    assert player.returncode == 0
print('PASS: cued load, paused cue, latest pending click, stale generation, pending pause')
