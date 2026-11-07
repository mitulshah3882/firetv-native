#!/usr/bin/env bash
# Builds the host-side N64 tools used by Phase 2 (static recompilation of libdragon homebrew):
#   - N64Recomp (pinned in third_party/deps.lock), with small idempotent build-time patches
#
#   tools/n64-tools.sh        -> build/host-n64recomp/N64Recomp
#
# SPDX-FileCopyrightText: 2026 Mitul Shah
# SPDX-License-Identifier: MIT
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/third_party/_src/N64Recomp-598ce20"
OUT="$ROOT/build/host-n64recomp"
[[ -d "$SRC" ]] || "$ROOT/tools/fetch-deps.sh" n64recomp

# Patch 1: MIPS trap instructions (teq/tne/tge/...). Modern GCC emits `teq rt, zero, 7` after
# every integer division (-mcheck-zero-division), including inside the prebuilt newlib, and
# N64Recomp rejects the whole function. Division by zero is already defined behaviour in the
# recompiled C, so treat traps as no-ops.
python3 -I - "$SRC/src/recompilation.cpp" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
marker = "/* firetv-native: trap instructions */"
if marker not in s:
    anchor = "    case InstrId::cpu_break:\n"
    assert anchor in s, "N64Recomp source changed; update tools/n64-tools.sh"
    patch = ("    " + marker + "\n"
             "    case InstrId::cpu_teq: case InstrId::cpu_tne: case InstrId::cpu_tge: case InstrId::cpu_tgeu:\n"
             "    case InstrId::cpu_tlt: case InstrId::cpu_tltu: case InstrId::cpu_teqi: case InstrId::cpu_tnei:\n"
             "    case InstrId::cpu_tgei: case InstrId::cpu_tgeiu: case InstrId::cpu_tlti: case InstrId::cpu_tltiu:\n"
             "        break;\n")
    s = s.replace(anchor, patch + anchor, 1)
    open(p, "w").write(s)
    print("patched: trap instructions")
PY

# Patch 2: memory-ordering and cache hints (sync, cache, pref) have no meaning for recompiled code.
python3 -I - "$SRC/src/recompilation.cpp" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
marker = "/* firetv-native: barrier and cache hints */"
if marker not in s:
    anchor = "    case InstrId::cpu_break:\n"
    patch = ("    " + marker + "\n"
             "    case InstrId::cpu_sync: case InstrId::cpu_cache: case InstrId::cpu_pref:\n"
             "        break;\n")
    s = s.replace(anchor, patch + anchor, 1)
    open(p, "w").write(s)
    print("patched: sync/cache/pref")
PY

# Patch 3: coprocessor-0 registers other than Status (libdragon reads Count for timing, etc.) go
# through runtime hooks: `x = cop0_read(ctx, reg)` / `cop0_write(ctx, reg, x)`.
python3 -I - "$SRC/src/recompilation.cpp" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
marker = "/* firetv-native: generic cop0 v2 */"
if marker not in s:
    old_r = """            default:
                fmt::print(stderr, "Unhandled cop0 register in mfc0: {}\\n", (int)reg);
                return false;"""
    new_r = """            default: """ + marker + """
                print_indent();
                if (rt == 0) fmt::print(output_file, "cop0_read(ctx, {});\\n", (int)reg); else fmt::print(output_file, "ctx->r{} = cop0_read(ctx, {});\\n", rt, (int)reg);
                break;"""
    assert old_r in s, "mfc0 anchor not found"
    s = s.replace(old_r, new_r, 1)
    old_w = """            default:
                fmt::print(stderr, "Unhandled cop0 register in mtc0: {}\\n", (int)reg);
                return false;"""
    new_w = """            default:
                print_indent();
                fmt::print(output_file, "cop0_write(ctx, {}, {});\\n", (int)reg, rt == 0 ? std::string("0") : fmt::format("ctx->r{}", rt));
                break;"""
    assert old_w in s, "mtc0 anchor not found"
    s = s.replace(old_w, new_w, 1)
    open(p, "w").write(s)
    print("patched: generic cop0")
PY

# Patch 4: GCC's local clone names (foo.part.0, foo.isra.0, cb.0) aren't valid C identifiers, and
# recompiled newlib/libdragon functions (printf, abort, main, ...) would collide with the host's
# own symbols (on Vega, a shared library, calls could even bind to the host libc). So: replace '.'
# with '_' and prefix every recompiled function with "n64_" (TOML ignore lists use these names).
python3 -I - "$SRC/src/elf.cpp" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
marker = "/* firetv-native: sanitize + prefix names */"
if marker not in s:
    anchor = "                    if (num_instructions > 0) {\n                        context.section_functions[section_index].push_back(context.functions.size());"
    assert anchor in s, "elf.cpp anchor not found"
    patch = ("                    " + marker + "\n"
             "                    for (char& ch : name) { if (ch == '.') ch = '_'; }\n"
             "                    if (name.rfind(\"n64_\", 0) != 0) name = \"n64_\" + name;\n\n")
    s = s.replace(anchor, patch + anchor, 1)
    open(p, "w").write(s)
    print("patched: sanitize + prefix names")
PY

cmake -S "$SRC" -B "$OUT" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
ninja -C "$OUT" N64Recomp
echo "$OUT/N64Recomp"
