"""Render the app mark as crisp PNG, Windows ICO and macOS ICNS assets.

Run with: python -m pip install Pillow && python assets/generate_icons.py
The committed images mean Pillow is not needed to build or run the app.
"""

from __future__ import annotations

import io
import struct
from pathlib import Path

from PIL import Image, ImageDraw


HERE = Path(__file__).resolve().parent
SIZES = (16, 24, 32, 48, 64, 128, 256)
MASTER_SIZE = 1024
SCALE = MASTER_SIZE / 256


def point(value: float) -> int:
    return round(value * SCALE)


def curve(p0, p1, p2, p3, steps=64):
    points = []
    for i in range(steps + 1):
        t = i / steps
        u = 1 - t
        x = u**3 * p0[0] + 3 * u * u * t * p1[0] + 3 * u * t * t * p2[0] + t**3 * p3[0]
        y = u**3 * p0[1] + 3 * u * u * t * p1[1] + 3 * u * t * t * p2[1] + t**3 * p3[1]
        points.append((point(x), point(y)))
    return points


def draw_curve(draw: ImageDraw.ImageDraw, control_points: tuple, width: int) -> None:
    points = curve(*control_points)
    draw.line(points, fill="white", width=width, joint="curve")
    radius = width / 2
    for x, y in (points[0], points[-1]):
        draw.ellipse((round(x - radius), round(y - radius),
                      round(x + radius), round(y + radius)), fill="white")


def make_master() -> Image.Image:
    mask = Image.new("L", (MASTER_SIZE, MASTER_SIZE))
    ImageDraw.Draw(mask).rounded_rectangle(
        (0, 0, MASTER_SIZE - 1, MASTER_SIZE - 1), radius=point(60), fill=255
    )
    top = (0x39, 0x84, 0xFA)
    bottom = (0x23, 0x47, 0xCA)
    # Keep the master artwork entirely opaque inside the rounded shape.
    gradient = Image.new("RGBA", (MASTER_SIZE, MASTER_SIZE))
    pixels = gradient.load()
    for y in range(MASTER_SIZE):
        for x in range(MASTER_SIZE):
            mix = (x + y) / (2 * (MASTER_SIZE - 1))
            pixels[x, y] = tuple(round(a * (1 - mix) + b * mix)
                                 for a, b in zip(top, bottom)) + (255,)
    gradient.putalpha(mask)
    draw = ImageDraw.Draw(gradient)
    draw.ellipse((point(59), point(111), point(93), point(145)), fill="white")
    draw_curve(draw, ((112, 88), (150, 110), (150, 146), (112, 168)), point(18))
    draw_curve(draw, ((143, 62), (207, 95), (207, 161), (143, 194)), point(18))
    return gradient


def main() -> None:
    master = make_master()
    images = {
        size: master.resize((size, size), Image.Resampling.LANCZOS)
        for size in SIZES
    }
    for size, img in images.items():
        img.save(HERE / f"icon_{size}.png", optimize=True)

    # An ICO can contain PNG frames; unlike a single-size bitmap this stays
    # crisp in Windows Explorer, Start and the title bar.
    frames = []
    for size, img in images.items():
        buffer = io.BytesIO()
        img.save(buffer, format="PNG", optimize=True)
        frames.append((size, buffer.getvalue()))
    header_size = 6 + 16 * len(frames)
    offset = header_size
    with (HERE / "YuanAudio.ico").open("wb") as output:
        output.write(struct.pack("<HHH", 0, 1, len(frames)))
        for size, data in frames:
            output.write(struct.pack("<BBBBHHII", size if size < 256 else 0,
                                     size if size < 256 else 0, 0, 0, 1, 32,
                                     len(data), offset))
            offset += len(data)
        for _, data in frames:
            output.write(data)

    # Modern ICNS files store PNG representations for each point/Retina size.
    # Generate this on any platform, so building the macOS bundle does not need
    # iconutil or Pillow installed on the Mac used for the release build.
    icns_sizes = ((16, b"icp4"), (32, b"icp5"), (64, b"icp6"),
                  (128, b"ic07"), (256, b"ic08"), (512, b"ic09"),
                  (1024, b"ic10"))
    chunks = []
    for size, kind in icns_sizes:
        image = master if size == MASTER_SIZE else master.resize(
            (size, size), Image.Resampling.LANCZOS)
        buffer = io.BytesIO()
        image.save(buffer, format="PNG", optimize=True)
        data = buffer.getvalue()
        chunks.append(kind + struct.pack(">I", len(data) + 8) + data)
    with (HERE / "YuanAudio.icns").open("wb") as output:
        output.write(b"icns" + struct.pack(">I", 8 + sum(map(len, chunks))))
        for chunk in chunks:
            output.write(chunk)


if __name__ == "__main__":
    main()
