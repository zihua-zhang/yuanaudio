"""Album art must not truncate or suppress audio; fixtures are synthetic."""
import base64
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import wave as wav_module

exe = Path(sys.argv[1]).resolve()
fixture = Path(__file__).parent / 'fixtures' / 'sine-with-cover.mp3'
before = hashlib.sha256(fixture.read_bytes()).digest()

with tempfile.TemporaryDirectory(prefix='audio-decoder-regression-') as directory:
    root = Path(directory)
    # Exercise Unicode and spaces in the native file path too.
    source = root / '带封面的 音乐.mp3'
    source.write_bytes(fixture.read_bytes())
    config = dict(path=str(source), library='decoder-regression', relative=source.name,
                  local=str(root / 'cache'), shared='')
    def run():
        result = subprocess.run([str(exe), '--worker', 'wave'],
                                input=json.dumps(config, ensure_ascii=False)+'\n',
                                capture_output=True, text=True, encoding='utf-8', timeout=20)
        events = [json.loads(line) for line in result.stdout.splitlines() if line.startswith('{')]
        return result, events

    result, events = run()
    assert result.returncode == 0, (events, result.stderr[-1000:])
    full = [e for e in events if e.get('event') == 'wave' and e.get('complete')]
    assert len(full) == 1, events
    wave = base64.b64decode(full[0]['data'])
    magic, version, rate, channels, frames, stride, count = struct.unpack('<IIiiqqi', wave[:36])
    assert magic == 0x31564C41 and version == 1
    assert channels == 1 and abs(frames / rate - 2.0) < 0.06, (rate, channels, frames)
    assert hashlib.sha256(wave[:-32]).digest() == wave[-32:]
    peaks = struct.unpack('<'+'h'*(count*channels*2), wave[36:-32])
    assert min(peaks) < -1000 and max(peaks) > 1000, 'silent or missing audio'
    assert hashlib.sha256(source.read_bytes()).digest() == before, 'source changed'

    # Real libraries can contain MP3s with a corrupt trailing packet. Show the usable
    # waveform with a warning. A labelled local reference may be saved, but it
    # must never be published as a normal/shared complete waveform.
    damaged = root / 'trailing-junk.mp3'
    damaged.write_bytes(fixture.read_bytes() + b'junk'*1024)
    config.update(path=str(damaged), relative=damaged.name, local=str(root/'damaged-cache'))
    result, events = run()
    assert result.returncode == 0, events
    assert any(e.get('complete') and e.get('approximate') for e in events), events
    assert any(e.get('event') == 'warning' for e in events), events
    assert not list((root/'damaged-cache').rglob('*.json'))
    assert list((root/'damaged-cache').rglob('*.view'))
    assert any(e.get('approximate') and e.get('stored') for e in events)

    truncated = root/'truncated.wav'
    with wav_module.open(str(truncated), 'wb') as out:
        out.setnchannels(1); out.setsampwidth(2); out.setframerate(48000)
        out.writeframes(struct.pack('<h', 2000)*144000)
    truncated.write_bytes(truncated.read_bytes()[:50000])
    config.update(path=str(truncated), relative=truncated.name, local=str(root/'truncated-cache'))
    result, events = run()
    assert result.returncode == 0, events
    assert any(e.get('complete') and e.get('approximate') for e in events), [e.get('source') for e in events]
    assert any(e.get('event') == 'warning' for e in events), events
    assert not list((root/'truncated-cache').rglob('*.json'))
    assert list((root/'truncated-cache').rglob('*.view'))

    # A misleading extension must fail once and must not publish a complete waveform.
    broken = root / 'broken.mp3'
    broken.write_bytes(b'not audio\x00' * 100)
    config.update(path=str(broken), relative=broken.name, local=str(root / 'broken-cache'))
    result, events = run()
    assert result.returncode != 0
    assert len([e for e in events if e.get('event') == 'error']) == 1, events
    assert not any(e.get('complete') for e in events), events
    assert not list((root / 'broken-cache').rglob('*.json'))
print('PASS: MP3 with embedded cover, full duration, non-silent peaks, Unicode path, source unchanged, corrupt input fails safely')
