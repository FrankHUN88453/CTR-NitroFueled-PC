"""Disassembles the DXBC shaders embedded in IGZ files (the D3D11 shaders of the PC games on the
same Alchemy engine, e.g. Crash Bandicoot N. Sane Trilogy) with d3dcompiler_47.dll, as a
reference for the GCN -> HLSL translations of the PS4 shaders.

Writes <out dir>/<effect>_<index>_<type>.asm for every DXBC blob ("DXBC" magic) in the files.

usage: python dxbc_disasm.py <file.igz|dir> <out dir> [d3dcompiler_47.dll]
"""
import ctypes
import glob
import os
import struct
import sys


def load_compiler(path=None):
    for p in [path, "d3dcompiler_47.dll"]:
        if not p:
            continue
        try:
            return ctypes.WinDLL(p)
        except OSError:
            pass
    raise OSError("d3dcompiler_47.dll not found")


def disassemble(dll, blob):
    out = ctypes.c_void_p()
    hr = dll.D3DDisassemble(blob, len(blob), 0, None, ctypes.byref(out))
    if hr != 0 or not out.value:
        return None
    vt = ctypes.cast(ctypes.cast(out, ctypes.POINTER(ctypes.c_void_p))[0], ctypes.POINTER(ctypes.c_void_p))
    get_ptr = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(vt[3])
    get_size = ctypes.WINFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)(vt[4])
    release = ctypes.WINFUNCTYPE(ctypes.c_ulong, ctypes.c_void_p)(vt[2])
    text = ctypes.string_at(get_ptr(out), get_size(out)).decode(errors="replace").rstrip("\0")
    release(out)
    return text


def blobs(data):
    """DXBC containers: magic, 16-byte checksum, version, total size."""
    i = data.find(b"DXBC")
    while i >= 0:
        if i + 32 <= len(data):
            size = struct.unpack_from("<I", data, i + 24)[0]
            if 32 < size <= len(data) - i:
                yield i, data[i:i + size]
        i = data.find(b"DXBC", i + 4)


def main(path, out_dir, dll_path=None):
    dll = load_compiler(dll_path)
    os.makedirs(out_dir, exist_ok=True)
    files = [path] if path.endswith(".igz") else sorted(glob.glob(os.path.join(path, "**", "*.igz"), recursive=True))
    total = 0
    for p in files:
        effect = os.path.basename(p)[:-4]
        data = open(p, "rb").read()
        for n, (off, blob) in enumerate(blobs(data)):
            text = disassemble(dll, blob)
            if not text:
                continue
            kind = next((line.split()[0] for line in text.splitlines() if line[:3] in ("ps_", "vs_", "gs_", "cs_", "hs_", "ds_")), "x")
            open(os.path.join(out_dir, f"{effect}_{n:03d}_{kind}.asm"), "w").write(text)
            total += 1
    print(f"{total} shaders disassembled -> {out_dir}")


if __name__ == "__main__":
    main(*sys.argv[1:4])
