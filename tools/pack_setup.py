"""Packs the first-start setup that a release exe carries (RCDATA "SETUP"): a minimal Python runtime
built from a python.org CPython install (like the official "embeddable" package: the interpreter,
its DLLs and the standard library as compiled .pyc in a zip, isolated by a ._pth file) plus the
shader setup scripts (setup_shaders.py and what it imports). Nothing of the game is in it.

Archive: "CTRP", u32 version, u32 count, then per file: u16 name length, name (utf-8, '/' paths),
u32 size, data. Files land under python/ and setup/.

Run it with the same Python that is packed (the .pyc files must match the interpreter).
usage: python pack_setup.py <out .bin> [python install dir, default: the running one]
"""
import io
import os
import struct
import sys
import zipfile

SCRIPTS = ["setup_shaders.py", "runtime_shaders.json", "iga.py", "igz.py", "export_shaders.py", "gcn.py",
           "gcn2hlsl.py", "pack_shaders.py", "font_glyphs.py", "make_glyph_atlas.py"]
# standard library parts the setup never needs
SKIP_LIB = {"test", "idlelib", "tkinter", "turtledemo", "ensurepip", "venv", "pydoc_data", "site-packages",
            "lib2to3", "__pycache__", "sqlite3", "_pyrepl"}
SKIP_DLL_PREFIX = ("_test", "_tkinter", "tcl", "tk", "_sqlite3", "sqlite3", "_ctypes_test")


def stdlib_zip(lib):
    """The standard library as unchecked hash-based .pyc files (compiled in memory: the install folder
    may not be writable), the layout zipimport reads."""
    import importlib._bootstrap_external as be
    import importlib.util
    buf = io.BytesIO()
    skipped = 0
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
        for root, dirs, names in os.walk(lib):
            dirs[:] = sorted(d for d in dirs if d not in SKIP_LIB)
            for name in sorted(names):
                if not name.endswith(".py"):
                    continue
                path = os.path.join(root, name)
                rel = os.path.relpath(path, lib).replace(os.sep, "/")
                source = open(path, "rb").read()
                try:
                    code = compile(source, rel, "exec", dont_inherit=True, optimize=1)
                except SyntaxError:
                    skipped += 1
                    continue
                data = be._code_to_hash_pyc(code, importlib.util.source_hash(source), checked=False)
                z.writestr(rel[:-3] + ".pyc", bytes(data))
    if skipped:
        print(f"  {skipped} standard library files did not compile (skipped)")
    return buf.getvalue()


def main(out, home=None):
    home = home or os.path.dirname(sys.executable)
    ver = f"{sys.version_info.major}{sys.version_info.minor}"
    files = []
    for name in ("python.exe", "python3.dll", f"python{ver}.dll", "vcruntime140.dll", "vcruntime140_1.dll"):
        files.append(("python/" + name, open(os.path.join(home, name), "rb").read()))
    dlls = os.path.join(home, "DLLs")
    for name in sorted(os.listdir(dlls)):
        if name.lower().endswith((".pyd", ".dll")) and not name.lower().startswith(SKIP_DLL_PREFIX):
            files.append(("python/" + name, open(os.path.join(dlls, name), "rb").read()))
    files.append((f"python/python{ver}.zip", stdlib_zip(os.path.join(home, "Lib"))))
    files.append((f"python/python{ver}._pth", f"python{ver}.zip\n.\n".encode()))
    files.append(("python/LICENSE.txt", open(os.path.join(home, "LICENSE.txt"), "rb").read()))
    here = os.path.dirname(os.path.abspath(__file__))
    for name in SCRIPTS:
        files.append(("setup/" + name, open(os.path.join(here, name), "rb").read()))
    out_parts = [b"CTRP", struct.pack("<II", 1, len(files))]
    for name, data in files:
        n = name.encode()
        out_parts += [struct.pack("<H", len(n)), n, struct.pack("<I", len(data)), data]
    blob = b"".join(out_parts)
    if not os.path.exists(out) or open(out, "rb").read() != blob:
        open(out, "wb").write(blob)
    print(f"setup: {len(files)} files, {len(blob) >> 20} MiB (Python {sys.version.split()[0]}) -> {out}")


if __name__ == "__main__":
    main(*sys.argv[1:3])
