#!/usr/bin/env python3
"""Extract all NVN2 (Switch 2 Ampere) BNSH shaders from rom:/42 and nro0 (MTF Recomp, MIT).

Scans all non-media entries in Camelot's rom:/42 (via cra.py), unpacks NARC/SARC
containers and Zstd (0x5A) / LZ4-block (0x94) payloads, locates every BNSH
('BNSH\\0\\0\\0\\0' + 'grsc' at +0x60) across shader/, ssg_system/, model/,
effect/, layout/, and nro0, and writes:
  - <out_dir>/shaders.bin      (deduplicated shader records: key, stage, ctl, code)
  - <out_dir>/shader_map.tsv   (key, stage, ctl_len, code_len, source)
  - <out_dir>/pso_pairs.tsv    (vs_key, ps_key pairs for graphics programs)
"""

from __future__ import annotations

import argparse
import hashlib
import os
import struct
import sys
import time
import zlib
from pathlib import Path

import zstandard as zstd

sys.path.insert(0, str(Path(__file__).resolve().parent))
from cra import CraArchive

STAGES = ("vs", "hs", "ds", "gs", "ps", "cs")
CTL_MAGIC = 0x98761234
CTL_VERSION = 0xC9
PACK_MAGIC = b"MTFN2SD1"


def lz4_block_decompress(src: bytes, raw_size: int) -> bytes:
    dst = bytearray()
    i = 0
    n = len(src)
    while i < n and len(dst) < raw_size:
        tok = src[i]
        i += 1
        lit = tok >> 4
        if lit == 15:
            while i < n:
                b = src[i]
                i += 1
                lit += b
                if b != 255:
                    break
        dst.extend(src[i : i + lit])
        i += lit
        if len(dst) >= raw_size or i >= n:
            break
        off = struct.unpack_from("<H", src, i)[0]
        i += 2
        mlen = (tok & 0xF) + 4
        if (tok & 0xF) == 15:
            while i < n:
                b = src[i]
                i += 1
                mlen += b
                if b != 255:
                    break
        pos = len(dst) - off
        for _ in range(mlen):
            dst.append(dst[pos])
            pos += 1
    return bytes(dst[:raw_size])


def maybe_decompress(data: bytes, zctx: zstd.ZstdDecompressor) -> bytes:
    if not data:
        return data
    k = data[0]
    if k == 0x4D and data[:4] == b"MMMM" and len(data) >= 12:
        _, count = struct.unpack_from("<II", data, 4)
        at = (12 + 4 * count + 15) & ~15
        if at <= len(data):
            out = []
            for i in range(count):
                s = struct.unpack_from("<I", data, 12 + 4 * i)[0]
                out.append(maybe_decompress(data[at : at + s], zctx))
                at += (s + 15) & ~15
            return b"".join(out)
    if k == 0x5A and data[:4] == b"Z\x00\x00\x00" and len(data) >= 8:
        return zctx.decompress(data[8:])
    if k == 0x94 and len(data) >= 8:
        raw_size = struct.unpack_from("<I", data, 4)[0]
        return lz4_block_decompress(data[8:], raw_size)
    if k in (0x70, 0x80) and len(data) >= 0x10:
        return data[0x10:]
    if k in (0x00, 0x10, 0x20, 0x30) and len(data) >= 0x10:
        try:
            return zlib.decompress(data[0x10:], -15 if k == 0x30 else 47)
        except zlib.error:
            return data
    return data


def iter_bnsh_blobs(data: bytes, label: str, zctx: zstd.ZstdDecompressor):
    data = maybe_decompress(data, zctx)
    if len(data) < 0x88:
        return
    magic = data[:4]
    if magic == b"NARC":
        btaf_size = struct.unpack_from("<I", data, 0x14)[0]
        count = struct.unpack_from("<I", data, 0x18)[0]
        btnf_off = 0x10 + btaf_size
        btnf_size = struct.unpack_from("<I", data, btnf_off + 4)[0]
        fimg_off = btnf_off + btnf_size + 8
        for i in range(count):
            s, e = struct.unpack_from("<II", data, 0x1C + i * 8)
            yield from iter_bnsh_blobs(data[fimg_off + s : fimg_off + e], f"{label}#{i}", zctx)
        return
    if magic == b"SARC":
        sfat_off = struct.unpack_from("<H", data, 4)[0]
        data_off = struct.unpack_from("<I", data, 0x0C)[0]
        ncnt = struct.unpack_from("<H", data, sfat_off + 6)[0]
        for i in range(ncnt):
            s, e = struct.unpack_from("<II", data, sfat_off + 12 + i * 16 + 8)
            yield from iter_bnsh_blobs(data[data_off + s : data_off + e], f"{label}#s{i}", zctx)
        return

    # Scan for embedded or top-level BNSH\0\0\0\0
    pos = 0
    while True:
        idx = data.find(b"BNSH\x00\x00\x00\x00", pos)
        if idx < 0:
            break
        if idx + 0x88 <= len(data) and data[idx + 0x60 : idx + 0x64] == b"grsc":
            yield data, idx, f"{label}@{idx:#x}"
        pos = idx + 8


def shader_key(ctl: bytes, code: bytes) -> str:
    if len(ctl) >= 0x34:
        klen = struct.unpack_from("<I", ctl, 0x30)[0]
        if 0x48 <= klen <= len(ctl):
            ctl = ctl[:klen]
    return hashlib.sha256(ctl + code).hexdigest()[:16]


