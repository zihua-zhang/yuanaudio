"""Install pinned FFmpeg headers; the Windows runtime is already supplied by Qt.

Only public API headers are compiled. No FFmpeg program or GPL build is bundled.
"""
import hashlib
from pathlib import Path
import tarfile
import urllib.request

root = Path(__file__).resolve().parents[1]
version = '7.1.1'
digest = '733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1'
archive = root / '.tools' / f'ffmpeg-{version}.tar.xz'
archive.parent.mkdir(parents=True, exist_ok=True)
if not archive.exists() or hashlib.sha256(archive.read_bytes()).hexdigest() != digest:
    with urllib.request.urlopen(f'https://ffmpeg.org/releases/ffmpeg-{version}.tar.xz', timeout=60) as response:
        data = response.read()
    if hashlib.sha256(data).hexdigest() != digest:
        raise RuntimeError('FFmpeg source archive checksum mismatch')
    archive.write_bytes(data)
include = root / '.tools' / 'ffmpeg-headers' / 'include'
with tarfile.open(archive) as tar:
    for member in tar.getmembers():
        parts = Path(member.name).parts
        if len(parts) != 3 or parts[1] not in ('libavcodec', 'libavformat', 'libavutil') or not parts[2].endswith('.h') or not member.isfile():
            continue
        destination = include / parts[1] / parts[2]
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(tar.extractfile(member).read())
# FFmpeg configure normally produces these two public architecture flags.
# Both supported targets (Windows x64 and macOS arm64) are little-endian with unaligned access.
(include / 'libavutil' / 'avconfig.h').write_text(
    '#ifndef AVUTIL_AVCONFIG_H\n#define AVUTIL_AVCONFIG_H\n'
    '#define AV_HAVE_BIGENDIAN 0\n#define AV_HAVE_FAST_UNALIGNED 1\n#endif\n', encoding='ascii')
print(f'Prepared FFmpeg {version} headers (ABI 61/61/59)')
