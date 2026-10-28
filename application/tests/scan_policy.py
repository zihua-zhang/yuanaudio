"""A cache nested in the music root is excluded before recursion; scans yield locally."""
import hashlib, json, os, sqlite3, subprocess, sys, tempfile, time, wave
from contextlib import closing
from pathlib import Path

exe = sys.argv[1]
def scan(root, database, **options):
    process = subprocess.Popen([exe, '--worker', 'scan'], stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, encoding='utf-8')
    process.stdin.write(json.dumps(dict(root=str(root), database=str(database), library=options.pop('library', 'qa'), **options))+'\n')
    process.stdin.flush()
    return process
with tempfile.TemporaryDirectory(prefix='audio-scan-policy-') as tmp:
    folder = Path(tmp); root = folder/'music'; root.mkdir()
    cache = root/'音频缓存'; (cache/'nested').mkdir(parents=True)
    for target in (root/'real.wav', cache/'fake.wave', cache/'nested'/'fake.wav'):
        with wave.open(str(target), 'wb') as out:
            out.setnchannels(1); out.setsampwidth(2); out.setframerate(8000); out.writeframes(b'\0'*100)
    process = scan(root, folder/'library.sqlite', excluded=[str(cache)])
    text = process.communicate(timeout=10)[0]
    events = [json.loads(line) for line in text.splitlines() if line.startswith('{')]
    assert process.returncode == 0, events
    assert next(e['count'] for e in events if e['event']=='complete') == 1, events
    control = folder/'control.json'; control.write_text(json.dumps({'paused':True,'entriesPerSecond':400}))
    process = scan(root, folder/'library.sqlite', excluded=[str(cache)], control=str(control))
    try:
        time.sleep(.5)
        assert process.poll() is None, 'scan ignored initial pause'
        staged = control.with_suffix('.tmp'); staged.write_text(json.dumps({'paused':False,'entriesPerSecond':400}))
        os.replace(staged, control)
        text = process.communicate(timeout=10)[0]
        events = [json.loads(line) for line in text.splitlines() if line.startswith('{')]
        assert any(e['event']=='deferred' and e['paused'] for e in events), events
        assert any(e['event']=='complete' and e['count']==1 for e in events), events
    finally:
        if process.poll() is None: process.kill(); process.wait()
    excluded_folder = '排除 🎵100%_'
    for relative in (excluded_folder+'/one.wav', excluded_folder+'/deep/two.wav',
                     excluded_folder+'more/keep.wav', 'skip.wav', 'keep.wav'):
        target = root/relative; target.parent.mkdir(parents=True, exist_ok=True)
        with wave.open(str(target), 'wb') as out:
            out.setnchannels(1); out.setsampwidth(2); out.setframerate(8000); out.writeframes(b'\x10\x20'*50)
    original = {p: hashlib.sha256(p.read_bytes()).hexdigest() for p in root.rglob('*') if p.is_file()}
    database = folder/'library.sqlite'
    for library in ('qa', 'other'):
        process = scan(root, database, library=library, excluded=[str(cache)])
        text = process.communicate(timeout=10)[0]
        assert process.returncode == 0, text
    with closing(sqlite3.connect(database)) as db:
        db.execute("INSERT INTO catalog_exclusions VALUES(?,?,'folder')", ('qa', excluded_folder))
        db.execute("INSERT INTO catalog_exclusions VALUES(?,?,'track')", ('qa', 'skip.wav'))
        # Clear only QA rows, then let the real worker rebuild with its persisted exclusion rules.
        db.execute("DELETE FROM search_fts WHERE rowid IN (SELECT id FROM tracks WHERE library='qa')")
        db.execute("DELETE FROM tracks WHERE library='qa'")
        db.execute("DELETE FROM folder_fts WHERE rowid IN (SELECT id FROM folders WHERE library='qa')")
        db.execute("DELETE FROM folders WHERE library='qa'")
        db.commit()
    process = scan(root, database, excluded=[str(cache)])
    text = process.communicate(timeout=10)[0]
    assert process.returncode == 0, text
    events = [json.loads(line) for line in text.splitlines() if line.startswith('{')]
    assert any(e.get('event') == 'complete' and e.get('count') == 3 for e in events), events
    with closing(sqlite3.connect(database)) as db:
        actual = {row[0] for row in db.execute("SELECT relative FROM tracks WHERE library='qa'")}
        assert actual == {'real.wav', 'keep.wav', excluded_folder+'more/keep.wav'}, actual
        assert db.execute("SELECT count(*) FROM tracks WHERE library='other'").fetchone() == (6,)
        assert not any(row[0] == excluded_folder or row[0].startswith(excluded_folder+'/')
                       for row in db.execute("SELECT path FROM folders WHERE library='qa'"))
        db.execute("DELETE FROM search_fts WHERE rowid IN (SELECT id FROM tracks WHERE library='qa')")
        db.execute("DELETE FROM tracks WHERE library='qa'")
        db.execute("DELETE FROM folder_fts WHERE rowid IN (SELECT id FROM folders WHERE library='qa')")
        db.execute("DELETE FROM folders WHERE library='qa'")
        db.execute("DELETE FROM catalog_exclusions WHERE library='qa'")
        db.execute("DELETE FROM libraries WHERE id='qa'")
        db.execute("INSERT INTO removed_libraries VALUES('qa')")
        db.commit()
    process = scan(root, database, excluded=[str(cache)])
    text = process.communicate(timeout=10)[0]
    assert process.returncode != 0, 'a stale scanner resurrected the removed library'
    with closing(sqlite3.connect(database)) as db:
        assert db.execute("SELECT count(*) FROM libraries WHERE id='qa'").fetchone() == (0,)
        assert db.execute("SELECT count(*) FROM tracks WHERE library='other'").fetchone() == (6,)
    assert all(p.exists() and hashlib.sha256(p.read_bytes()).hexdigest() == digest
               for p, digest in original.items()), 'catalog changes modified original audio'
print('PASS: cache pruning, scan pause/resume, persistent catalog exclusions, removed-library guard, original files unchanged')
