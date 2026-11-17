"""Find the vertex inputs that are only used as integers and write them as translation answers ({location: uint}).

Maxwell/Ampere do no type conversion when reading vertex attribute registers: the bits of an integer format (bone
indices in R8UI / RG8UI / RGBA8UI) reach the shader as they are, and the shader uses them as integers. Without PC
reflection the translator declares every vertex input float4, so an integer format is converted to float on the host
and every bone index is garbage: skinned models (the players) are thrown off screen or explode. The runtime learns
such inputs one shader at a time (answers.tsv); this finds them in every translated vertex shader at once.

An input counts as integer when, after spirv-opt --ssa-rewrite, every value loaded from it (through access chains,
extracts, shuffles, copies and phis) is used only by OpBitcast to an integer type.
Contains adapted code under the MIT licence (THIRD_PARTY_NOTICES.md).

Usage: int_inputs.py [--spv build/generated/shaders/spv] [--out build/generated/shaders/answers-static.tsv]
       then: translate.py --answers <the merged file>
"""

from __future__ import annotations

import argparse
import concurrent.futures as cf
import json
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SPIRV_OPT = ROOT / "build" / "toolchain" / "msys64" / "ucrt64" / "bin" / "spirv-opt.exe"

OP_NAME, OP_DECORATE, OP_TYPE_INT, OP_TYPE_FLOAT, OP_TYPE_VECTOR, OP_TYPE_POINTER = 5, 71, 21, 22, 23, 32
OP_VARIABLE, OP_LOAD, OP_ACCESS_CHAIN, OP_IN_BOUNDS_ACCESS_CHAIN = 59, 61, 65, 66
OP_VECTOR_SHUFFLE, OP_COMPOSITE_EXTRACT, OP_COPY_OBJECT, OP_BITCAST, OP_PHI = 79, 81, 83, 124, 245
DEC_LOCATION, SC_INPUT = 30, 1


def parse(words):
    """{id: (opcode, operands)} for instructions with a result id, the instruction list and the result-type map"""
    insts = []
    i = 5
    while i < len(words):
        op, n = words[i] & 0xFFFF, words[i] >> 16
        if not n:
            break
        insts.append((op, words[i + 1:i + n]))
        i += n
    return insts


def analyse(words) -> dict:
    insts = parse(words)
    types, location, var_type = {}, {}, {}
    result_of = {}   # id -> (op, result type, operands after the result id)
    for op, a in insts:
        if op in (OP_TYPE_INT, OP_TYPE_FLOAT):
            types[a[0]] = ("int" if op == OP_TYPE_INT else "float", a[1])
        elif op == OP_TYPE_VECTOR:
            types[a[0]] = ("vec", a[1], a[2])
        elif op == OP_TYPE_POINTER:
            types[a[0]] = ("ptr", a[1], a[2])
        elif op == OP_DECORATE and len(a) >= 3 and a[1] == DEC_LOCATION:
            location[a[0]] = a[2]
        elif op == OP_VARIABLE and a[2] == SC_INPUT:
            var_type[a[1]] = a[0]
    # producers and users: instructions whose result type then result id come first
    has_result = {OP_LOAD, OP_ACCESS_CHAIN, OP_IN_BOUNDS_ACCESS_CHAIN, OP_VECTOR_SHUFFLE, OP_COMPOSITE_EXTRACT,
                  OP_COPY_OBJECT, OP_BITCAST, OP_PHI}
    uses = {}
    for op, a in insts:
        if op == OP_VARIABLE:   # (its own definition, not a use)
            continue
        if op in has_result and len(a) >= 2:
            result_of[a[1]] = (op, a[0], a[2:])
        # every id operand of every instruction counts as a use (over-approximate: unknown uses make an input "bad")
        ops = a[2:] if op in has_result else a
        for x in ops:
            uses.setdefault(x, []).append((op, a))

    def is_int_type(t):
        ty = types.get(t)
        if not ty:
            return False
        if ty[0] == "int":
            return True
        return ty[0] == "vec" and types.get(ty[1], ("",))[0] == "int"

    out = {}
    for var, ptype in var_type.items():
        loc = location.get(var)
        pt = types.get(ptype)
        if loc is None or not pt or pt[0] != "ptr":
            continue
        pointee = types.get(pt[2])
        # float4 inputs only (an input already declared uint was answered before)
        if not pointee or pointee[0] != "vec" or types.get(pointee[1], ("",))[0] != "float":
            continue
        work, seen, nint, bad = [var], set(), 0, False
        while work and not bad:
            v = work.pop()
            if v in seen:
                continue
            seen.add(v)
            for op, a in uses.get(v, []):
                if op in (OP_ACCESS_CHAIN, OP_IN_BOUNDS_ACCESS_CHAIN, OP_LOAD, OP_VECTOR_SHUFFLE, OP_COMPOSITE_EXTRACT,
                          OP_COPY_OBJECT, OP_PHI):
                    if op in (OP_ACCESS_CHAIN, OP_IN_BOUNDS_ACCESS_CHAIN) and a[2] != v:
                        bad = True   # used as an index: not a plain read of the input
                        break
                    work.append(a[1])
                elif op == OP_BITCAST and is_int_type(a[0]):
                    nint += 1
                elif op == OP_NAME or op == OP_DECORATE:
                    continue
                elif op == 15:   # OpEntryPoint interface list
                    continue
                else:
                    bad = True
                    break
        if not bad and nint:
            out[loc] = "uint"
    return out


