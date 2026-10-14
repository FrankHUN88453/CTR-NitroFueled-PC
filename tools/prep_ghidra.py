"""Turn a PS4 eboot ELF into a plain ELF that Ghidra loads cleanly.

- ET_SCE_DYNEXEC -> ET_EXEC, PT_SCE_RELRO -> PT_LOAD, bss materialised
- R_X86_64_RELATIVE / R_X86_64_64 relocations applied (image base 0)
- imports get a stub (ret) in an extra segment; GOT slots point at them
- function starts from .eh_frame_hdr

usage: python prep_ghidra.py <eboot.elf> <eboot.dyn.json> <out.elf> <out.symbols.tsv>

symbols.tsv lines:  I <addr> <name> <library>   import stub
                    F <addr>                     function start (eh_frame)
                    E <addr> <name>              entry point
"""
import json
import struct
import sys

PT_LOAD = 1
PT_DYNAMIC = 2
PT_GNU_EH_FRAME = 0x6474E550
PT_SCE_RELRO = 0x61000010
PT_SCE_DYNLIBDATA = 0x61000000

DT_SCE_JMPREL = 0x61000029
DT_SCE_PLTRELSZ = 0x6100002D
DT_SCE_RELA = 0x6100002F
DT_SCE_RELASZ = 0x61000031
DT_SCE_SYMTAB = 0x61000039

IMPORT_BASE = 0x3000000
IMPORT_STRIDE = 0x10
PAGE = 0x4000


def align(v, a):
    return (v + a - 1) & ~(a - 1)


def read_encoded(buf, off, enc, pc, datarel):
    fmt = enc & 0x0F
    if fmt == 0x03:
        v, = struct.unpack_from("<I", buf, off)
        size = 4
    elif fmt == 0x0B:
        v, = struct.unpack_from("<i", buf, off)
        size = 4
    elif fmt == 0x04:
        v, = struct.unpack_from("<Q", buf, off)
        size = 8
    else:
        raise ValueError(f"unsupported pointer encoding {enc:#x}")
    app = enc & 0x70
    if app == 0x10:
        v += pc
    elif app == 0x30:
        v += datarel
    return v & 0xFFFFFFFFFFFFFFFF, size


