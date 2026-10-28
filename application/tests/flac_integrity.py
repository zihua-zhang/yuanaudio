"""Verify FLAC reference classification with generated, CRC-valid lossless fixtures.

No encoder executable or third-party Python module is required. Each audio frame
uses FLAC's verbatim subframes; STREAMINFO contains the MD5 of canonical PCM.
"""
import base64
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


def crc(data, width, polynomial):
    value = 0
    mask = (1 << width) - 1
    for byte in data:
        value ^= byte << (width - 8)
        for _ in range(8):
            value = ((value << 1) ^ (polynomial if value & (1 << (width - 1)) else 0)) & mask
    return value


def pcm_samples(bits, channels, count):
    limits = [-(1 << (bits - 1)), -12345, -1, 0, 1, 12345, (1 << (bits - 1)) - 1]
    return [[limits[(frame * (channel + 1) + channel) % len(limits)]
             for channel in range(channels)] for frame in range(count)]


def canonical_pcm(samples, bits):
    return b''.join(value.to_bytes((bits + 7) // 8, 'little', signed=True)
                    for frame in samples for value in frame)


def flac(samples, bits, channels, digest=None, declared_frames=None):
    block = 4096
    assert len(samples) % block == 0
    rate = 48000
    packed = ((rate << 44) | ((channels - 1) << 41) | ((bits - 1) << 36)
              | (len(samples) if declared_frames is None else declared_frames))
    md5 = hashlib.md5(canonical_pcm(samples, bits)).digest() if digest is None else digest
    streaminfo = struct.pack('>HH', block, block) + bytes(6) + packed.to_bytes(8, 'big') + md5
    metadata = b'fLaC' + b'\x80\x00\x00\x22' + streaminfo
    frames = []
    for number, offset in enumerate(range(0, len(samples), block)):
        # Fixed block size, sample rate/depth inherited from STREAMINFO,
        # independent channels, one-byte UTF-8 frame number (<128).
        header = bytes([0xff, 0xf8, 0x70, (channels - 1) << 4, number]) + struct.pack('>H', block - 1)
        frame = bytearray(header + bytes([crc(header, 8, 0x07)]))
        for channel in range(channels):
            frame.append(0x02)  # Verbatim, no wasted bits.
            for sample in samples[offset:offset + block]:
                frame.extend(sample[channel].to_bytes((bits + 7) // 8, 'big', signed=True))
        frame.extend(crc(frame, 16, 0x8005).to_bytes(2, 'big'))
        frames.append(bytes(frame))
    return metadata, frames


def main():
    exe = Path(sys.argv[1]).resolve()
    trailer = bytes.fromhex('f000ff0f44444048463c360e55fff0')
    checks = 0
    with tempfile.TemporaryDirectory(prefix='audio-flac-integrity-') as directory:
        root = Path(directory)
        for bits, channels in [(16, 1), (16, 2), (24, 1), (24, 2)]:
            samples = pcm_samples(bits, channels, 12288)
            original_md5 = hashlib.md5(canonical_pcm(samples, bits)).digest()
            metadata, frames = flac(samples, bits, channels)
            clean = metadata + b''.join(frames)
            changed = [list(frame) for frame in samples]
            changed[5000][0] += 1
            altered_metadata, altered_frames = flac(changed, bits, channels, digest=original_md5)
            no_md5, _ = flac(samples, bits, channels, digest=bytes(16))
            wrong_md5, _ = flac(samples, bits, channels, digest=bytes([original_md5[0] ^ 1]) + original_md5[1:])
            unknown_count, _ = flac(samples, bits, channels, declared_frames=0)
            wrong_count, _ = flac(samples, bits, channels, declared_frames=len(samples) + 512)
            broken_pcm = bytearray(frames[1])
            broken_pcm[1000] ^= 1  # Real PCM mutation, deliberately retain the old CRC16.
            broken_crc = bytearray(frames[1])
            broken_crc[-1] ^= 1  # Only the CRC field changes; PCM and STREAMINFO MD5 remain valid.
            variants = [
                ('clean', clean, False, len(samples)),
                ('trailer', clean + trailer, False, len(samples)),
                ('altered-pcm', altered_metadata + b''.join(altered_frames), True, len(samples)),
                ('truncated', metadata + b''.join(frames[:-1]), True, 8192),
                ('wrong-md5', wrong_md5 + b''.join(frames), True, len(samples)),
                ('zero-md5-clean', no_md5 + b''.join(frames), False, len(samples)),
                ('zero-md5-trailer', no_md5 + b''.join(frames) + trailer, True, len(samples)),
                ('zero-md5-corrupt-pcm-crc', no_md5 + frames[0] + broken_pcm + frames[2], True, 8192),
                ('zero-md5-corrupt-crc-only', no_md5 + frames[0] + broken_crc + frames[2], True, 8192),
                ('valid-md5-corrupt-crc-only', metadata + frames[0] + broken_crc + frames[2], False, len(samples)),
                ('unknown-count-trailer', unknown_count + b''.join(frames) + trailer, True, len(samples)),
                ('wrong-count-trailer', wrong_count + b''.join(frames) + trailer, True, len(samples)),
            ]
            baseline = None
            for name, data, approximate, expected_frames in variants:
                case = f'{bits}bit-{channels}ch-{name}'
                path = root / (case + '.flac')
                path.write_bytes(data)
                before = hashlib.sha256(data).digest()
                config = dict(path=str(path), relative=path.name, library='flac-integrity',
                              local=str(root / (case + '-cache')), shared='')
                result = subprocess.run([str(exe), '--worker', 'wave'],
                                        input=json.dumps(config) + '\n', capture_output=True,
                                        text=True, encoding='utf8', timeout=20)
                events = [json.loads(line) for line in result.stdout.splitlines() if line.startswith('{')]
                full = [event for event in events if event.get('event') == 'wave' and event.get('complete')]
                assert result.returncode == 0 and len(full) == 1, (case, events, result.stderr)
                assert full[0].get('approximate') is approximate, (case, full[0].get('approximate'), result.stderr)
                wave = base64.b64decode(full[0]['data'])
                _, _, rate, wave_channels, decoded_frames, _, _ = struct.unpack('<IIiiqqi', wave[:36])
                assert (rate, wave_channels, decoded_frames) == (48000, channels, expected_frames), case
                if name == 'clean':
                    baseline = wave
                elif name in ('trailer', 'valid-md5-corrupt-crc-only'):
                    assert wave == baseline, case
                if approximate:
                    assert not list((root / (case + '-cache')).rglob('*.json')), case
                assert hashlib.sha256(path.read_bytes()).digest() == before, case
                checks += 1
    print(f'PASS: {checks} FLAC integrity cases; 16/24-bit, mono/stereo, exact PCM MD5, changed PCM, '
          'truncation, trailing data, frame CRC errors, missing MD5 and unknown/mismatched sample counts')


if __name__ == '__main__':
    main()
