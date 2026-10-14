"""Convert a 24-bit BMP (ctr_pc screenshot) to PNG, optionally downscaled by an integer factor.
usage: python bmp2png.py in.bmp out.png [scale]"""
import struct
import sys
import zlib


def main(src, dst, scale=1):
    b = open(src, "rb").read()
    off, = struct.unpack_from("<I", b, 10)
    w, h = struct.unpack_from("<ii", b, 18)
    row = (w * 3 + 3) & ~3
    out = bytearray()
    for y in range(0, h, scale):
        line = b[off + (h - 1 - y) * row: off + (h - 1 - y) * row + w * 3]
        out.append(0)
        for x in range(0, w, scale):
            bb, g, r = line[x * 3: x * 3 + 3]
            out += bytes((r, g, bb))
    W, H = (w + scale - 1) // scale, (h + scale - 1) // scale

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(out), 6)) + chunk(b"IEND", b"")
    open(dst, "wb").write(png)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 1)
