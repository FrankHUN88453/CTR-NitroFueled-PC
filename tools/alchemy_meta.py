"""Extract the Alchemy (ig*) reflection data from the eboot.

Every reflected class has a self-registration function that ends in a call to
igMetaObject registration (REGISTER) with:
    rdi  &Class::_Meta            (global slot)
    rsi  meta-class instance       (0 = default igMetaObject)
    rdx  Parent registration fn    (called first)
    rcx  &igMetaObjectList slot
    r8   class name, r9d sizeof(Class)
    stack: &temp_instance (vptr at +0), fields callback, finalize table
The fields callback instantiates field descriptors (FIELDS_INST: rsi = array of
field-type getters, ecx = count) and names them (FIELDS_NAME: rsi = names,
rdx = per-field globals, rcx = u16 offsets, r8d = first own field index).

Addresses are specific to CUSA13795 / CL 1233015.

usage: python alchemy_meta.py <eboot.ghidra.elf> <eboot.symbols.tsv> <out.json>
"""
import bisect
import json
import os
import re
import struct
import sys

from capstone import CS_ARCH_X86, CS_MODE_64, Cs
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_OP_REG, X86_REG_RBP, X86_REG_RIP

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from elfimg import Image  # noqa: E402

REGISTER = 0x143FCD0
ENSURE = 0x13ECC00
FIELDS_INST = 0x143F110
FIELDS_NAME = 0x143F250
TEXT_END = 0x1ADA100

VOLATILE = ("rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11")
REG64 = {}
for full, aliases in {
    "rax": "eax ax al", "rbx": "ebx bx bl", "rcx": "ecx cx cl", "rdx": "edx dx dl",
    "rsi": "esi si sil", "rdi": "edi di dil", "rbp": "ebp", "rsp": "esp",
}.items():
    REG64[full] = full
    for a in aliases.split():
        REG64[a] = full
for n in range(8, 16):
    for suf in ("", "d", "w", "b"):
        REG64[f"r{n}{suf}"] = f"r{n}"


class Tracker:
    """Very small forward register/stack tracker over straight-line code."""

    def __init__(self, md):
        self.md = md

    def run(self, code, start, stop_at, on_call=None):
        regs, stack, pushes, calls = {}, {}, [], []
        for ins in self.md.disasm(code, start):
            if ins.address >= stop_at:
                break
            m, ops = ins.mnemonic, ins.operands
            if m == "push":
                if ops[0].type == X86_OP_REG:
                    pushes.append(regs.get(REG64.get(ins.reg_name(ops[0].reg)), None))
                elif ops[0].type == X86_OP_IMM:
                    pushes.append(ops[0].imm)
                else:
                    pushes.append(None)
                continue
            if m in ("call", "jmp") and ops and ops[0].type == X86_OP_IMM:
                target = ops[0].imm
                snap = {r: regs.get(r) for r in VOLATILE}
                snap["pushes"] = list(pushes)
                calls.append((ins.address, target, snap))
                if on_call:
                    on_call(ins.address, target, snap, stack)
                for r in VOLATILE:
                    regs.pop(r, None)
                regs["rax"] = ("ret", target, snap.get("rdi"))
                pushes = []
                continue
            if m in ("ret",):
                break
            if len(ops) != 2:
                if ops and ops[0].type == X86_OP_REG:
                    regs.pop(REG64.get(ins.reg_name(ops[0].reg)), None)
                continue
            dst, src = ops
            if dst.type == X86_OP_REG:
                d = REG64.get(ins.reg_name(dst.reg))
                val = None
                if m == "lea" and src.type == X86_OP_MEM:
                    if src.mem.base == X86_REG_RIP:
                        val = ins.address + ins.size + src.mem.disp
                    elif src.mem.base == X86_REG_RBP and src.mem.index == 0:
                        val = ("stack", src.mem.disp)
                elif m == "mov":
                    if src.type == X86_OP_IMM:
                        val = src.imm
                    elif src.type == X86_OP_REG:
                        val = regs.get(REG64.get(ins.reg_name(src.reg)))
                    elif src.type == X86_OP_MEM and src.mem.base == X86_REG_RIP:
                        val = ("load", ins.address + ins.size + src.mem.disp)
                    elif src.type == X86_OP_MEM and src.mem.base == X86_REG_RBP and src.mem.index == 0:
                        val = stack.get(src.mem.disp)
                elif m == "xor" and src.type == X86_OP_REG and src.reg == dst.reg:
                    val = 0
                if val is None:
                    regs.pop(d, None)
                else:
                    regs[d] = val
            elif dst.type == X86_OP_MEM and dst.mem.base == X86_REG_RBP and m == "mov" \
                    and dst.mem.index == 0:
                if src.type == X86_OP_REG:
                    stack[dst.mem.disp] = regs.get(REG64.get(ins.reg_name(src.reg)))
                elif src.type == X86_OP_IMM:
                    stack[dst.mem.disp] = src.imm
        return regs, stack, calls