def parse_bnsh(data: bytes, base: int, label: str, shaders: dict, pairs: set):
    nvar, pvar = struct.unpack_from("<IQ", data, base + 0x7C)
    for v in range(nvar):
        prog_off = struct.unpack_from("<Q", data, base + pvar + 0x40 * v + 0x10)[0]
        if not prog_off:
            continue
        stage_keys: dict[str, str] = {}
        for si, st in enumerate(STAGES):
            sc = struct.unpack_from("<Q", data, base + prog_off + 8 + 8 * si)[0]
            if not sc:
                continue
            pctl, pcode, ncode, nctl = struct.unpack_from("<QQII", data, base + sc + 8)
            ctl = data[base + pctl : base + pctl + nctl]
            code = data[base + pcode : base + pcode + ncode]
            if len(ctl) < 0x48 or struct.unpack_from("<II", ctl, 0) != (CTL_MAGIC, CTL_VERSION):
                continue
            k = shader_key(ctl, code)
            stage_keys[st] = k
            if (k, st) not in shaders:
                shaders[(k, st)] = (ctl, code, f"{label}#var{v}")
        for off, st, stage_id in ((0x40, "ts", 6), (0x48, "ms", 7)):
            if base + prog_off + off + 8 > len(data):
                continue
            sc = struct.unpack_from("<Q", data, base + prog_off + off)[0]
            if not sc or base + sc + 0x20 > len(data):
                continue
            pctl, pcode, ncode, nctl = struct.unpack_from("<QQII", data, base + sc + 8)
            if not (0x48 <= nctl < 0x10000) or base + pctl + nctl > len(data) or base + pcode + ncode > len(data):
                continue
            ctl = data[base + pctl : base + pctl + nctl]
            code = data[base + pcode : base + pcode + ncode]
            if len(ctl) > 0x56 and struct.unpack_from("<II", ctl, 0) == (CTL_MAGIC, CTL_VERSION) and ctl[0x56] == stage_id:
                k = shader_key(ctl, code)
                stage_keys[st] = k
                if (k, st) not in shaders:
                    shaders[(k, st)] = (ctl, code, f"{label}#var{v}")
        if "vs" in stage_keys and "ps" in stage_keys:
            pairs.add((stage_keys["vs"], stage_keys["ps"]))


def main() -> int:
    ap = argparse.ArgumentParser(description="Extract all NVN2 shaders from rom:/42 and nro0")
    ap.add_argument("--nro", required=True, help="Path to nro0.nro")
    ap.add_argument("--rom42", required=True, help="Path to romfs/42")
    ap.add_argument("--out", required=True, help="Output directory (e.g. build/v327680/shaders-nvn2)")
    args = ap.parse_args()

    t0 = time.time()
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    zctx = zstd.ZstdDecompressor()
    arc = CraArchive(args.nro, args.rom42)

    shaders: dict[tuple[str, str], tuple[bytes, bytes, str]] = {}
    pairs: set[tuple[str, str]] = set()

    skip_ext = (".bfstm", ".webm", ".bfsar", ".ntex", ".bin", ".dat", ".cbin", ".rx")
    scanned = 0
    for idx, name, size, _ in arc.iter_entries():
        if size == 0 or name.endswith(skip_ext):
            continue
        raw = arc.read_entry(idx, verify_oracle=False)
        scanned += 1
        for blob, base, label in iter_bnsh_blobs(raw, name, zctx):
            parse_bnsh(blob, base, label, shaders, pairs)

    # Scan nro0 for embedded BNSH
    nro_bytes = Path(args.nro).read_bytes()
    for blob, base, label in iter_bnsh_blobs(nro_bytes, "nro0", zctx):
        parse_bnsh(blob, base, label, shaders, pairs)

    # Write packed shaders.bin
    pack_path = out_dir / "shaders.bin"
    map_path = out_dir / "shader_map.tsv"
    pairs_path = out_dir / "pso_pairs.tsv"

    items = sorted(shaders.items(), key=lambda kv: (kv[0][1], kv[0][0]))
    with open(pack_path, "wb") as pf, open(map_path, "w", encoding="utf-8") as mf:
        pf.write(struct.pack("<8sII", PACK_MAGIC, 1, len(items)))
        mf.write("key\tstage\tctl_len\tcode_len\tsource\n")
        for (k, st), (ctl, code, src) in items:
            k_b = k.encode("ascii")
            st_b = st.encode("ascii")
            src_b = src.encode("utf-8")
            pf.write(struct.pack("<16s4sIII", k_b, st_b, len(ctl), len(code), len(src_b)))
            pf.write(ctl)
            pf.write(code)
            pf.write(src_b)
            mf.write(f"{k}\t{st}\t{len(ctl)}\t{len(code)}\t{src}\n")

    with open(pairs_path, "w", encoding="utf-8") as pf:
        pf.write("vs_key\tps_key\n")
        for vs_k, ps_k in sorted(pairs):
            pf.write(f"{vs_k}\t{ps_k}\n")

    by_stage: dict[str, int] = {}
    for _, st in shaders:
        by_stage[st] = by_stage.get(st, 0) + 1
    dt = time.time() - t0
    print(f"Scanned {scanned} rom:/42 entries + nro0 in {dt:.1f}s")
    print(f"Extracted {len(items)} unique shaders ({len(pairs)} vs/ps pairs): {by_stage}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