def main(elf_path, dyn_path, out_path, tsv_path):
    elf = open(elf_path, "rb").read()
    dyn = json.load(open(dyn_path))

    phoff, = struct.unpack_from("<Q", elf, 0x20)
    phnum, = struct.unpack_from("<H", elf, 0x38)
    phdrs = [list(struct.unpack_from("<IIQQQQQQ", elf, phoff + i * 56)) for i in range(phnum)]
    entry, = struct.unpack_from("<Q", elf, 0x18)

    loads = [p for p in phdrs if p[0] in (PT_LOAD, PT_SCE_RELRO)]
    pt_dyn = next(p for p in phdrs if p[0] == PT_DYNAMIC)
    dlib = next(p for p in phdrs if p[0] == PT_SCE_DYNLIBDATA)
    dlib_data = elf[dlib[2]:dlib[2] + dlib[5]]
    tags = {}
    for off in range(pt_dyn[2], pt_dyn[2] + pt_dyn[5], 16):
        tag, val = struct.unpack_from("<qQ", elf, off)
        if tag == 0:
            break
        tags[tag] = val

    # Build the in-memory image of every loadable segment.
    segs = []
    for p_type, flags, off, va, _pa, filesz, memsz, _al in loads:
        img = bytearray(memsz)
        img[:filesz] = elf[off:off + filesz]
        segs.append({"va": va, "flags": flags if p_type == PT_LOAD else 4, "img": img})

    def seg_for(va):
        for s in segs:
            if s["va"] <= va < s["va"] + len(s["img"]):
                return s
        return None

    def write_u64(va, val):
        s = seg_for(va)
        if s is None:
            return False
        o = va - s["va"]
        s["img"][o:o + 8] = struct.pack("<Q", val & 0xFFFFFFFFFFFFFFFF)
        return True

    # Import stubs.
    import_addr = {}
    stub_lines = []

    def stub(nid, name, lib):
        key = (nid, lib)
        if key not in import_addr:
            addr = IMPORT_BASE + len(import_addr) * IMPORT_STRIDE
            import_addr[key] = addr
            stub_lines.append(f"I {addr:x} {name or 'imp_' + nid.replace('+', '_').replace('-', '_')} {lib}")
        return import_addr[key]

    for imp in dyn["imports"]:
        write_u64(imp["got"], stub(imp["nid"], imp["name"], imp["library"]))
    for imp in dyn["data_imports"]:
        write_u64(imp["addr"], stub(imp["nid"], imp["name"], imp["library"]) + imp["addend"])

    # RELATIVE relocations (base 0 -> value == addend).
    n_rel = n_miss = 0
    rela_off, rela_sz = tags[DT_SCE_RELA], tags[DT_SCE_RELASZ]
    for o in range(rela_off, rela_off + rela_sz, 24):
        r_off, r_info, r_add = struct.unpack_from("<QQq", dlib_data, o)
        if r_info & 0xFFFFFFFF == 8:
            if write_u64(r_off, r_add):
                n_rel += 1
            else:
                n_miss += 1

    stub_img = bytearray(b"\xCC" * (len(import_addr) * IMPORT_STRIDE))
    for addr in import_addr.values():
        stub_img[addr - IMPORT_BASE] = 0xC3
    segs.append({"va": IMPORT_BASE, "flags": 5, "img": stub_img})

    # Function starts from .eh_frame_hdr.
    eh = next(p for p in phdrs if p[0] == PT_GNU_EH_FRAME)
    hdr_va = eh[3]
    s = seg_for(hdr_va)
    buf = s["img"]
    base = hdr_va - s["va"]
    _ver, ptr_enc, cnt_enc, tbl_enc = buf[base:base + 4]
    o = base + 4
    _eh_ptr, n = read_encoded(buf, o, ptr_enc, hdr_va + 4, hdr_va)
    o += n
    fde_count, n = read_encoded(buf, o, cnt_enc, 0, hdr_va)
    o += n
    func_starts = []
    for _ in range(fde_count):
        loc, n = read_encoded(buf, o, tbl_enc, 0, hdr_va)
        o += n
        _fde, n = read_encoded(buf, o, tbl_enc, 0, hdr_va)
        o += n
        func_starts.append(loc)

    # Write the new ELF: header, phdrs, then each segment page aligned.
    eh_phdr = (PT_GNU_EH_FRAME, 4, 0, hdr_va, hdr_va, eh[5], eh[5], 4)
    nph = len(segs) + 1
    file_off = align(0x40 + nph * 56, PAGE)
    out_ph = []
    blobs = []
    for sg in segs:
        out_ph.append((PT_LOAD, sg["flags"], file_off, sg["va"], sg["va"],
                       len(sg["img"]), len(sg["img"]), PAGE))
        blobs.append((file_off, sg["img"]))
        file_off = align(file_off + len(sg["img"]), PAGE)
    s_eh = seg_for(hdr_va)
    eh_off = next(ph[2] for ph in out_ph if ph[3] == s_eh["va"]) + hdr_va - s_eh["va"]
    out_ph.append((eh_phdr[0], eh_phdr[1], eh_off) + eh_phdr[3:])

    out = bytearray(file_off)
    out[:0x40] = elf[:0x40]
    struct.pack_into("<H", out, 0x10, 2)                  # ET_EXEC
    struct.pack_into("<QQQ", out, 0x18, entry, 0x40, 0)   # entry, phoff, shoff
    struct.pack_into("<HHHHHH", out, 0x34, 0x40, 56, nph, 64, 0, 0)
    for i, ph in enumerate(out_ph):
        struct.pack_into("<IIQQQQQQ", out, 0x40 + i * 56, *ph)
    for off, img in blobs:
        out[off:off + len(img)] = img
    open(out_path, "wb").write(out)

    with open(tsv_path, "w", encoding="utf-8") as f:
        f.write(f"E {entry:x} _start\n")
        for line in stub_lines:
            f.write(line + "\n")
        for a in sorted(set(func_starts)):
            f.write(f"F {a:x}\n")

    print(f"relocs applied {n_rel} (outside image {n_miss}), imports {len(import_addr)}, "
          f"functions {len(set(func_starts))}, out {len(out):#x} bytes")


if __name__ == "__main__":
    main(*sys.argv[1:5])
