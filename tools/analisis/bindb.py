# Adapted from mkw-recomp tools/analisis/bindb.py (MIT).
"""Build build/bindb.sqlite: functions, symbols, imports, calls, data references and strings
of every loaded module (main, sdk, nro0).

Usage: python tools/analisis/bindb.py [--nro nro0=build/nro/<sha256>.nro] [--out build/bindb.sqlite]
"""

import argparse
import re
import sqlite3
import subprocess
import sys
import time
import tomllib
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "recomp"))

from layout import layout  # noqa: E402
from module import Module  # noqa: E402
from nso import R_AARCH64_ABS64, R_AARCH64_GLOB_DAT, R_AARCH64_JUMP_SLOT, R_AARCH64_RELATIVE  # noqa: E402

CXXFILT = ROOT / "toolchain" / "llvm-mingw-20261104-ucrt-x86_64" / "bin" / "llvm-cxxfilt.exe"
LOOKAHEAD = 4
STRING = re.compile(rb"[\x20-\x7e\t\n]{4,}\x00")

SCHEMA = """
CREATE TABLE module(name TEXT PRIMARY KEY, base INT, size INT, text_lo INT, text_hi INT, build_id TEXT);
CREATE TABLE func(addr INT PRIMARY KEY, module TEXT, end INT, name TEXT);
CREATE TABLE sym(module TEXT, addr INT, size INT, name TEXT, demangled TEXT, kind TEXT);
CREATE TABLE plt(addr INT PRIMARY KEY, module TEXT, got INT, name TEXT, target INT);
CREATE TABLE call(site INT, caller INT, target INT, kind TEXT);
CREATE TABLE ref(site INT, func INT, target INT, kind TEXT);
CREATE TABLE str(addr INT PRIMARY KEY, module TEXT, text TEXT);
CREATE TABLE svc(site INT, func INT, num INT);
CREATE TABLE fptr(slot INT PRIMARY KEY, module TEXT, target INT, vtable INT);
"""
INDEXES = """
CREATE INDEX call_target ON call(target);
CREATE INDEX call_caller ON call(caller);
CREATE INDEX ref_target ON ref(target);
CREATE INDEX ref_func ON ref(func);
CREATE INDEX sym_name ON sym(name);
CREATE INDEX sym_addr ON sym(addr);
CREATE INDEX plt_name ON plt(name);
CREATE INDEX svc_func ON svc(func);
CREATE INDEX fptr_target ON fptr(target);
CREATE INDEX fptr_vtable ON fptr(vtable);
"""


def default_exefs() -> Path:
    cfg = tomllib.loads((ROOT / "config.local.toml").read_text(encoding="utf-8"))
    return Path(cfg["game"]["dump_path"]) / "updates" / str(cfg["game"]["update_version"]) / "exefs"


def sext(v: np.ndarray, bits: int) -> np.ndarray:
    v = v.astype(np.int64)
    return np.where(v & (1 << (bits - 1)), v - (1 << bits), v)


def demangle(names: list[str]) -> dict[str, str]:
    if not names or not CXXFILT.is_file():
        return {n: n for n in names}
    res = subprocess.run([str(CXXFILT)], input="\n".join(names), capture_output=True, text=True, check=True)
    return dict(zip(names, res.stdout.splitlines(), strict=False))


def owner(starts: np.ndarray, sites: np.ndarray) -> np.ndarray:
    idx = np.searchsorted(starts, sites, side="right") - 1
    return starts[np.clip(idx, 0, None)]


def branches(words: np.ndarray, pcs: np.ndarray) -> list[tuple[int, int, str]]:
    out = []
    for opcode, kind in ((0x94000000, "bl"), (0x14000000, "b")):
        m = (words & 0xFC000000) == opcode
        targets = pcs[m] + sext(words[m] & 0x3FFFFFF, 26) * 4
        out += [(int(s), int(t), kind) for s, t in zip(pcs[m], targets, strict=True)]
    return out


