"""Add shaders dumped at runtime by ctr_pc (port/run/missing_shaders/Rt_<fnv>_<kind>.bin + .gnm,
e.g. the Bink video shaders embedded in the eboot) to out/shaders, so that gcn2hlsl.py
translates them like the shaders from shaders_ps4.pak.

usage: python import_runtime_shaders.py <missing_shaders dir> <out/shaders dir>
"""
import glob
import json
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from export_shaders import parse_gnm  # noqa: E402
from gcn2hlsl import fnv1a64  # noqa: E402


def main(src, out):
    index_path = os.path.join(out, "index.json")
    index = json.load(open(index_path))
    known = set()
    for e in index:
        p = os.path.join(out, e["name"] + ".bin")
        if os.path.exists(p):
            code = open(p, "rb").read()
            known.add(fnv1a64(code[:e["gnm"]["shader_size"]]))
    added = 0
    for p in sorted(glob.glob(os.path.join(src, "Rt_*.bin"))):
        base = os.path.basename(p)[:-4]
        _, h, kind = base.split("_")
        code = open(p, "rb").read()
        if fnv1a64(code) in known:
            continue  # translated from the pak meanwhile
        gnm_path = p[:-4] + ".gnm"
        if not os.path.exists(gnm_path):
            print(f"{base}: no Gnm struct dump, skipped")
            continue
        gnm = parse_gnm(kind, open(gnm_path, "rb").read())
        if gnm["shader_size"] != len(code):
            print(f"{base}: size mismatch {gnm['shader_size']} != {len(code)}, skipped")
            continue
        index.append({"name": base, "effect": "Runtime", "hash": int(h[:8], 16), "kind": kind,
                      "size": len(code), "gnm": gnm})
        shutil.copyfile(p, os.path.join(out, base + ".bin"))
        known.add(fnv1a64(code))
        added += 1
        print(f"{base}: added ({kind}, {len(code)} bytes)")
    json.dump(index, open(index_path, "w"), indent=1)
    print(f"{added} runtime shaders added")


if __name__ == "__main__":
    main(*sys.argv[1:3])
