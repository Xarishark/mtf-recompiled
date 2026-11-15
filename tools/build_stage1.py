"""Stage-1 compiler & linker smoke test for Phase F1.

Generates a Ninja build file in build/v327680_stage1/ that compiles every
generated C chunk of `main` and `sdk` along with a minimal runtime stub and
links them into build/v327680_stage1/mtf-stage1.exe to verify Gate F1.
"""

import argparse
import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CLANG = ROOT / "toolchain" / "llvm-mingw-20261104-ucrt-x86_64" / "bin" / "clang.exe"
NINJA = ROOT / "toolchain" / "ninja" / "ninja.exe"

GEN_FLAGS = [
    "-O1",
    "-fno-strict-aliasing",
    "-Wno-return-type",
    "-Wno-unused-label",
    "-fvisibility=hidden",
    "-march=x86-64-v3",
]

STUB_C = """\
#include <stdio.h>
#include <stdlib.h>
#include "cpu.h"

extern const uint64_t main_fn_addrs[];
extern const GuestFn main_fn_ptrs[];
extern const size_t main_fn_count;
extern const uint64_t sdk_fn_addrs[];
extern const GuestFn sdk_fn_ptrs[];
extern const size_t sdk_fn_count;

volatile int g_fnprof = 0;
void fnprof_hit(uint64_t addr, uint64_t lr, uint64_t x0) { (void)addr; (void)lr; (void)x0; }
uint64_t rt_ticks(void) { return 0; }

void rt_dispatch(Cpu *c) {
    (void)c;
    abort();
}

void rt_spin_gate(Cpu *c, uint64_t head) {
    (void)c;
    (void)head;
}

void rt_svc(Cpu *c, uint32_t imm, uint64_t pc) {
    (void)c;
    (void)imm;
    (void)pc;
    abort();
}

void rt_brk(Cpu *c, uint32_t imm, uint64_t pc) {
    (void)c;
    fprintf(stderr, "BRK #%u at 0x%llx\\n", imm, (unsigned long long)pc);
    abort();
}

void rt_udf(Cpu *c, uint64_t pc) {
    (void)c;
    fprintf(stderr, "UDF at 0x%llx\\n", (unsigned long long)pc);
    abort();
}

int main(void) {
    printf("MTF Stage-1 link check OK: main=%zu fns, sdk=%zu fns\\n",
           main_fn_count, sdk_fn_count);
    return 0;
}
"""


def npath(p: Path) -> str:
    return p.resolve().as_posix().replace(":", "$:")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--gen", default=str(ROOT / "generated" / "v327680"))
    parser.add_argument("--out", default=str(ROOT / "build" / "v327680_stage1"))
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 8)
    args = parser.parse_args()

    if not CLANG.is_file():
        sys.exit(f"Missing clang at {CLANG}")
    if not NINJA.is_file():
        sys.exit(f"Missing ninja at {NINJA}")

    gen_dir = Path(args.gen)
    out_dir = Path(args.out)
    obj_dir = out_dir / "obj"
    obj_dir.mkdir(parents=True, exist_ok=True)

    stub_path = out_dir / "stage1_stub.c"
    stub_path.write_text(STUB_C, encoding="utf-8")

    sources: list[tuple[Path, str]] = []
    for mod in ("main", "sdk"):
        mod_dir = gen_dir / mod
        for c_file in sorted(mod_dir.glob("*.c")):
            sources.append((c_file, "gen"))
    sources.append((ROOT / "runtime" / "fma.c", "rt"))
    sources.append((stub_path, "rt"))

    inc_rt = (ROOT / "runtime").resolve().as_posix()
    ninja_lines = [
        f"cc = {CLANG.resolve().as_posix()}",
        f"genflags = {' '.join(GEN_FLAGS)} -I\"{inc_rt}\"",
        f"rtflags = -O2 -Wall -march=x86-64-v3 -I\"{inc_rt}\"",
        "",
        "rule cc_gen",
        "  command = $cc $genflags -c $in -o $out",
        "  description = CC(gen) $out",
        "",
        "rule cc_rt",
        "  command = $cc $rtflags -c $in -o $out",
        "  description = CC(rt) $out",
        "",
        "rule link",
        "  command = $cc -fuse-ld=lld -Wl,--stack,16777216 -o $out @$out.rsp",
        "  rspfile = $out.rsp",
        "  rspfile_content = $in",
        "  description = LINK $out",
        "",
    ]

    objs = []
    for src, kind in sources:
        obj = obj_dir / (src.stem + ".o")
        objs.append(obj)
        rule = "cc_gen" if kind == "gen" else "cc_rt"
        ninja_lines.append(f"build {npath(obj)}: {rule} {npath(src)}")

    exe = out_dir / "mtf-stage1.exe"
    ninja_lines.append("")
    ninja_lines.append(f"build {npath(exe)}: link {' '.join(npath(o) for o in objs)}")
    ninja_lines.append(f"default {npath(exe)}")
    ninja_lines.append("")

    ninja_file = out_dir / "build.ninja"
    ninja_file.write_text("\n".join(ninja_lines), encoding="utf-8")

    t0 = time.perf_counter()
    subprocess.check_call([str(NINJA), "-f", str(ninja_file), "-j", str(args.jobs)])
    dt = time.perf_counter() - t0
    print(f"Built {exe} in {dt:.1f}s")
    subprocess.check_call([str(exe)])
    return 0


if __name__ == "__main__":
    sys.exit(main())
