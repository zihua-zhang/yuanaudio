"""Optional longer playback stress with synthetic fixtures, muted output and no NAS."""
import json
from pathlib import Path
import queue
import subprocess
import sys
import threading
import time

exe = Path(sys.argv[1]).resolve()
source = Path(__file__).parent/'fixtures'/'sine-with-cover.mp3'
player = subprocess.Popen([str(exe), '--worker', 'player'], stdin=subprocess.PIPE,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding='utf-8')
events = queue.Queue()
def read_events():
    for line in player.stdout:
        if line.startswith('{'): events.put(json.loads(line))
def drain_errors():
    for _ in player.stderr: pass
threading.Thread(target=read_events, daemon=True).start()
threading.Thread(target=drain_errors, daemon=True).start()
def send(**message):
    player.stdin.write(json.dumps(message)+'\n'); player.stdin.flush()
started = time.monotonic()
try:
    for generation in range(1, 301):
        send(op='load', path=str(source), generation=generation, autoplay=True, volume=0)
        send(op='seek', position=500, generation=generation)
        send(op='pause', generation=generation-1)  # must not affect the newest media
        time.sleep(0.03)
    deadline = time.monotonic()+10
    reached = False
    latest_events = []
    while time.monotonic() < deadline:
        message = events.get(timeout=max(0.01, deadline-time.monotonic()))
        if message.get('event') == 'error':
            raise AssertionError(message)
        if message.get('generation') == 300:
            latest_events.append(message)
        if message.get('generation') == 300 and message.get('event') == 'position' and message.get('position',0) > 550:
            reached = True; break
    assert reached, ('newest track did not advance after 300 replacements', latest_events[-12:])
finally:
    player.stdin.close()
    try: player.wait(timeout=5)
    except subprocess.TimeoutExpired: player.kill(); player.wait(); raise AssertionError('player did not close')
assert player.returncode == 0
print(json.dumps(dict(switches=300, latest_track_advanced=True, clean_exit=True,
                      elapsed_seconds=round(time.monotonic()-started,2))))