def one(spv: Path, temp: Path) -> tuple:
    key = spv.stem
    res = spv.with_suffix("").with_suffix(".res.json")
    try:
        if json.loads(res.read_text(encoding="utf-8")).get("stage") != "vs":
            return key, {}
    except (OSError, ValueError):
        return key, {}
    ssa = temp / f"{key}.ssa.spv"
    r = subprocess.run([str(SPIRV_OPT), "--private-to-local", "--ssa-rewrite", "--eliminate-dead-code-aggressive", str(spv), "-o", str(ssa)], capture_output=True)
    if r.returncode or not ssa.is_file():
        return key, {}
    data = ssa.read_bytes()
    ssa.unlink()
    return key, analyse(list(struct.unpack(f"<{len(data) // 4}I", data)))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--spv", type=Path, default=ROOT / "build" / "generated" / "shaders" / "spv")
    ap.add_argument("--out", type=Path, default=ROOT / "build" / "generated" / "shaders" / "answers-static.tsv")
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 2))
    ap.add_argument("keys", nargs="*")
    a = ap.parse_args()
    if not SPIRV_OPT.is_file():
        sys.exit(f"{SPIRV_OPT} is missing")
    files = [p for p in a.spv.glob("*.spv") if not p.name.endswith(".reader.spv")]
    if a.keys:
        files = [p for p in files if p.stem in set(a.keys)]
    found = {}
    with tempfile.TemporaryDirectory(prefix="mtf-int-", dir=a.out.parent) as t, cf.ThreadPoolExecutor(a.jobs) as pool:
        for n, (key, locs) in enumerate(pool.map(lambda p: one(p, Path(t)), files), 1):
            if locs:
                found[key] = locs
            if n % 2000 == 0:
                print(f"\r{n}/{len(files)}, {len(found)} vertex shaders with integer inputs", end="", flush=True)
    lines = [f"{k}\tattr\t{loc}\t{ty}" for k in sorted(found) for loc, ty in sorted(found[k].items())]
    a.out.write_text("# key\tattr\tlocation\ttype (tools/shaders/int_inputs.py)\n" + "\n".join(lines) + "\n",
                     encoding="utf-8")
    print(f"\n{len(found)} vertex shaders, {len(lines)} integer inputs -> {a.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
