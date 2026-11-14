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

# Patch 5: load-linked / store-conditional (ll, lld, sc, scd). Recompiled code runs on one host
# thread, so LL is a plain load and SC a plain store that always succeeds (rt = 1).
python3 -I - "$SRC/src/recompilation.cpp" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
marker = "/* firetv-native: ll/sc */"
if marker not in s:
    anchor = "    case InstrId::cpu_break:\n"
    patch = r'''    __MARKER__
    case InstrId::cpu_ll: case InstrId::cpu_lld: case InstrId::cpu_sc: case InstrId::cpu_scd: {
        const int16_t llsc_off = (int16_t)instr.Get_immediate();
        const std::string llsc_base = rs == 0 ? std::string("0") : fmt::format("ctx->r{}", rs);
        const std::string llsc_val = rt == 0 ? std::string("0") : fmt::format("ctx->r{}", rt);
        print_indent();
        if (instr_id == InstrId::cpu_ll) {
            if (rt != 0) fmt::print(output_file, "ctx->r{} = MEM_W({:#X}, {});\n", rt, llsc_off, llsc_base);
        } else if (instr_id == InstrId::cpu_lld) {
            if (rt != 0) fmt::print(output_file, "ctx->r{} = LD({:#X}, {});\n", rt, llsc_off, llsc_base);
        } else {
            if (instr_id == InstrId::cpu_sc) fmt::print(output_file, "MEM_W({:#X}, {}) = (int32_t){};\n", llsc_off, llsc_base, llsc_val);
            else fmt::print(output_file, "SD({}, {:#X}, {});\n", llsc_val, llsc_off, llsc_base);
            if (rt != 0) { print_indent(); fmt::print(output_file, "ctx->r{} = 1;\n", rt); }
        }
        break;
    }
'''.replace("__MARKER__", marker)
    assert anchor in s
    s = s.replace(anchor, patch + anchor, 1)
    open(p, "w").write(s)
    print("patched: ll/sc")
PY

# Patch 6: a function containing an instruction we can't translate (e.g. libdragon's emulator-only
# debug opcode in emux_write) becomes a stub that reports itself if it is ever called, instead of
# aborting the whole recompile. Each function is recompiled into a buffer first.
python3 -I - "$SRC/src/main.cpp" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
marker = "/* firetv-native: stub untranslatable functions */"
if marker not in s:
    old = "                result = N64Recomp::recompile_function(context, i, current_output_file, static_funcs_by_section, false);\n"
    assert old in s, "main.cpp anchor not found"
    new = r'''                __MARKER__
                std::ostringstream func_stream;
                result = N64Recomp::recompile_function(context, i, func_stream, static_funcs_by_section, false);
                if (result) {
                    current_output_file << func_stream.str();
                } else {
                    fmt::print(stderr, "[firetv-native] {} could not be recompiled; emitting a stub\n", func.name);
                    fmt::print(current_output_file, "RECOMP_FUNC void {}(uint8_t* rdram, recomp_context* ctx) {{\n    n64rt_unrecompilable(\"{}\", ctx);\n}}\n", func.name, func.name);
                    result = true;
                }
'''.replace("__MARKER__", marker)
    s = s.replace(old, new, 1)
    if "#include <sstream>" not in s:
        s = "#include <sstream>\n" + s
    open(p, "w").write(s)
    print("patched: stub untranslatable functions")
PY

# Patch 7: trunc.l.s / trunc.l.d (float -> 64-bit int). The generator already has the operations;
# only the instruction table entries are missing.
python3 -I - "$SRC/src/operations.cpp" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
marker = "/* firetv-native: trunc.l */"
if marker not in s:
    anchor = "        { InstrId::cpu_trunc_w_d, { UnaryOpType::TruncateWFromD, Operand::FdU32L,   Operand::FsDouble, true } },\n"
    assert anchor in s, "operations.cpp anchor not found"
    patch = ("        " + marker + "\n"
             "        { InstrId::cpu_trunc_l_s, { UnaryOpType::TruncateLFromS, Operand::FdU64,    Operand::Fs,       true } },\n"
             "        { InstrId::cpu_trunc_l_d, { UnaryOpType::TruncateLFromD, Operand::FdU64,    Operand::FsDouble, true } },\n")
    s = s.replace(anchor, anchor + patch, 1)
    open(p, "w").write(s)
    print("patched: trunc.l")
