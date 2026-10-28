"""Known non-audio trailers preserve exact gapless samples; arbitrary damage does not."""
import base64, hashlib, json, struct, subprocess, sys, tempfile
from pathlib import Path
exe=Path(sys.argv[1]).resolve()
clean=(Path(__file__).parent/'fixtures/sine-with-cover.mp3').read_bytes()
def ape(value, header=False):
    item=struct.pack('<II',len(value),0)+b'Title\0'+value
    footer=b'APETAGEX'+struct.pack('<IIII',2000,len(item)+32,1,0x80000000 if header else 0)+bytes(8)
    start=footer[:20]+struct.pack('<I',0xa0000000)+bytes(8) if header else b''
    return start+item+footer
lyrics=b'LYRICSBEGINETT00005Title'
lyrics+=f'{len(lyrics):06d}'.encode()+b'LYRICS200'
id3=b'TAG'+bytes(125)
fixtures={'clean':clean,'ape':clean+ape(b'title'),'ape-header':clean+ape(b'title',True),
          'large-ape':clean+ape(b'a'*262144),'lyrics':clean+lyrics,'stack':clean+ape(b'title')+lyrics+id3,
          'interior-damage':clean[:9000]+bytes(100)+clean[9100:]+ape(b'title'),
          'unknown-tail':clean+b'junk'*1024}
with tempfile.TemporaryDirectory(prefix='mp3-tag-integrity-') as directory:
    root=Path(directory); baseline=None
    for name,data in fixtures.items():
        path=root/(name+'.mp3');path.write_bytes(data)
        config=dict(path=str(path),library='qa',relative=path.name,local=str(root/(name+'-cache')),shared='')
        proc=subprocess.run([str(exe),'--worker','wave'],input=json.dumps(config)+'\n',capture_output=True,text=True,encoding='utf-8',timeout=20)
        events=[json.loads(line) for line in proc.stdout.splitlines() if line.startswith('{')]
        complete=[e for e in events if e.get('event')=='wave' and e.get('complete')]
        assert hashlib.sha256(path.read_bytes()).digest()==hashlib.sha256(data).digest()
        if name in ['interior-damage','unknown-tail']:
            assert proc.returncode!=0 or (complete and complete[-1]['approximate']), (name,events,proc.stderr)
        else:
            assert proc.returncode==0 and len(complete)==1 and not complete[-1]['approximate'], (name,events,proc.stderr)
            wave=base64.b64decode(complete[-1]['data'])
            if baseline is None:baseline=wave
            assert wave==baseline, (name,'waveform or gapless sample count changed')
        print('PASS',name)
