"""Lists the constant buffer layouts (RDEF reflection chunk) of DXBC shaders embedded in IGZ
files, e.g. the PC (DX11) shaders of other games on the same Alchemy engine version.

usage: python dxbc_rdef.py <file.igz|dir> [effect filter]
"""
import glob
import os
import struct
import sys


def cstr(b, o):
    return b[o:b.index(b"\0", o)].decode(errors="replace")


def rdef(dx):
    n = struct.unpack_from("<I", dx, 28)[0]
    for k in range(n):
        off = struct.unpack_from("<I", dx, 32 + 4 * k)[0]
        if dx[off:off + 4] != b"RDEF":
            continue
        c = dx[off + 8:]
        ncb, cbo, nres, reso = struct.unpack_from("<IIII", c, 0)
        out = []
        for r in range(nres):
            name_o, typ, ret, dim, ns, bindp, bindc, flags = struct.unpack_from("<8I", c, reso + 32 * r)
            out.append(f"  res {cstr(c, name_o)} type {typ} slot {bindp}")
        for i in range(ncb):
            name_o, nvar, varo, size, flags, typ = struct.unpack_from("<6I", c, cbo + 24 * i)
            out.append(f"  cbuffer {cstr(c, name_o)} size {size}")
            for v in range(nvar):
                vn, vo, vs, vf, vt, dv = struct.unpack_from("<6I", c, varo + 24 * v)
                out.append(f"    +{vo:4d} {vs:4d} {cstr(c, vn)}")
        return out
    return []


def main(path, filt=""):
    files = [path] if path.endswith(".igz") else sorted(glob.glob(os.path.join(path, "**", "*.igz"), recursive=True))
    for p in files:
        if filt and filt.lower() not in os.path.basename(p).lower():
            continue
        b = open(p, "rb").read()
        pos, seen = 0, set()
        while True:
            pos = b.find(b"DXBC", pos)
            if pos < 0:
                break
            size = struct.unpack_from("<I", b, pos + 24)[0]
            dx = b[pos:pos + size]
            lines = rdef(dx)
            key = "\n".join(lines)
            if lines and key not in seen:
                seen.add(key)
                print(f"== {os.path.basename(p)} @{pos:x} ({size} bytes)")
                print(key)
            pos += 4
    return 0


if __name__ == "__main__":
    main(*sys.argv[1:3])
