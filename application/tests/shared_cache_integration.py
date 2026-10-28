"""Shared-cache attachment and six isolated workers. Uses temporary local fixtures only."""
from concurrent.futures import ThreadPoolExecutor
from contextlib import closing
import hashlib
import json
from pathlib import Path
import queue
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
import uuid
import wave

exe = Path(sys.argv[1]).resolve()

def brief(events):
    return [{k: v for k, v in event.items() if k != 'data'} for event in events]

def run(kind, config, success=True, timeout=35):
    result = subprocess.run([str(exe), '--worker', kind], input=json.dumps(config)+'\n',
                            text=True, encoding='utf-8', capture_output=True, timeout=timeout)
    events = [json.loads(line) for line in result.stdout.splitlines() if line.startswith('{')]
    assert (result.returncode == 0) == success, (kind, brief(events), result.stderr[-500:])
    return events

def identity(music, cache):
    events = run('register', dict(root=str(music), cache=str(cache)))
    return next(event['id'] for event in events if event['event'] == 'registered')

with tempfile.TemporaryDirectory(prefix='audio-shared-08-') as temporary:
    root = Path(temporary)
    music = root/'music'; music.mkdir()
    source = music/'whole.wav'
    with wave.open(str(source), 'wb') as target:
        target.setnchannels(1); target.setsampwidth(2); target.setframerate(48000)
        target.writeframes(b'\x10\x20' * 48000 * 4)
    original = hashlib.sha256(source.read_bytes()).hexdigest()
    unwritable = root/'not-a-directory'; unwritable.write_text('keep')
    write_failure = dict(path=str(source), relative=source.name, library='write-failure-client',
                         sharedLibrary=str(uuid.uuid4()), local=str(root/'write-failure-local'),
                         shared=str(unwritable))
    for expected_source in ('波形已生成', '本机波形缓存'):
        failed = run('wave', write_failure, False)
        assert any(e.get('complete') and e.get('stored') and e.get('source') == expected_source for e in failed), failed
        assert any(e.get('event') == 'error' and e.get('kind') == 'shared_cache_write' for e in failed), failed
    assert unwritable.read_text() == 'keep'
    local_id = str(uuid.uuid4())
    database = root/'index.sqlite'
    run('scan', dict(root=str(music), library=local_id, database=str(database)))
    with closing(sqlite3.connect(database)) as db:
        db.execute("UPDATE tracks SET favorite=1,tags='personal'"); db.commit()
        before = db.execute('SELECT id,library,relative,favorite,tags FROM tracks').fetchall()
        db.execute('ALTER TABLE libraries DROP COLUMN shared_id')
        db.execute('PRAGMA user_version=3'); db.commit()
    shared = music/'音频缓存'
    attach_barrier = threading.Barrier(6)
    def attach(client):
        attach_barrier.wait(timeout=5)
        return run('attach-cache', dict(database=str(database), library=local_id, cache=str(shared)))
    with ThreadPoolExecutor(max_workers=6) as pool:
        attachments = list(pool.map(attach, range(6)))
    attached = attachments[0]
    event = next(e for e in attached if e['event'] == 'cache-attached')
    assert event['id'] == local_id and event['sharedId'] != local_id
    shared_id = event['sharedId']
    assert {e['sharedId'] for events in attachments for e in events if e['event'] == 'cache-attached'} == {shared_id}
    with closing(sqlite3.connect(database)) as db:
        assert db.execute('SELECT id,library,relative,favorite,tags FROM tracks').fetchall() == before
        assert db.execute('SELECT shared_id FROM libraries WHERE id=?', (local_id,)).fetchone() == (shared_id,)
    with closing(sqlite3.connect(root/'index.pre-0.8.sqlite')) as backup:
        assert backup.execute('PRAGMA user_version').fetchone() == (3,)
        assert backup.execute('SELECT id,library,relative,favorite,tags FROM tracks').fetchall() == before
        assert 'shared_id' not in {c[1] for c in backup.execute('PRAGMA table_info(libraries)')}
    manifest = (shared/'library.json').read_bytes()
    run('attach-cache', dict(database=str(database), library=local_id, cache=str(shared)))
    assert (shared/'library.json').read_bytes() == manifest
    for invalid in (music, root):
        run('attach-cache', dict(database=str(database), library=local_id, cache=str(invalid)), False)
    occupied = music/'occupied'; occupied.mkdir(); (occupied/'keep.txt').write_text('keep')
    run('attach-cache', dict(database=str(database), library=local_id, cache=str(occupied)), False)
    assert (occupied/'keep.txt').read_text() == 'keep'

    def config(client, cache=shared, shared_identity=shared_id, audio=source, rate=0):
        return dict(path=str(audio), relative=audio.name, library=f'local-client-{client}',
                    sharedLibrary=shared_identity, local=str(root/f'client-{client}'),
                    shared=str(cache), readBytesPerSecond=rate)
    barrier = threading.Barrier(6)
    def generate(client):
        barrier.wait(timeout=5)
        return run('wave', config(client))
    with ThreadPoolExecutor(max_workers=6) as pool:
        groups = list(pool.map(generate, range(6)))
    completed = [e for events in groups for e in events if e.get('complete')]
    assert len(completed) == 6 and len({e['data'] for e in completed}) == 1
    assert sum(e.get('source') == '波形已生成' for e in completed) == 1, [brief(events) for events in groups]
    # Accurate existing local content must populate a newly attached shared folder without decode.
    fresh = root/'fresh-shared'; fresh_id = identity(music, fresh)
    published = run('wave', config(0, fresh, fresh_id))
    assert any(e.get('source') == '本机波形缓存' for e in published), published
    reused = run('wave', config(99, fresh, fresh_id))
    assert any(e.get('source') == '共享波形缓存' for e in reused), reused
    mirror = root/'other-mount'; mirror.mkdir()
    mirrored_source = mirror/source.name; shutil.copy2(source, mirrored_source)
    independent_path = run('wave', config('other-mount', fresh, fresh_id, mirrored_source))
    assert any(e.get('source') == '共享波形缓存' for e in independent_path), independent_path
    assert not any(e.get('event') in ('reading', 'duration') for e in independent_path)

    portable = root/'portable-shared'; portable_id = identity(music, portable)
    stat = source.stat()
    publish = config(0, portable, portable_id)
    publish.update(operation='publish_existing',
                   indexedSize=stat.st_size, indexedModified=stat.st_mtime_ns // 1_000_000)
    hidden_source = music/'temporarily-offline-original.tmp'
    source.rename(hidden_source)
    try:
        events = run('wave', publish)
    finally:
        hidden_source.rename(source)
    assert any(e.get('source') == '共享波形已同步' and e.get('published') for e in events)
    assert not any(e.get('event') in ('reading', 'duration') for e in events), events
    assert not (portable/'peaks-v1').exists(), 'portable data must not claim a source fingerprint'
    consumer = run('wave', config(200, portable, portable_id))
    assert any(e.get('source') == '共享波形直读缓存' for e in consumer), consumer
    assert not (root/'client-200'/'peaks-v1').exists(), 'consumer must not promote a portable view'
    # A source with a different timestamp cannot reuse a view for the prior indexed version.
    import os
    os.utime(source, ns=(stat.st_atime_ns, stat.st_mtime_ns + 2_000_000_000))
    changed = run('wave', config(201, portable, portable_id))
    assert any(e.get('source') == '波形已生成' for e in changed), changed
    os.utime(source, ns=(stat.st_atime_ns, stat.st_mtime_ns))
    reference_local = root/'reference-client'
    shutil.copytree(root/'client-0'/'views-v1', reference_local/'views-v1')
    for view in reference_local.rglob('*.view'):
        data = view.read_bytes()
        view.write_bytes(bytes.fromhex('41575632') + data[4:])
    reference_cache = root/'reference-shared'; reference_id = identity(music, reference_cache)
    reference = dict(publish, local=str(reference_local), shared=str(reference_cache), sharedLibrary=reference_id)
    skipped = run('wave', reference)
    assert any(e.get('approximate') and e.get('published') is False for e in skipped), skipped
    assert not (reference_cache/'portable-v1').exists(), 'approximate data must stay local'

    long_source = music/'long.wav'
    with wave.open(str(long_source), 'wb') as target:
        target.setnchannels(1); target.setsampwidth(2); target.setframerate(48000)
        target.writeframes(b'\x10\x20' * 4_000_000)
    crash_cache = root/'crash-shared'; crash_id = identity(music, crash_cache)
    owner = subprocess.Popen([str(exe), '--worker', 'wave'], stdin=subprocess.PIPE,
                             stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                             text=True, encoding='utf-8')
    inbox = queue.Queue()
    def read_owner():
        for line in owner.stdout:
            if line.startswith('{'): inbox.put(json.loads(line))
    threading.Thread(target=read_owner, daemon=True).start()
    owner.stdin.write(json.dumps(config('owner', crash_cache, crash_id, long_source, 1048576))+'\n')
    owner.stdin.close()
    try:
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            event = inbox.get(timeout=max(.1, deadline-time.monotonic()))
            if event.get('event') == 'reading': break
        else: raise AssertionError('owner did not start actual decoding')
    finally:
        owner.kill(); owner.wait(timeout=5)
    started = time.monotonic()
    with ThreadPoolExecutor(max_workers=6) as pool:
        recovered = list(pool.map(lambda client: run('wave', dict(config(f'recover-{client}', crash_cache,
                                                     crash_id, long_source, 0), background=True)), range(6)))
    elapsed = time.monotonic() - started
    assert elapsed < 18, elapsed
    final = [e for events in recovered for e in events if e.get('complete')]
    assert len(final) == 6 and len({e['data'] for e in final}) == 1
    assert all(any(e.get('event') == 'waiting' for e in events) for events in recovered)
    assert all(any(e.get('event') == 'waiting' and e.get('limitMs') == 60000 for e in events)
               for events in recovered), 'unlimited background must retain background lease coordination'
    assert hashlib.sha256(source.read_bytes()).hexdigest() == original
    print(json.dumps(dict(result='PASS', normal_clients=6, normal_decodes=1,
                          crash_recovery_clients=6, crash_recovery_seconds=round(elapsed, 3),
                          preserved_local_identity=True, local_to_shared_without_decode=True,
                          concurrent_migration_clients=6, portable_offline_publication=True,
                          portable_not_promoted=True, shared_write_failure_retryable=True,
                          shared_keys_independent_of_mount=True)))
