"""rawimg.py png RAW OUT.png [W H] | diff RAW_A RAW_B [W H]: bbport's present dumps (RGBA8, W*H*4)."""
import struct
import sys
import zlib


def load(path, w, h):
    data = open(path, "rb").read()
    assert len(data) == w * h * 4, f"{path}: {len(data)} bytes, not {w}x{h}x4"
    return data


def write_png(path, data, w, h, step=1):
    ow, oh = w // step, h // step
    raw = bytearray()
    for y in range(oh):
        raw.append(0)
        row = data[(y * step) * w * 4:(y * step + 1) * w * 4]
        for x in range(ow):
            p = row[x * step * 4:x * step * 4 + 3]
            raw += p
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", ow, oh, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b"")
    open(path, "wb").write(png)


def diff(a, b):
    n = len(a) // 4
    total = 0
    big = 0
    for i in range(0, len(a), 4):
        d = max(abs(a[i] - b[i]), abs(a[i + 1] - b[i + 1]), abs(a[i + 2] - b[i + 2]))
        total += d
        big += d > 32
    return total / n, 100.0 * big / n


if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "png":
        w, h = (int(sys.argv[4]), int(sys.argv[5])) if len(sys.argv) > 5 else (1680, 1050)
        write_png(sys.argv[3], load(sys.argv[2], w, h), w, h, step=2)
    else:
        w, h = (int(sys.argv[4]), int(sys.argv[5])) if len(sys.argv) > 5 else (1680, 1050)
        mean, big = diff(load(sys.argv[2], w, h), load(sys.argv[3], w, h))
        print(f"mean max-channel difference {mean:.2f}/255, pixels differing by >32: {big:.2f}%")
