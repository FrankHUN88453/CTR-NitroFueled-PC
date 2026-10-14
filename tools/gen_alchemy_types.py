"""Turn out/alchemy_classes.json into
  - a C++-style header for reading (alchemy_classes.h)
  - a Ghidra apply file (ghidra_alchemy.json): structs, labels, vtable names

(also picks up vsc_methods.json next to the classes json)

usage: python gen_alchemy_types.py <eboot.ghidra.elf> <alchemy_classes.json> <out.h> <out_ghidra.json>
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from elfimg import Image  # noqa: E402

TEXT_END = 0x1ADA100
IMPORT_LO, IMPORT_HI = 0x3000000, 0x3010000

# metafield type -> (C type, size, ghidra type name)
PRIM = {
    "igBoolMetaField": ("bool", 1),
    "igCharMetaField": ("int8_t", 1),
    "igUnsignedCharMetaField": ("uint8_t", 1),
    "igShortMetaField": ("int16_t", 2),
    "igUnsignedShortMetaField": ("uint16_t", 2),
    "igIntMetaField": ("int32_t", 4),
    "igUnsignedIntMetaField": ("uint32_t", 4),
    "igLongMetaField": ("int64_t", 8),
    "igUnsignedLongMetaField": ("uint64_t", 8),
    "igSizeTypeMetaField": ("uint64_t", 8),
    "igFloatMetaField": ("float", 4),
    "igDoubleMetaField": ("double", 8),
    "igTimeMetaField": ("float", 4),
    "igStringMetaField": ("char*", 8),
    "igNameMetaField": ("igName", 16),
    "igRawRefMetaField": ("void*", 8),
    "igHandleMetaField": ("igHandle", 8),
    "igVec2fMetaField": ("float[2]", 8),
    "igVec3fMetaField": ("float[3]", 12),
    "igVec4fMetaField": ("float[4]", 16),
    "igQuaternionfMetaField": ("float[4]", 16),
    "igMatrix44fMetaField": ("float[16]", 64),
    "igVec2iMetaField": ("int32_t[2]", 8),
    "igVec3iMetaField": ("int32_t[3]", 12),
    "igVec4iMetaField": ("int32_t[4]", 16),
    "igEnumMetaField": ("int32_t", 4),
    "igBitFieldMetaField": ("uint32_t", 4),
}
POINTERS = ("igObjectRefMetaField", "igMemoryRefMetaField", "igMemoryRefHandleMetaField",
            "igObjectRefArrayMetaField")


def field_ctype(f):
    t, e = f["type"], f.get("element_type")
    if t in PRIM:
        return PRIM[t]
    if t in POINTERS:
        return ("void*", 8)
    return (None, None)


def main(elf_path, json_path, h_path, g_path):
    img = Image(elf_path)
    classes = json.load(open(json_path))
    by = {c["name"]: c for c in classes}

    def depth(c):
        d = 0
        while c and c["parent"]:
            c = by.get(c["parent"])
            d += 1
        return d

    order = sorted(classes, key=lambda c: (depth(c), c["name"]))

    def read_vtable(v):
        out = []
        while v:
            p = img.u64(v + 8 * len(out))
            if not (0 < p < TEXT_END or IMPORT_LO <= p < IMPORT_HI):
                break
            out.append(p)
        return out

    # vtables: name each slot after the class that first puts a new function there
    vtables, slot_owner, func_names = {}, {}, {}
    for c in order:
        vt = read_vtable(c["vptr"]) if c["vptr"] else []
        vtables[c["name"]] = vt
        p = by.get(c["parent"])
        pvt = None
        while p is not None and pvt is None:
            pvt = vtables.get(p["name"]) or None
            p = by.get(p["parent"]) if pvt is None else p
        owners = []
        for i, fn in enumerate(vt):
            if pvt and i < len(pvt):
                owner = slot_owner[(p["name"], i)]
                if pvt[i] == fn:
                    owners.append(owner)
                    continue
            else:
                owner = c["name"]
            owners.append(owner)
            if fn < TEXT_END and fn not in func_names:
                func_names[fn] = f"{c['name']}::vf{i:02d}" if owner == c["name"] \
                    else f"{c['name']}::{owner}_vf{i:02d}"
        for i, o in enumerate(owners):
            slot_owner[(c["name"], i)] = o

    structs, labels, own_members = [], [], {}
    lines = ["// Generated from Alchemy reflection data of CTR Nitro-Fueled (CUSA13795 CL1233015).",
             "// offsets are absolute within the object; [static] fields are not instance members.",
             ""]
    for c in order:
        name = c["name"]
        inst = sorted((f for f in c["fields"] if not (f["type"] or "").startswith("igStatic")),
                      key=lambda f: f["offset"])
        statics = [f for f in c["fields"] if (f["type"] or "").startswith("igStatic")]
        members = []
        for i, f in enumerate(inst):
            ctype, size = field_ctype(f)
            nxt = inst[i + 1]["offset"] if i + 1 < len(inst) else (c["size"] or f["offset"])
            room = max(nxt - f["offset"], 0)
            if size is None or size > room:
                ctype, size = (f"uint8_t[{room}]", room) if room else (None, 0)
            members.append({"offset": f["offset"], "size": size, "ctype": ctype, "name": f["name"],
                            "meta": f["type"] + (f"<{f['element_type']}>" if f.get("element_type") else "")})
        # Flatten: derived fields may live in the parent's tail padding, so the
        # parent cannot be embedded as one block.
        own = [m for m in members if m["ctype"]]
        flat = list(own_members.get(c["parent"], [])) if c["parent"] else \
            [{"offset": 0, "size": 8, "ctype": "void*", "name": "_vfptr", "meta": ""}]
        taken = set()
        for m in flat:
            taken.update(range(m["offset"], m["offset"] + m["size"]))
        for m in own:
            span = set(range(m["offset"], m["offset"] + m["size"]))
            if not span & taken:
                flat.append(m)
                taken |= span
        flat.sort(key=lambda m: m["offset"])
        own_members[name] = flat
        structs.append({"name": name, "parent": c["parent"], "size": c["size"] or 0,
                        "members": flat})

        lines.append(f"// meta={c['meta_class'] or 'igMetaObject'} register=0x{c['register_fn']:x}"
                     + (f" vptr=0x{c['vptr']:x} ({len(vtables[name])} slots)" if c["vptr"] else ""))
        lines.append(f"struct {name}" + (f" : {c['parent']}" if c["parent"] else "")
                     + f" {{  // sizeof 0x{(c['size'] or 0):x}")
        for m in members:
            lines.append(f"    /* +0x{m['offset']:03x} */ {m['ctype'] or '?':<14} {m['name']};  // {m['meta']}")
        for f in statics:
            lines.append(f"    /* static */ {f['name']};  // {f['type']}<{f.get('element_type')}>")
        lines.append("};")
        lines.append("")

        labels.append((c["register_fn"], f"{name}::arkRegister", True))
        if c["slot"]:
            labels.append((c["slot"], f"{name}::_Meta", False))
        if c["fields_cb"] and c["fields_cb"] < TEXT_END:
            labels.append((c["fields_cb"], f"{name}::arkRegisterFields", True))
        if c["vptr"]:
            labels.append((c["vptr"], f"{name}::vftable", False))
        for f in c["fields"]:
            if f.get("global"):
                labels.append((f["global"], f"{name}::{f['name']}_MetaField", False))

    for fn, n in func_names.items():
        labels.append((fn, n, True))
    vsc_path = os.path.join(os.path.dirname(json_path), "vsc_methods.json")
    if os.path.exists(vsc_path):
        for m in json.load(open(vsc_path)):
            labels.append((m["addr"], f"{m['class'] or 'VSC'}::{m['name']}", True))

    vtinfo = [{"vptr": by[n]["vptr"], "class": n, "slots": len(v)} for n, v in vtables.items() if v]
    with open(g_path, "w", encoding="utf-8") as f:
        json.dump({"structs": structs, "vtables": vtinfo,
                   "labels": [{"addr": a, "name": n, "func": fn} for a, n, fn in labels]}, f)
    with open(h_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print(f"structs {len(structs)}, vtables {len(vtinfo)}, named virtual funcs {len(func_names)}, "
          f"labels {len(labels)}")


if __name__ == "__main__":
    main(*sys.argv[1:5])
