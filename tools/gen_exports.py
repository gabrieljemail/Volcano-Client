#!/usr/bin/env python3
"""Writes the module-definition file that exports the client's own symbols.

Usage: gen_exports.py <llvm-nm> <object dir> <source dir> <output .def> <exe name> [ImGui object dir]

Every extern symbol in the client's own object files whose mangled name lives
in namespace Volcano is exported, so mods (DLLs) can link against any
non-inline method or free function. Third-party objects and the C++ standard
library are left out. ImGui's core objects (given as the optional last argument)
are exported whole, so a mod's HUD code calls the exe's ImGui and its context.
Polymorphic classes a mod derives from, and data a mod
reads, additionally need VC_API (see src/mods/VolcanoModAPI.h): data exports
have no plain import symbol, only __imp_.
"""
import os
import subprocess
import sys


def main():
    nm, object_dir, source_dir, output, name = sys.argv[1:6]
    imgui_dir = sys.argv[6] if len(sys.argv) > 6 else None

    objects = []
    for root, _, files in os.walk(object_dir):
        for f in files:
            if not f.endswith(".obj"):
                continue
            path = os.path.join(root, f)
            # Objects of deleted or renamed sources linger in the build tree.
            source = os.path.join(source_dir, os.path.relpath(path, object_dir)[:-len(".obj")])
            if os.path.exists(source):
                objects.append(path)

    imgui_objects = []
    if imgui_dir:
        for core in ("imgui", "imgui_draw", "imgui_tables", "imgui_widgets"):
            path = os.path.join(imgui_dir, core + ".cpp.obj")
            if os.path.exists(path):
                imgui_objects.append(path)

    code, data = set(), set()
    for start in range(0, len(objects), 40):
        result = subprocess.run([nm, "--defined-only", "--extern-only"] + objects[start:start + 40],
                                capture_output=True, text=True)
        for line in result.stdout.splitlines():
            parts = line.split(None, 2)
            if len(parts) != 3:
                continue
            kind, symbol = parts[1], parts[2]
            # Volcano's own C++ symbols only; skip string literals, RTTI and the compiler-made deleting destructors.
            if not symbol.startswith("?") or "Volcano@@" not in symbol:
                continue
            if symbol.startswith(("??_C", "??_R", "??_G", "??_E")):
                continue
            (code if kind in "Tt" else data).add(symbol)

    if imgui_objects:
        result = subprocess.run([nm, "--defined-only", "--extern-only"] + imgui_objects, capture_output=True, text=True)
        for line in result.stdout.splitlines():
            parts = line.split(None, 2)
            if len(parts) != 3 or not parts[2].startswith("?") or parts[2].startswith(("??_C", "??_R", "??_G", "??_E")):
                continue
            (code if parts[1] in "Tt" else data).add(parts[2])

    lines = ["LIBRARY " + name, "EXPORTS"]
    lines += ["    " + s for s in sorted(code)]
    lines += ["    " + s + " DATA" for s in sorted(data - code)]

    text = "\n".join(lines) + "\n"
    if os.path.exists(output) and open(output).read() == text:
        return  # Unchanged: leave the timestamp so the link isn't redone.
    with open(output, "w") as f:
        f.write(text)
    print(f"gen_exports: {len(code)} functions, {len(data - code)} data symbols exported")


if __name__ == "__main__":
    main()
