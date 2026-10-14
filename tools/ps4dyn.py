"""Parse the SCE dynamic linking info of a PS4 ELF (eboot / prx).

usage: python ps4dyn.py <in.elf> <out.json> [nid_names.txt]

nid_names.txt (optional) is a list of symbol names, one per line; their NIDs
are computed and used to resolve imported/exported symbols.
"""
import base64
import hashlib
import json
import struct
import sys

NID_SALT = bytes.fromhex("518D64A635DED8C1E6B039B1C3E55230")
B64_ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-"

PT_DYNAMIC = 2
PT_SCE_DYNLIBDATA = 0x61000000

DT_NEEDED = 1
DT_SCE_MODULE_INFO = 0x6100000D
DT_SCE_NEEDED_MODULE = 0x6100000F
DT_SCE_EXPORT_LIB = 0x61000013
DT_SCE_IMPORT_LIB = 0x61000015
DT_SCE_JMPREL = 0x61000029
DT_SCE_PLTRELSZ = 0x6100002D
DT_SCE_RELA = 0x6100002F
DT_SCE_RELASZ = 0x61000031
DT_SCE_STRTAB = 0x61000035
DT_SCE_STRSZ = 0x61000037
DT_SCE_SYMTAB = 0x61000039
DT_SCE_SYMTABSZ = 0x6100003F

R_X86_64_64 = 1
R_X86_64_GLOB_DAT = 6
R_X86_64_JUMP_SLOT = 7
R_X86_64_RELATIVE = 8

STT = {0: "notype", 1: "object", 2: "func", 3: "section", 4: "file", 6: "tls"}
STB = {0: "local", 1: "global", 2: "weak"}


def name2nid(name: str) -> str:
    h = hashlib.sha1(name.encode() + NID_SALT).digest()
    v, = struct.unpack("<Q", h[:8])
    raw = struct.pack(">Q", v)
    return base64.b64encode(raw, b"+-").decode().rstrip("=")


def decode_b64_index(s: str) -> int:
    v = 0
    for c in s:
        v = v * 64 + B64_ALPHABET.index(c)
    return v


def cstr(buf: bytes, off: int) -> str:
    end = buf.index(b"\0", off)
    return buf[off:end].decode("ascii", "replace")


def parse(elf: bytes, names: dict) -> dict:
    phoff, = struct.unpack_from("<Q", elf, 0x20)
    phnum, = struct.unpack_from("<H", elf, 0x38)
    phdrs = [struct.unpack_from("<IIQQQQQQ", elf, phoff + i * 56) for i in range(phnum)]
    dyn = next(p for p in phdrs if p[0] == PT_DYNAMIC)
    dlib = next(p for p in phdrs if p[0] == PT_SCE_DYNLIBDATA)
    dlib_data = elf[dlib[2]:dlib[2] + dlib[5]]

    tags = []
    for off in range(dyn[2], dyn[2] + dyn[5], 16):
        tag, val = struct.unpack_from("<qQ", elf, off)
        if tag == 0:
            break
        tags.append((tag, val))
    single = {t: v for t, v in tags}

    strtab = dlib_data[single[DT_SCE_STRTAB]:single[DT_SCE_STRTAB] + single[DT_SCE_STRSZ]]

    def entry(val):
        return {"name": cstr(strtab, val & 0xFFFFFFFF),
                "version": (val >> 32) & 0xFFFF,
                "id": (val >> 48) & 0xFFFF}

    result = {"needed": [], "modules": [], "import_libs": [], "export_libs": [],
              "module_info": None}
    for tag, val in tags:
        if tag == DT_NEEDED:
            result["needed"].append(cstr(strtab, val))
        elif tag == DT_SCE_NEEDED_MODULE:
            result["modules"].append(entry(val))
        elif tag == DT_SCE_IMPORT_LIB:
            result["import_libs"].append(entry(val))
        elif tag == DT_SCE_EXPORT_LIB:
            result["export_libs"].append(entry(val))
        elif tag == DT_SCE_MODULE_INFO:
            result["module_info"] = entry(val)

    mods = {m["id"]: m["name"] for m in result["modules"]}
    libs = {l["id"]: l["name"] for l in result["import_libs"] + result["export_libs"]}
    if result["module_info"]:
        mods.setdefault(result["module_info"]["id"], result["module_info"]["name"])

    symtab_off, symtab_sz = single[DT_SCE_SYMTAB], single[DT_SCE_SYMTABSZ]
    syms = []
    for off in range(symtab_off, symtab_off + symtab_sz, 24):
        st_name, st_info, st_other, st_shndx, st_value, st_size = \
            struct.unpack_from("<IBBHQQ", dlib_data, off)
        raw = cstr(strtab, st_name)
        sym = {"raw": raw, "type": STT.get(st_info & 0xF, st_info & 0xF),
               "bind": STB.get(st_info >> 4, st_info >> 4),
               "value": st_value, "size": st_size, "defined": st_shndx != 0}
        parts = raw.split("#")
        if len(parts) == 3:
            nid, lib_b64, mod_b64 = parts
            sym["nid"] = nid
            sym["library"] = libs.get(decode_b64_index(lib_b64), lib_b64)
            sym["module"] = mods.get(decode_b64_index(mod_b64), mod_b64)
            sym["name"] = names.get(nid)
        else:
            sym["name"] = raw or None
        syms.append(sym)

    def relocs(off, size):
        out = []
        for o in range(off, off + size, 24):
            r_off, r_info, r_add = struct.unpack_from("<QQq", dlib_data, o)
            out.append((r_off, r_info & 0xFFFFFFFF, r_info >> 32, r_add))
        return out

    imports = []
    for r_off, r_type, r_sym, r_add in relocs(single[DT_SCE_JMPREL], single[DT_SCE_PLTRELSZ]):
        s = syms[r_sym]
        imports.append({"got": r_off, "kind": "plt", "nid": s.get("nid"), "name": s.get("name"),
                        "library": s.get("library"), "module": s.get("module")})
    data_imports = []
    rela_count = {}
    for r_off, r_type, r_sym, r_add in relocs(single[DT_SCE_RELA], single[DT_SCE_RELASZ]):
        rela_count[r_type] = rela_count.get(r_type, 0) + 1
        if r_type in (R_X86_64_64, R_X86_64_GLOB_DAT) and r_sym:
            s = syms[r_sym]
            if not s["defined"]:
                data_imports.append({"addr": r_off, "addend": r_add, "nid": s.get("nid"),
                                     "name": s.get("name"), "library": s.get("library"),
                                     "module": s.get("module")})

    result["imports"] = imports
    result["data_imports"] = data_imports
    result["exports"] = [s for s in syms if s["defined"] and s["bind"] != "local"]
    result["rela_type_counts"] = rela_count
    return result


def load_names(path):
    names = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            n = line.strip()
            if n:
                names[name2nid(n)] = n
    return names


if __name__ == "__main__":
    names = load_names(sys.argv[3]) if len(sys.argv) > 3 else {}
    with open(sys.argv[1], "rb") as f:
        res = parse(f.read(), names)
    with open(sys.argv[2], "w", encoding="utf-8") as f:
        json.dump(res, f, indent=1)
    resolved = sum(1 for i in res["imports"] if i["name"])
    print(f"{sys.argv[1]}: {len(res['modules'])} modules, {len(res['import_libs'])} import libs, "
          f"{len(res['imports'])} plt imports ({resolved} named), "
          f"{len(res['data_imports'])} data imports, {len(res['exports'])} exports")
