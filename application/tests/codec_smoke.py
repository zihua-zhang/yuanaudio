"""Optional codec check; imageio-ffmpeg is a development fixture generator only."""
import json
import base64
import struct
from pathlib import Path
import subprocess
import sys
import tempfile
import imageio_ffmpeg

executable=Path(sys.argv[1]).resolve()
generator=imageio_ffmpeg.get_ffmpeg_exe()
formats=[('wav',['-c:a','pcm_s24le']),('mp3',['-c:a','libmp3lame','-q:a','3']),('flac',['-c:a','flac']),('m4a',['-c:a','aac']),('ogg',['-c:a','libvorbis']),('aiff',['-c:a','pcm_s16be']),('aac',['-c:a','aac']),('opus',['-c:a','libopus']),('wma',['-c:a','wmav2'])]
with tempfile.TemporaryDirectory(prefix='audio-library-codecs-') as directory:
    root=Path(directory)
    for extension,options in formats:
        source=root/f'雨夜 测试.{extension}'
        subprocess.run([generator,'-hide_banner','-loglevel','error','-f','lavfi','-i','sine=frequency=440:sample_rate=48000:duration=1.2','-ac','2',*options,str(source)],check=True,timeout=20)
        config={'path':str(source),'relative':source.name,'library':'codec-test','local':str(root/'cache'),'shared':''}
        result=subprocess.run([str(executable),'--worker','wave'],input=json.dumps(config,ensure_ascii=False)+'\n',text=True,encoding='utf-8',capture_output=True,timeout=20)
        events=[json.loads(line) for line in result.stdout.splitlines() if line.startswith('{')]
        assert result.returncode==0 and any(e.get('event')=='wave' and e.get('complete') for e in events),(extension,events,result.stderr)
        full=next(e for e in events if e.get('complete'))
        data=base64.b64decode(full['data'])
        _,_,rate,channels,frames,stride,count=struct.unpack('<IIiiqqi',data[:36])
        assert channels==2 and abs(frames/rate-1.2)<0.1,(extension,rate,channels,frames)
        assert not full.get('approximate'), (extension, events)
        print(f'PASS: {extension}')
