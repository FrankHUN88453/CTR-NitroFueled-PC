"""Extract methods exposed to visual scripting (VSC) with their native addresses.

<Class>Nodes::arkRegisterFields builds, on its stack, an array of 16-byte C++
member-function pointers {ptr, adj} in the same order as a static array of
"<method>_VSC" name strings, then passes both to the node registration.

usage: python vsc_methods.py <eboot.ghidra.elf> <eboot.symbols.tsv> <alchemy_classes.json> <out.json>
"""
import bisect
import json
import os
import pickle
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capstone import CS_ARCH_X86, CS_MODE_64, Cs  # noqa: E402

from alchemy_meta import Tracker  # noqa: E402
from elfimg import Image  # noqa: E402

TEXT_END = 0x1ADA100
RODATA_END = 0x21FB5D0
RELRO = (0x2200000, 0x25E5670)


def main(elf_path, tsv_path, classes_path, out_path):
    img = Image(elf_path)
    text = bytes(img.segs[0][3])
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    tr = Tracker(md)
    starts = sorted(int(l.split()[1], 16) for l in open(tsv_path) if l.startswith("F "))
    classes = json.load(open(classes_path))
    names = {c["name"] for c in classes}
    cb2 = {c["fields_cb"]: c["name"] for c in classes if c["fields_cb"]}
    reg2 = {c["register_fn"]: c["name"] for c in classes}
    leas = pickle.load(open(os.path.join(os.path.dirname(out_path), "eboot.lea.pkl"), "rb"))

    def vsc_name(v):
        if not 0 < v < RODATA_END:
            return None
        s = img.cstr(v)
        return s if s and s.endswith("_VSC") else None

    # 1) arrays of "*_VSC" string pointers in RELRO
    arrays = []
    a = RELRO[0]
    while a < RELRO[1]:
        if vsc_name(img.u64(a)):
            start = a
            while a < RELRO[1] and vsc_name(img.u64(a)):
                a += 8
            arrays.append((start, (a - start) // 8))
        else:
            a += 8

    out, unmatched = [], []
    for arr, count in arrays:
        refs = leas.get(arr, [])
        if not refs:
            unmatched.append((arr, count, "no code ref"))
            continue
        site = refs[0]
        f = starts[bisect.bisect_right(starts, site) - 1]
        end = starts[bisect.bisect_right(starts, f)] if bisect.bisect_right(starts, f) < len(starts) \
            else TEXT_END
        _r, stack, _calls = tr.run(text[f:end], f, end)
        code = sorted((off, v) for off, v in stack.items()
                      if isinstance(v, int) and 0 < v < TEXT_END)
        # find a run of `count` code pointers spaced 0x10 apart
        offs = {off: v for off, v in code}
        run = None
        for off, _v in code:
            if all(off + 0x10 * k in offs for k in range(count)):
                run = [offs[off + 0x10 * k] for k in range(count)]
                break
        owner = cb2.get(f) or reg2.get(f)
        cls = owner[:-5] if owner and owner.endswith("Nodes") and owner[:-5] in names else owner
        if run is None:
            unmatched.append((arr, count, f"no ptr run in {f:#x} {owner}"))
            continue
        for k in range(count):
            n = vsc_name(img.u64(arr + 8 * k))
            out.append({"class": cls, "nodes_class": owner, "name": n[:-4], "addr": run[k]})

    with open(out_path, "w", encoding="utf-8") as fp:
        json.dump(out, fp, indent=1)
    print(f"name arrays {len(arrays)} ({sum(c for _a, c in arrays)} names), "
          f"methods resolved {len(out)}, unmatched arrays {len(unmatched)}")
    for u in unmatched[:15]:
        print("  unmatched", hex(u[0]), u[1], u[2])


if __name__ == "__main__":
    main(*sys.argv[1:5])
