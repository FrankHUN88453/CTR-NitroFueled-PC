"""Convert a fake-signed (unencrypted) PS4 SELF/SPRX into a plain ELF.

usage: python self2elf.py <in.self> <out.elf>
"""
import struct
import sys
import zlib

SELF_MAGIC = b"\x4f\x15\x3d\x1d"

SF_ENCRYPTED = 0x2
SF_DEFLATED = 0x8
SF_BLOCKED = 0x800


def self2elf(data: bytes) -> bytes:
    if data[:4] != SELF_MAGIC:
        if data[:4] == b"\x7fELF":
            return data
        raise ValueError("not a SELF file")

    nseg, = struct.unpack_from("<H", data, 0x18)
    segs = [struct.unpack_from("<4Q", data, 0x20 + i * 0x20) for i in range(nseg)]

    elf_off = 0x20 + nseg * 0x20
    phoff, = struct.unpack_from("<Q", data, elf_off + 0x20)
    phentsize, phnum = struct.unpack_from("<HH", data, elf_off + 0x36)
    phdrs = [struct.unpack_from("<IIQQQQQQ", data, elf_off + phoff + i * phentsize)
             for i in range(phnum)]

    hdr_size = phoff + phnum * phentsize
    end = hdr_size
    for p in phdrs:
        end = max(end, p[2] + p[5])
    out = bytearray(end)
    out[:hdr_size] = data[elf_off:elf_off + hdr_size]

    for flags, off, csize, dsize in segs:
        if not flags & SF_BLOCKED:
            continue
        if flags & SF_ENCRYPTED:
            raise ValueError("segment is encrypted; need a decrypted dump")
        idx = (flags >> 20) & 0xFFF
        blob = data[off:off + csize]
        if flags & SF_DEFLATED:
            blob = zlib.decompress(blob)
        p_off, p_filesz = phdrs[idx][2], phdrs[idx][5]
        out[p_off:p_off + p_filesz] = blob[:p_filesz]

    # PT_SCE_VERSION (0x6fffff01) is not a SELF segment; it is stored at the
    # end of the SELF file.
    for p in phdrs:
        if p[0] == 0x6FFFFF01 and p[5]:
            out[p[2]:p[2] + p[5]] = data[len(data) - p[5]:]
    return bytes(out)


if __name__ == "__main__":
    src, dst = sys.argv[1], sys.argv[2]
    with open(src, "rb") as f:
        elf = self2elf(f.read())
    with open(dst, "wb") as f:
        f.write(elf)
    print(f"{src} -> {dst} ({len(elf):#x} bytes)")