def adrp_refs(words: np.ndarray, pcs: np.ndarray) -> list[tuple[int, int, str]]:
    adrp = np.nonzero((words & 0x9F000000) == 0x90000000)[0]
    w = words[adrp].astype(np.int64)
    imm = sext(((w >> 5) & 0x7FFFF) << 2 | ((w >> 29) & 3), 21) << 12
    page = (pcs[adrp] & ~0xFFF) + imm
    rd = w & 31
    out, done = [], np.zeros(len(adrp), bool)
    for k in range(1, LOOKAHEAD + 1):
        j = np.minimum(adrp + k, len(words) - 1)
        nxt = words[j].astype(np.int64)
        rn = (nxt >> 5) & 31
        add = ((nxt & 0xFF800000) == 0x91000000) & (rn == rd)
        ldst = ((nxt & 0x3B000000) == 0x39000000) & (rn == rd)
        scale = np.where((nxt >> 26) & 1 & (nxt >> 23) & 1 & ((nxt >> 30) == 0), 4, nxt >> 30)
        for m, off, kind in ((add, (nxt >> 10) & 0xFFF, "add"), (ldst, ((nxt >> 10) & 0xFFF) << scale, "ldst")):
            m = m & ~done
            out += [(int(s), int(t), kind) for s, t in zip(pcs[j[m]], page[m] + off[m], strict=True)]
            done |= m
    return out


def plt_stubs(words: np.ndarray, pcs: np.ndarray) -> list[tuple[int, int]]:
    n = len(words) - 3
    w0, w1, w2, w3 = (words[i : i + n].astype(np.int64) for i in range(4))
    m = (
        ((w0 & 0x9F00001F) == 0x90000010)
        & ((w1 & 0xFFC003FF) == 0xF9400211)
        & ((w2 & 0xFFC003FF) == 0x91000210)
        & (w3 == 0xD61F0220)
    )
    imm = sext(((w0[m] >> 5) & 0x7FFFF) << 2 | ((w0[m] >> 29) & 3), 21) << 12
    got = (pcs[:n][m] & ~0xFFF) + imm + (((w1[m] >> 10) & 0xFFF) << 3)
    return [(int(a), int(g)) for a, g in zip(pcs[:n][m], got, strict=True)]


def fptrs(db: sqlite3.Connection, mods: dict[str, Module], exported: dict[str, int]) -> None:
    starts = {a for (a,) in db.execute("SELECT addr FROM func")}
    rows = []
    for name, m in mods.items():
        n = m.nso
        for off, typ, sym, add in n.relocs:
            if typ == R_AARCH64_RELATIVE:
                target = m.base + add
            elif typ in (R_AARCH64_ABS64, R_AARCH64_GLOB_DAT) and sym:
                target = exported.get(n.syms[sym][0], 0) + add
            else:
                continue
            slot = m.base + off
            if target in starts and not m.lo <= slot < m.hi:
                rows.append((slot, name, target))
    rows.sort()
    out, run = [], 0
    for i, (slot, name, target) in enumerate(rows):
        if not i or slot != rows[i - 1][0] + 8:
            run = slot
        out.append((slot, name, target, run))
    db.executemany("INSERT INTO fptr VALUES (?,?,?,?)", out)


