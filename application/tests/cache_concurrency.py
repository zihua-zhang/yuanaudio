"""Ten independent clients must agree on identity and published waveform data."""
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import os
import time
import uuid

exe = Path(sys.argv[1]).resolve()
fixture = Path(__file__).parent / 'fixtures' / 'sine-with-cover.mp3'

def worker(kind, config):
    result = subprocess.run([str(exe), '--worker', kind], input=json.dumps(config)+'\n',
                            text=True, encoding='utf-8', capture_output=True, timeout=15)
    events = [json.loads(line) for line in result.stdout.splitlines() if line.startswith('{')]
    assert result.returncode == 0, (result.returncode, events, result.stderr[-500:])
    return events

with tempfile.TemporaryDirectory(prefix='audio-library-concurrency-') as temporary:
    root = Path(temporary)
    music = root/'music'; music.mkdir()
    source = music/'covered.mp3'; source.write_bytes(fixture.read_bytes())
    # A dead remote initializer must not permanently strand a new shared cache.
    stale = root/'stale-remote'; stale.mkdir()
    lock = stale/'.library-init.lock'
    lock.write_text('999999\nAudioLibrary\nqa-dead-remote-host\nqa-other-machine\nqa-other-boot\n')
    os.utime(lock, (time.time()-7*86400, time.time()-7*86400))
    (stale/'.library-init-ABC123.tmp').write_bytes(b'incomplete abandoned publication')
    recovered = worker('register', dict(root=str(music), cache=str(stale)))
    assert any(e.get('event') == 'registered' for e in recovered), recovered
    if os.name == 'nt':
        # Windows can briefly deny reads of an already visible atomic publication.
        # Hold our own valid file exclusively, then release it while registration waits.
        import ctypes
        from ctypes import wintypes
        busy = root/'busy-publication'; busy.mkdir()
        expected_id = str(uuid.uuid4())
        identity = busy/'library.json'
        identity.write_text(json.dumps({'format': 1, 'library_id': expected_id}), encoding='utf-8')
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                                      wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
        kernel.CreateFileW.restype = wintypes.HANDLE
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        handle = kernel.CreateFileW(str(identity), 0x80000000, 0, None, 3, 0x80, None)
        assert handle != wintypes.HANDLE(-1).value, ctypes.get_last_error()
        release = threading.Timer(.25, lambda: kernel.CloseHandle(handle))
        release.start()
        try:
            readable = worker('register', dict(root=str(music), cache=str(busy)))
            assert next(e['id'] for e in readable if e.get('event') == 'registered') == expected_id
        finally:
            release.join()
    with ThreadPoolExecutor(max_workers=10) as pool:
        for round_id in range(20):
            barrier = threading.Barrier(10)
            shared = root/f'shared-{round_id}'
            def register(client):
                barrier.wait(timeout=10)
                return worker('register', dict(root=str(music), cache=str(shared)))
            events = list(pool.map(register, range(10)))
            ids = {e['id'] for group in events for e in group if e.get('event') == 'registered'}
            assert len(ids) == 1, ids
        library = ids.pop()
        def decode(client):
            return worker('wave', dict(path=str(source), relative=source.name, library=library,
                                      local=str(root/f'client-{client}'), shared=str(shared)))
        results = list(pool.map(decode, range(10)))
        waves = {e['data'] for group in results for e in group if e.get('complete')}
        assert len(waves) == 1, 'clients disagree on waveform'
        reused = decode(11)
        assert any(e.get('source') == '共享波形缓存' for e in reused), reused
        assert source.read_bytes() == fixture.read_bytes(), 'source changed'
print('PASS: 200 simultaneous registrations, consistent identity, 10 concurrent wave clients, intact shared reuse')
