"""Extract every shader of MTF 1.1.1 into build/generated/shaders/src.

The game's files are one Camelot archive, rom:/42, whose file table only the decoded NRO holds (cra.py). Shaders are
BNSH binaries inside the FSHA, VFXB and SARC members of the archive's NARC containers, plus the ones embedded in the
NRO itself. Each shader is an NVN2 (Switch 2) control section and an Ampere code area. A shader is named by its key,
the first 16 hex digits of SHA-256 over the control section's first 0x48 bytes (+0x30..+0x34 zeroed) and the code
area: the same key the runtime computes when the game hands the shader to NVN.

Contains adapted code under the MIT licence (THIRD_PARTY_NOTICES.md): bnsh_shaders, nvn2_ctl, key_ctl, skey,
nvn2_stage and the CRA scan.

Output: <key>.ctl and <key>.code per shader, and shaders.tsv (key, stage, first source).
"""

from __future__ import annotations

import argparse
import hashlib
import multiprocessing
import os
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import cra  # noqa: E402

PROJECT_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_ARCHIVE = PROJECT_ROOT / "build" / "content" / "0400D4201C6EA000" / "updates" / "327680" / "romfs" / "42"
DEFAULT_NRO = PROJECT_ROOT / "build" / "generated" / "runtime" / "0400D4201C6EA000" / "1.1.1" / "nro-captured.nro"
DEFAULT_OUT = PROJECT_ROOT / "build" / "generated" / "shaders" / "src"

CTL_MAGIC = bytes.fromhex("34127698")
NVN2_KEY_CTL = 0x48
STAGES = ["vs", "hs", "ds", "gs", "ps", "cs"]
SPH_STAGE = {1: "vs", 2: "hs", 3: "ds", 4: "gs", 5: "ps"}
SKIP_MAGIC = (b"FSTM", b"\x1aE\xdf\xa3")   # sound streams and WebM movies hold no shaders
MAX_DEPTH = 8


def nvn2_ctl(ctl: bytes) -> bool:
    return len(ctl) >= NVN2_KEY_CTL and ctl[:4] == CTL_MAGIC and ctl[4:8] == b"\xc9\0\0\0"


def skey(ctl: bytes, code: bytes) -> str:
    c = ctl[:NVN2_KEY_CTL]
    c = c[:0x30] + bytes(4) + c[0x34:]
    return hashlib.sha256(c + code).hexdigest()[:16]


def nvn2_stage(ctl: bytes, code: bytes) -> str | None:
    """Compute, task and mesh by the control section's stage byte (+0x56: 5, 6, 7); otherwise the SPH at the code area's
    start: shader type in bits 10..13 of word 0."""
    if len(ctl) > 0x56 and ctl[0x56] in (5, 6, 7):
        return {5: "cs", 6: "ts", 7: "ms"}[ctl[0x56]]
    if len(code) < 4:
        return None
    w = struct.unpack_from("<I", code, 0)[0]
    return "vs" if (w >> 10) & 0xF == 0 and w & 0x1F == 1 else SPH_STAGE.get((w >> 10) & 0xF)


def bnsh_shaders(d: bytes, base: int, label: str):
    if d[base + 0x60:base + 0x64] != b"grsc":
        return
    nvar, pvar = struct.unpack_from("<IQ", d, base + 0x7C)
    for v in range(nvar):
        prog = struct.unpack_from("<Q", d, base + pvar + 0x40 * v + 0x10)[0]
        if not prog:
            continue
        slots = [(8 + 8 * i, st) for i, st in enumerate(STAGES)] + [(0x40, "ts"), (0x48, "ms")]
        for off, st in slots:
            if base + prog + off + 8 > len(d):
                continue
            sc = struct.unpack_from("<Q", d, base + prog + off)[0]
            if not sc or base + sc + 0x20 > len(d):
                continue
            pctl, pcode, ncode, nctl = struct.unpack_from("<QQII", d, base + sc + 8)
            if base + pctl + nctl > len(d) or base + pcode + ncode > len(d):
                continue
            ctl, code = d[base + pctl:base + pctl + nctl], d[base + pcode:base + pcode + ncode]
            if not nvn2_ctl(ctl) or struct.unpack_from("<I", ctl, 0x44)[0] != len(code):
                continue
            if st in ("ts", "ms") and (len(ctl) <= 0x56 or ctl[0x56] != (6 if st == "ts" else 7)):
                continue
            yield nvn2_stage(ctl, code) or st, ctl, code, f"{label}#BNSH@{base:#x}#var{v}"