def build(exefs: Path, nros: list[tuple[str, Path]], out: Path) -> None:
    mod_layout = layout(exefs, nros)
    nro_map = dict(nros)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.unlink(missing_ok=True)
    db = sqlite3.connect(out)
    db.executescript(SCHEMA)
    mods = {
        name: Module(name, nro_map.get(name, exefs / name), base, simd="c")
        for name, base, _ in mod_layout
    }
    names = {s[0] for m in mods.values() for s in m.nso.syms if s[0]}
    pretty = demangle(sorted(names))
    exported = {}
    for name, m in mods.items():
        for sname, info, shndx, value, size in m.nso.syms:
            if sname and shndx:
                exported.setdefault(sname, m.base + value)
    for name, m in mods.items():
        t0 = time.time()
        n = m.nso
        db.execute("INSERT INTO module VALUES (?,?,?,?,?,?)", (name, m.base, len(m.image), m.lo, m.hi, n.build_id))
        rows = []
        for sname, info, shndx, value, size in n.syms:
            if not sname:
                continue
            kind = "import" if not shndx else ("func" if info & 0xF == 2 else "object")
            rows.append((name, m.base + value if shndx else None, size, sname, pretty.get(sname, sname), kind))
        db.executemany("INSERT INTO sym VALUES (?,?,?,?,?,?)", rows)
        symname = {m.base + v: pretty.get(s, s) for s, info, sh, v, _ in n.syms if s and sh and info & 0xF == 2}
        starts = np.array(sorted(set(m.seeds()) | set(symname)), dtype=np.int64)
        ends = np.append(starts[1:], m.hi)
        db.executemany(
            "INSERT INTO func VALUES (?,?,?,?)",
            ((int(a), name, int(e), symname.get(int(a))) for a, e in zip(starts, ends, strict=True)),
        )
        got_name = {
            m.base + off: n.syms[sym][0]
            for off, typ, sym, _ in n.relocs
            if typ in (R_AARCH64_JUMP_SLOT, R_AARCH64_GLOB_DAT) and sym
        }
        words = m.words.astype(np.int64)
        pcs = m.lo + 4 * np.arange(len(words), dtype=np.int64)
        stubs = plt_stubs(words, pcs)
        db.executemany(
            "INSERT INTO plt VALUES (?,?,?,?,?)",
            (
                (a, name, g, pretty.get(got_name.get(g, ""), got_name.get(g)), exported.get(got_name.get(g, "")))
                for a, g in stubs
            ),
        )
        calls = branches(words, pcs)
        sites = np.array([c[0] for c in calls], dtype=np.int64)
        callers = owner(starts, sites) if len(calls) else []
        db.executemany(
            "INSERT INTO call VALUES (?,?,?,?)",
            ((s, int(f), t, k) for (s, t, k), f in zip(calls, callers, strict=True)),
        )
        refs = adrp_refs(words, pcs)
        rsites = np.array([r[0] for r in refs], dtype=np.int64)
        funcs = owner(starts, rsites) if len(refs) else []
        db.executemany(
            "INSERT INTO ref VALUES (?,?,?,?)", ((s, int(f), t, k) for (s, t, k), f in zip(refs, funcs, strict=True))
        )
        svc = np.nonzero((words & 0xFFE0001F) == 0xD4000001)[0]
        if len(svc):
            sites = pcs[svc]
            rows = zip(
                sites.tolist(), owner(starts, sites).tolist(), ((words[svc] >> 5) & 0xFFFF).tolist(), strict=True
            )
            db.executemany("INSERT INTO svc VALUES (?,?,?)", rows)
        ro, ro_size = n.rodata
        blob = bytes(m.image[ro : ro + ro_size])
        db.executemany(
            "INSERT INTO str VALUES (?,?,?)",
            ((m.base + ro + s.start(), name, s.group()[:-1].decode()) for s in STRING.finditer(blob)),
        )
        print(
            f"{name}: {len(starts):,} functions, {len(calls):,} branches, {len(refs):,} refs, {len(stubs):,} PLT stubs,"
            f" {time.time() - t0:.1f}s"
        )
    db.executescript(INDEXES)
    db.commit()
    fptrs(db, mods, exported)
    db.commit()
    for table in ("func", "sym", "plt", "call", "ref", "str", "svc", "fptr"):
        print(f"{table}: {db.execute(f'SELECT count(*) FROM {table}').fetchone()[0]:,}")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--exefs", type=Path, default=None)
    ap.add_argument("--nro", action="append", default=[])
    ap.add_argument("--out", type=Path, default=ROOT / "build/bindb.sqlite")
    args = ap.parse_args()
    exefs = args.exefs or default_exefs()
    nros = []
    for spec in args.nro:
        n, p = spec.split("=", 1)
        nros.append((n, Path(p)))
    if not nros:
        for p in sorted((ROOT / "build/nro").glob("*.nro")):
            nros.append(("nro0", p))
            break
    build(exefs, nros, args.out)


if __name__ == "__main__":
    main()
