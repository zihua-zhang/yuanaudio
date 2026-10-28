"""End-to-end checks against the real child processes; no NAS or user media."""
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import threading
import queue
import time
import sqlite3
from contextlib import closing
import wave

exe = Path(sys.argv[1]).resolve()

def run(kind, config, success=True):
    result = subprocess.run([str(exe), '--worker', kind], input=json.dumps(config, ensure_ascii=False)+'\n', capture_output=True, text=True, encoding='utf-8', timeout=30)
    events = [json.loads(line) for line in result.stdout.splitlines() if line.startswith('{')]
    assert (result.returncode == 0) == success, (kind, result.returncode, events, result.stderr[-1000:])
    return events

with tempfile.TemporaryDirectory(prefix='audio-library-integration-') as temp:
    folder = Path(temp)
    music = folder/'音乐'/'伤感'; music.mkdir(parents=True)
    source = music/'雨夜.wav'
    with wave.open(str(source), 'wb') as out:
        out.setnchannels(2); out.setsampwidth(2); out.setframerate(48000)
        out.writeframes(b''.join(struct.pack('<hh', int(12000*math.sin(i*2*math.pi*440/48000)), -int(12000*math.sin(i*2*math.pi*440/48000))) for i in range(96000)))
    original = source.read_bytes()
    reg = run('register', {'root':str(music.parent), 'cache':str(folder/'shared')})
    assert any(e.get('event')=='registered' for e in reg), ('missing registration', reg)
    library = next(e['id'] for e in reg if e['event']=='registered')
    database = folder/'index.sqlite'
    (music.parent/'空目录'/'子目录').mkdir(parents=True)
    scan = run('scan', {'root':str(music.parent), 'library':library, 'database':str(database)})
    assert any(e.get('event')=='complete' and e.get('count')==1 for e in scan)
    run('scan', {'root':str(folder/'disconnected'), 'library':library, 'database':str(database)}, success=False)
    with closing(sqlite3.connect(database)) as db:
        assert db.execute('select relative from tracks').fetchall()==[('伤感/雨夜.wav',)], 'offline scan removed records'
        assert db.execute("select path from folders where path='空目录/子目录'").fetchall()==[('空目录/子目录',)], 'empty folders must be indexed'
    reg2 = run('register', {'root':str(music.parent), 'cache':str(folder/'shared')})
    assert next(e['id'] for e in reg2 if e['event']=='registered')==library
    cfg = {'path':str(source), 'relative':'伤感/雨夜.wav', 'library':library, 'local':str(folder/'client-a'), 'shared':str(folder/'shared')}
    first = run('wave', cfg)
    ready = next(e for e in first if e.get('event')=='wave' and e.get('complete'))
    assert ready['source']=='波形已生成'
    second = run('wave', cfg)
    assert any(e.get('source')=='本机波形缓存' for e in second)
    cfg['local']=str(folder/'client-b')
    third=run('wave',cfg)
    assert any(e.get('source')=='共享波形缓存' for e in third)
    for p in (folder/'shared').rglob('*.wave'): p.write_bytes(b'corrupt')
    cfg['local']=str(folder/'client-c')
    repaired=run('wave',cfg)
    assert any(e.get('source')=='波形已生成' for e in repaired)
    assert source.read_bytes()==original, 'original audio was modified'
    player = subprocess.Popen([str(exe),'--worker','player'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,encoding='utf-8')
    inbox=queue.Queue()
    def read_events():
        for line in player.stdout:
            if line.startswith('{'): inbox.put(json.loads(line))
    threading.Thread(target=read_events,daemon=True).start()
    def command(**data):
        player.stdin.write(json.dumps(data,ensure_ascii=False)+'\n'); player.stdin.flush()
    def until(predicate, seconds=6):
        deadline=time.monotonic()+seconds
        while time.monotonic()<deadline:
            try: event=inbox.get(timeout=max(0.01,deadline-time.monotonic()))
            except queue.Empty: break
            if predicate(event): return event
        raise AssertionError('player did not reach expected state')
    try:
        command(op='load',path=str(source),generation=1,autoplay=True,volume=0)
        until(lambda e:e.get('event')=='position' and e.get('generation')==1 and e.get('position',0)>100)
        command(op='pause',generation=1)
        until(lambda e:e.get('event')=='state' and e.get('playing') is False)
        command(op='seek',generation=1,position=1200)
        until(lambda e:e.get('event')=='position' and e.get('position',0)>=1200)
        command(op='load',path=str(source),generation=2,autoplay=False,volume=0)
        until(lambda e:e.get('event')=='position' and e.get('generation')==2 and e.get('duration')==2000)
        command(op='play',generation=1)  # stale command must not start the new selection
        time.sleep(.35)  # Paused workers intentionally emit no periodic position reports.
        while not inbox.empty():
            observed=inbox.get_nowait()
            if observed.get('generation')==2:
                assert not (observed.get('event')=='state' and observed.get('playing')), observed
                if observed.get('event')=='position': assert observed['position']==0, observed
        command(op='play',generation=2)
        until(lambda e:e.get('event')=='position' and e.get('generation')==2 and e.get('position',0)>100)
        # An offline selection must recover via Play without switching to another row.
        returning = folder/'reconnected.wav'
        command(op='load',path=str(returning),generation=3,autoplay=True,volume=0)
        until(lambda e:e.get('event')=='error' and e.get('generation')==3)
        returning.write_bytes(original)
        command(op='play',generation=3)
        until(lambda e:e.get('event')=='position' and e.get('generation')==3 and e.get('position',0)>100)
        covered = Path(__file__).parent/'fixtures'/'sine-with-cover.mp3'
        command(op='load',path=str(covered.resolve()),generation=4,autoplay=True,volume=0)
        until(lambda e:e.get('event')=='position' and e.get('generation')==4 and e.get('position',0)>200)
    finally:
        player.stdin.close()
        try: player.wait(timeout=5)
        except subprocess.TimeoutExpired: player.kill(); player.wait()
    print('PASS: registration, scanner/offline retention, real stereo decode, local/shared reuse, corrupt-cache recovery, original preserved, playback/pause/seek/switch/stale command')