def sarc_members(d: bytes):
    if len(d) < 0x20 or d[:4] != b"SARC":
        return
    # SARC: magic, u16 header length (0x14), u16 byte order mark, u32 file size, u32 data offset; then SFAT: magic,
    # u16 header length (0xC), u16 node count, u32 hash key, and 16-byte nodes {name hash, attributes, start, end}
    data = struct.unpack_from("<I", d, 0xC)[0]
    sfat = struct.unpack_from("<H", d, 4)[0]
    if d[sfat:sfat + 4] != b"SFAT":
        return
    count = struct.unpack_from("<H", d, sfat + 6)[0]
    if sfat + 0xC + 16 * count > len(d):
        return
    for i in range(count):
        start, end = struct.unpack_from("<II", d, sfat + 0xC + 16 * i + 8)
        if data + end <= len(d) and start <= end:
            yield i, d[data + start:data + end]


def scan(d: bytes, label: str, out: dict, depth: int = 0) -> None:
    if depth > MAX_DEPTH or not d:
        return
    if d[:4] == b"NARC":
        try:
            members = cra.narc_members(d, label)
        except cra.Error:
            members = []
        for i, m in enumerate(members):
            scan(cra.undo(m), f"{label}/{i}", out, depth + 1)
        return
    if d[:4] == b"SARC":
        for i, m in sarc_members(d):
            scan(cra.undo(m), f"{label}/{i}", out, depth + 1)
        return
    at = d.find(b"BNSH\0\0\0\0")
    while at >= 0:
        if at + 0x80 <= len(d):
            size = struct.unpack_from("<I", d, at + 0x1C)[0]
            if at + size <= len(d):
                try:
                    for stage, ctl, code, where in bnsh_shaders(d, at, label):
                        key = skey(ctl, code)
                        if key not in out:
                            out[key] = (stage, ctl, code, where)
                except struct.error:   # a BNSH cut short inside its container: its tables point past the end
                    out.setdefault("__malformed__", []).append(f"{label}@{at:#x}")
        at = d.find(b"BNSH\0\0\0\0", at + 8)


def scan_range(args):
    archive, nro_path, entries = args
    out: dict = {}
    with open(archive, "rb") as f:
        for e in entries:
            if cra.head(f, e)[:4] in SKIP_MAGIC:
                continue
            scan(cra.undo(cra.read(f, e)), f"rom:/42/{e[1]}", out)
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--archive", type=Path, default=DEFAULT_ARCHIVE)
    parser.add_argument("--nro", type=Path, default=DEFAULT_NRO)
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    parser.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 2))
    args = parser.parse_args()

    nro = args.nro.read_bytes()
    entries = cra.Index(nro).files()
    total = sum(e[2] for e in entries)
    parts, step, cur, size = [], total / (args.jobs * 8), [], 0
    for e in entries:
        cur.append(e)
        size += e[2]
        if size >= step:
            parts.append((str(args.archive), str(args.nro), cur))
            cur, size = [], 0
    if cur:
        parts.append((str(args.archive), str(args.nro), cur))

    shaders: dict = {}
    malformed: list = []
    scan(nro, "nro", shaders)
    malformed.extend(shaders.pop("__malformed__", []))   # the shaders embedded in the game's code
    with multiprocessing.Pool(args.jobs) as pool:
        for done, found in enumerate(pool.imap_unordered(scan_range, parts), 1):
            for key, value in found.items():
                if key == "__malformed__":
                    malformed.extend(value)
                else:
                    shaders.setdefault(key, value)
            print(f"\r{done}/{len(parts)} archive ranges, {len(shaders)} shaders", end="", flush=True)
    print()

    args.out.mkdir(parents=True, exist_ok=True)
    counts: dict = {}
    with open(args.out / "shaders.tsv", "w", encoding="utf-8") as tsv:
        tsv.write("# key\tstage\tsource (tools/shaders/extract.py)\n")
        for key in sorted(shaders):
            stage, ctl, code, where = shaders[key]
            (args.out / f"{key}.ctl").write_bytes(ctl)
            (args.out / f"{key}.code").write_bytes(code)
            tsv.write(f"{key}\t{stage}\t{where}\n")
            counts[stage] = counts.get(stage, 0) + 1
    print(f"{len(shaders)} shaders: " + ", ".join(f"{n} {s}" for s, n in sorted(counts.items())))
    if malformed:
        print(f"{len(malformed)} BNSH cut short and skipped, first: {malformed[0]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