def main(elf_path, tsv_path, out_path):
    img = Image(elf_path)
    text = bytes(img.segs[0][3])
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    tr = Tracker(md)

    starts = sorted(int(l.split()[1], 16) for l in open(tsv_path) if l.startswith("F "))

    def func_of(addr):
        i = bisect.bisect_right(starts, addr) - 1
        return starts[i] if i >= 0 else None

    def func_end(fstart):
        i = bisect.bisect_right(starts, fstart)
        return starts[i] if i < len(starts) else TEXT_END

    def as_int(v):
        return v if isinstance(v, int) else None

    # 1) all direct calls to REGISTER
    sites = []
    for m in re.finditer(rb"\xe8", text[:TEXT_END]):
        i = m.start()
        d, = struct.unpack_from("<i", text, i + 1)
        if i + 5 + d == REGISTER:
            sites.append(i)

    classes = []
    for site in sites:
        f = func_of(site)
        regs, stack, calls = tr.run(text[f:site + 5], f, site + 5)
        snap = next((c[2] for c in calls if c[0] == site), None)
        if snap is None:
            continue
        pushes = snap["pushes"]
        temp, fields_cb, finalize = (pushes[-1:] + [None])[0], None, None
        if len(pushes) >= 3:
            finalize, fields_cb, temp = pushes[-3], pushes[-2], pushes[-1]
        vptr = None
        if isinstance(temp, tuple) and temp[0] == "stack":
            vptr = as_int(stack.get(temp[1]))
        meta_class_slot = None
        rsi = snap.get("rsi")
        if isinstance(rsi, tuple) and rsi[0] == "ret" and rsi[1] == ENSURE:
            meta_class_slot = as_int(rsi[2])
        name = img.cstr(snap["r8"]) if as_int(snap.get("r8")) else None
        classes.append({
            "name": name,
            "size": as_int(snap.get("r9")),
            "slot": as_int(snap.get("rdi")),
            "register_fn": f,
            "register_site": site,
            "parent_register_fn": as_int(snap.get("rdx")),
            "meta_class_slot": meta_class_slot,
            "list_slot": as_int(snap.get("rcx")),
            "vptr": vptr,
            "fields_cb": as_int(fields_cb),
            "finalize": as_int(finalize),
        })

    by_regfn = {c["register_fn"]: c for c in classes}
    by_slot = {c["slot"]: c for c in classes if c["slot"]}

    # 2) field-type getters -> class. Simple getters call
    #    ENSURE(&Type::_Meta, Type::register); composite ones (refs/arrays with an
    #    element type) call a base getter and then an element getter.
    type_cache = {}

    def type_of_getter(g, depth=0):
        if g in type_cache:
            return type_cache[g]
        res = (None, None)
        if g and g < TEXT_END and depth < 4:
            end = min(func_end(g), g + 0x200)
            _r, _s, calls = tr.run(text[g:end], g, end)
            for _a, tgt, snap in calls:
                if tgt == ENSURE:
                    c = by_slot.get(as_int(snap.get("rdi"))) or by_regfn.get(as_int(snap.get("rsi")))
                    if c:
                        res = (c["name"], None)
                        break
            else:
                found = [t for t in (type_of_getter(tgt, depth + 1)[0] for _a, tgt, _s in calls) if t]
                if found:
                    res = (found[0], found[1] if len(found) > 1 else None)
        type_cache[g] = res
        return res

    # 3) fields
    n_fields = 0
    for c in classes:
        cb = c["fields_cb"]
        c["fields"] = []
        if not cb or cb >= TEXT_END:
            continue
        _r, _s, calls = tr.run(text[cb:func_end(cb)], cb, func_end(cb))
        inst = [s for _a, t, s in calls if t == FIELDS_INST]
        names = [s for _a, t, s in calls if t == FIELDS_NAME]
        if not inst or not names:
            continue
        types_arr, count = as_int(inst[0].get("rsi")), as_int(inst[0].get("rcx"))
        names_arr, globs_arr = as_int(names[0].get("rsi")), as_int(names[0].get("rdx"))
        offs_arr = as_int(names[0].get("rcx"))
        if not (types_arr and count and names_arr and offs_arr) or count > 4096:
            continue
        for k in range(count):
            getter = img.u64(types_arr + 8 * k)
            ftype, elem = type_of_getter(getter)
            c["fields"].append({
                "name": img.cstr(img.u64(names_arr + 8 * k)),
                "offset": struct.unpack("<H", img.read(offs_arr + 2 * k, 2))[0],
                "type": ftype,
                "element_type": elem,
                "type_getter": getter,
                "global": img.u64(globs_arr + 8 * k) if globs_arr else None,
            })
        n_fields += count

    for c in classes:
        p = by_regfn.get(c["parent_register_fn"])
        c["parent"] = p["name"] if p else None
        mc = by_slot.get(c["meta_class_slot"])
        c["meta_class"] = mc["name"] if mc else None

    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(classes, f, indent=1)
    print(f"call sites {len(sites)}, classes {len(classes)}, named {sum(1 for c in classes if c['name'])}, "
          f"with parent {sum(1 for c in classes if c['parent'])}, with vptr "
          f"{sum(1 for c in classes if c['vptr'])}, with fields "
          f"{sum(1 for c in classes if c['fields'])}, fields {n_fields}, "
          f"typed {sum(1 for c in classes for x in c['fields'] if x['type'])}")


if __name__ == "__main__":
    main(*sys.argv[1:4])