PY

# Patch 8: libdragon's EMUX opcodes (COP0, CO=1, funct 0x20-0x2F: emulator logging/profiling hooks;
# real hardware ignores them). XDETECT reports "no emulator" (rd = 0); the rest are no-ops.
# Also make the unhandled-instruction error say where.
python3 -I - "$SRC/src/recompilation.cpp" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
marker = "/* firetv-native: emux */"
if marker not in s:
    anchor = "    if (!handled) {\n        fmt::print(stderr, \"Unhandled instruction: {}\\n\", instr.getOpcodeName());\n"
    assert anchor in s, "unhandled anchor not found"
    patch = r'''    __MARKER__
    if (!handled) {
        const uint32_t emux_raw = instr.getRaw();
        if ((emux_raw >> 26) == 0x10 && ((emux_raw >> 25) & 1) && (emux_raw & 0x3F) >= 0x20 && (emux_raw & 0x3F) <= 0x2F) {
            const int emux_rd = (emux_raw >> 20) & 0x1F;
            print_indent();
            if ((emux_raw & 0x3F) == 0x20 && emux_rd != 0) fmt::print(output_file, "ctx->r{} = 0; /* emux xdetect: none */\n", emux_rd);
            else fmt::print(output_file, "/* emux op {:08X} ignored */\n", emux_raw);
            handled = true;
        }
    }
'''.replace("__MARKER__", marker)
    s = s.replace(anchor, patch + anchor.replace(
        'fmt::print(stderr, "Unhandled instruction: {}\\n", instr.getOpcodeName());',
        'fmt::print(stderr, "Unhandled instruction: {} at {:08X} (word {:08X})\\n", instr.getOpcodeName(), instr_vram, instr.getRaw());'), 1)
    open(p, "w").write(s)
    print("patched: emux")
PY

# Patch 9: jump tables whose base address GCC hoisted into a callee-saved register. GCC loads
# table bases (lui/addiu) into s0-s7 once at function entry; N64Recomp's jump-table scan is linear,
# so an early-return epilogue in the middle of the function (`ld s1, 80(sp)`) wiped that state and
# every switch placed after it became a bogus tail call ("no recompiled function at <mid-function
# address>"; libxm's xm_tick has five). Callee-saved registers are only reloaded from the stack in
# epilogues, so a tracked constant survives such a restore.
python3 -I - "$SRC/src/analysis.cpp" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
marker = "/* firetv-native: hoisted jump-table bases */"
if marker not in s:
    anchor = "    case InstrId::cpu_lw:\n"
    assert anchor in s and "case InstrId::cpu_ld:" not in s, "analysis.cpp changed; update tools/n64-tools.sh"
    keep = ("if (base == (int)RegId::GPR_O32_sp && ((rt >= 16 && rt <= 23) || rt == 30) && "
            "reg_states[rt].valid_lui) break;")
    patch = ("    " + marker + "\n"
             "    case InstrId::cpu_ld:\n"
             "        " + keep + "\n"
             "        reg_states[rt].invalidate();\n"
             "        break;\n"
             + anchor +
             "        " + keep + "\n")
    s = s.replace(anchor, patch, 1)
    open(p, "w").write(s)
    print("patched: hoisted jump-table bases")
PY

cmake -S "$SRC" -B "$OUT" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
ninja -C "$OUT" N64Recomp
echo "$OUT/N64Recomp"
