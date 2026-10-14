"""Shared helpers: load the prepared (relocated) ELF image produced by prep_ghidra.py."""
import bisect
import struct


class Image:
    def __init__(self, path):
        data = open(path, "rb").read()
        phnum, = struct.unpack_from("<H", data, 0x38)
        self.segs = []
        for i in range(phnum):
            t, fl, off, va, _pa, fsz, msz, _al = struct.unpack_from("<IIQQQQQQ", data, 0x40 + i * 56)
            if t == 1:
                self.segs.append((va, va + msz, fl, memoryview(data)[off:off + fsz]))
        self.segs.sort()

    def seg(self, va):
        for s in self.segs:
            if s[0] <= va < s[1]:
                return s
        return None

    def read(self, va, n):
        s = self.seg(va)
        o = va - s[0]
        return bytes(s[3][o:o + n])

    def u64(self, va):
        return struct.unpack("<Q", self.read(va, 8))[0]

    def u32(self, va):
        return struct.unpack("<I", self.read(va, 4))[0]

    def cstr(self, va, maxlen=512):
        s = self.seg(va)
        if s is None:
            return None
        o = va - s[0]
        b = bytes(s[3][o:o + maxlen])
        end = b.find(b"\0")
        if end < 0:
            return None
        try:
            return b[:end].decode("ascii")
        except UnicodeDecodeError:
            return None


def load_relocs(dlib_json_path=None):
    """Return sorted list of (site, target) for RELATIVE relocs, read from the raw eboot.elf."""
    raise NotImplementedError
