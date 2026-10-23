#!/usr/bin/env python3
"""
Idempotent build.ninja patcher for God of War III Remastered ARM64.
Ensures that compiler flags, bridge rules, and linker dependencies are configured
exactly once without duplicate rules or corrupted target lines.
"""

import os
import sys
import re

def patch_ninja_file(ninja_path):
    if not os.path.exists(ninja_path):
        print(f"[!] {ninja_path} not found.")
        return False

    with open(ninja_path, "r", encoding="utf-8") as f:
        content = f.read()

    # Strip BOM if present
    if content.startswith('\ufeff'):
        content = content[1:]

    lines = content.splitlines()
    changed = False

    # 1. Clean up any malformed rules where gpu/gow3gpu_bridge.o was attached to another target
    for i, line in enumerate(lines):
        if line.startswith("build runtime/modules/libSceGnmDriver/ps4_gnmdriver.o gpu/gow3gpu_bridge.o:"):
            lines[i] = "build runtime/modules/libSceGnmDriver/ps4_gnmdriver.o: compile_c runtime/modules/libSceGnmDriver/ps4_gnmdriver.c"
            changed = True

    # 2. Ensure -Igpu is in cflags
    for i, line in enumerate(lines):
        if line.startswith("cflags ="):
            if " -Igpu" not in line:
                lines[i] = line.replace("-I.", "-I. -Igpu")
                changed = True
            break

    # 3. Ensure Homebrew and SPIRV-Cross flags are present in cflags and ldflags (for macOS)
    for i, line in enumerate(lines):
        if line.startswith("cflags =") and "opt/freetype" not in line:
            lines[i] = line.replace(
                "-Iruntime/core",
                "-I3rdparty/spirv-cross -I/opt/homebrew/include -I/opt/homebrew/include/spirv_cross -I/opt/homebrew/opt/freetype/include/freetype2 -I/opt/homebrew/include/freetype2 -Iruntime/core"
            )
            changed = True
        elif line.startswith("ldflags =") and "opt/freetype" not in line:
            lines[i] = line.replace(
                "-lpthread -lm",
                "-lpthread -lm -L/opt/homebrew/lib -L/opt/homebrew/opt/freetype/lib -lfreetype -lspirv-cross-msl -lspirv-cross-core -lspirv-cross-glsl -lc++"
            )
            changed = True

    # 4. Ensure exactly one compile_cpp rule exists for gpu/gow3gpu_bridge.o
    bridge_rule = "build gpu/gow3gpu_bridge.o: compile_cpp gpu/gow3gpu_bridge.cpp"
    bridge_rule_indices = [idx for idx, l in enumerate(lines) if l.strip() == bridge_rule]

    if len(bridge_rule_indices) == 0:
        # Find where ps4_gnmdriver.o is built and insert right after
        inserted = False
        for i, line in enumerate(lines):
            if line.startswith("build runtime/modules/libSceGnmDriver/ps4_gnmdriver.o:"):
                lines.insert(i + 1, bridge_rule)
                inserted = True
                changed = True
                break
        if not inserted:
            lines.append(bridge_rule)
            changed = True
    elif len(bridge_rule_indices) > 1:
        # Remove duplicates, keep only the first one
        for dup_idx in reversed(bridge_rule_indices[1:]):
            del lines[dup_idx]
            changed = True

    # 5. Ensure gpu/gow3gpu_bridge.o is in the link line exactly once
    link_prefix = "build ps4_app: link_bin "
    for i, line in enumerate(lines):
        if line.startswith(link_prefix):
            # Tokenize targets
            tokens = line.split()
            # Count occurrences of gpu/gow3gpu_bridge.o
            count = tokens.count("gpu/gow3gpu_bridge.o")
            if count == 0:
                # Add after ps4_gnmdriver.o if present, else append
                if "runtime/modules/libSceGnmDriver/ps4_gnmdriver.o" in tokens:
                    idx = tokens.index("runtime/modules/libSceGnmDriver/ps4_gnmdriver.o")
                    tokens.insert(idx + 1, "gpu/gow3gpu_bridge.o")
                else:
                    tokens.append("gpu/gow3gpu_bridge.o")
                lines[i] = " ".join(tokens)
                changed = True
            elif count > 1:
                # Remove extra occurrences
                new_tokens = []
                seen = False
                for t in tokens:
                    if t == "gpu/gow3gpu_bridge.o":
                        if not seen:
                            new_tokens.append(t)
                            seen = True
                    else:
                        new_tokens.append(t)
                lines[i] = " ".join(new_tokens)
                changed = True
            break

    if changed:
        new_content = "\n".join(lines) + "\n"
        with open(ninja_path, "w", encoding="utf-8", newline="\n") as f:
            f.write(new_content)
        print(f"[+] Successfully patched {ninja_path} (idempotent update).")
    else:
        print(f"[+] {ninja_path} is already cleanly configured.")

    return True

if __name__ == "__main__":
    base = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    target = os.path.join(base, "build.ninja")
    if len(sys.argv) > 1:
        target = sys.argv[1]
    patch_ninja_file(target)
