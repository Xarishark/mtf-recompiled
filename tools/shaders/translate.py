#!/usr/bin/env python3
"""Offline NVN2 (Switch 2 Ampere) -> SPIR-V shader translator (MTF Recomp, MIT).

Reads build/v327680/shaders-nvn2/shaders.bin (produced by extract.py) and
profiles/0400D4201C6EA000/shader_facts.tsv, and translates every shader stage
to Vulkan SPIR-V via persistent build/xlate/totk_xlate.exe --serve workers.
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
import threading
import time
from pathlib import Path

PACK_MAGIC = b"MTFN2SD1"
NVN2_STAGE_BANK = 25
NVN2_STAGE_INDEX = {"vs": 0, "hs": 1, "ms": 1, "ds": 2, "gs": 3, "ps": 4}


def load_facts(facts_path: Path) -> dict[str, dict]:
    facts: dict[str, dict] = {}
    if not facts_path.is_file():
        return facts
    with open(facts_path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            k, v = line.split("\t", 1)
            facts[k] = json.loads(v)
    return facts


def iter_pack(pack_path: Path):
    with open(pack_path, "rb") as f:
        hdr = f.read(16)
        magic, ver, count = struct.unpack("<8sII", hdr)
        if magic != PACK_MAGIC or ver != 1:
            raise ValueError(f"Unexpected pack header: {magic!r} v{ver}")
        for _ in range(count):
            k_b, st_b, ctl_len, code_len, src_len = struct.unpack("<16s4sIII", f.read(32))
            ctl = f.read(ctl_len)
            code = f.read(code_len)
            src = f.read(src_len).decode("utf-8")
            yield k_b.decode("ascii"), st_b.rstrip(b"\x00").decode("ascii"), ctl, code, src


def build_job(
    key: str,
    stage: str,
    ctl: bytes,
    code_path: str,
    code_len: int,
    facts: dict[str, dict],
) -> dict:
    a, e = struct.unpack_from("<II", ctl, 0x40)
    # Read c1 from the code slice on disk only if present
    c1_hex = ""
    if a < e <= code_len:
        with open(code_path, "rb") as cf_f:
            cf_f.seek(a)
            c1_hex = cf_f.read(e - a).hex()

    job: dict = {
        "key": key,
        "stage": stage,
        "arch": "sm86",
        "code": code_path,
        "debug": False,
    }
    if c1_hex:
        job["c1"] = c1_hex
    if stage in NVN2_STAGE_INDEX:
        job["stage_bank"] = NVN2_STAGE_BANK + NVN2_STAGE_INDEX[stage]
    if stage == "cs" and len(ctl) >= 0xBC:
        job["nvn2_cs"] = [
            ctl[0xB1],
            ctl[0xB2],
            ctl[0xB3],
            struct.unpack_from("<H", ctl, 0xBA)[0],
            ctl[0xA9] * 4,
            ctl[0xB4],
            ctl[0xB5],
            ctl[0xAA] * 4,
        ]
        f_entry = facts.get(key, {})
        if "local" in f_entry:
            job["local"] = f_entry["local"]
        else:
            lx, ly, lz = struct.unpack_from("<III", ctl, 0x58)
            if 1 <= lx <= 1024 and 1 <= ly <= 1024 and 1 <= lz <= 64:
                job["local"] = [lx, ly, lz]
        if "shared" in f_entry:
            job["shared"] = f_entry["shared"]
    return job


def worker_run(
    xlate_exe: str,
    spv_dir: str,
    chunk: list[tuple[str, str, bytes, bytes, str]],
    facts: dict[str, dict],
    progress_cb,
) -> tuple[int, list[tuple[str, str, str]]]:
    ok = 0
    fails: list[tuple[str, str, str]] = []
    batch_size = 200
    with tempfile.TemporaryDirectory() as td:
        for b0 in range(0, len(chunk), batch_size):
            batch = chunk[b0 : b0 + batch_size]
            jobs = []
            meta = []
            for idx, (key, stage, ctl, code, src) in enumerate(batch):
                cpath = os.path.join(td, f"s_{idx}.bin")
                with open(cpath, "wb") as cf_f:
                    cf_f.write(code)
                jobs.append(build_job(key, stage, ctl, cpath, len(code), facts))
                meta.append((key, stage, src))
            jpath = os.path.join(td, "jobs.json")
            with open(jpath, "w", encoding="utf-8") as jf:
                json.dump(jobs, jf, separators=(",", ":"))
            res = subprocess.run(
                [xlate_exe, jpath, spv_dir],
                capture_output=True,
                text=True,
                encoding="utf-8",
            )
            err_lines = {
                line.split(":", 1)[0].strip(): line
                for line in res.stderr.splitlines()
                if line.strip()
            }
            for key, stage, src in meta:
                spv_path = os.path.join(spv_dir, f"{key}.spv")
                if os.path.isfile(spv_path):
                    ok += 1
                else:
                    msg = err_lines.get(key, res.stderr.strip() or res.stdout.strip() or f"rc={res.returncode}")
                    fails.append((key, stage, f"{msg} ({src})"))
                progress_cb()
    return ok, fails


def main() -> int:
    ap = argparse.ArgumentParser(description="Translate extracted NVN2 shaders to SPIR-V")
    ap.add_argument("--pack", default="build/v327680/shaders-nvn2/shaders.bin", help="Path to shaders.bin")
    ap.add_argument("--facts", default="profiles/0400D4201C6EA000/shader_facts.tsv", help="Path to shader_facts.tsv")
    ap.add_argument("--xlate", default="build/xlate/totk_xlate.exe", help="Path to totk_xlate.exe")
    ap.add_argument("--out", default="build/v327680/shaders-nvn2/spv", help="Output SPIR-V directory")
    ap.add_argument("--workers", type=int, default=os.cpu_count() or 4, help="Parallel translator workers")
    ap.add_argument("--limit", type=int, default=0, help="Translate only first N shaders (for smoke testing)")
    ap.add_argument("--skip-existing", action="store_true", help="Skip keys that already have .spv")
    args = ap.parse_args()

    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    facts = load_facts(Path(args.facts))

    items = list(iter_pack(Path(args.pack)))
    if args.limit > 0:
        items = items[: args.limit]
    if args.skip_existing:
        items = [it for it in items if not (out_dir / f"{it[0]}.spv").is_file()]

    total = len(items)
    if total == 0:
        print("Nothing to translate (0 items).")
        return 0

    n_workers = max(1, min(args.workers, total))
    chunks = [items[i::n_workers] for i in range(n_workers)]

    t0 = time.time()
    done = 0
    lock = threading.Lock()

    def tick():
        nonlocal done
        with lock:
            done += 1
            if done % 2000 == 0 or done == total:
                dt = time.time() - t0
                rate = done / max(dt, 1e-6)
                print(f"Progress: {done}/{total} ({rate:.1f} shaders/s, {dt:.1f}s)", flush=True)

    total_ok = 0
    all_fails: list[tuple[str, str, str]] = []
    with cf.ThreadPoolExecutor(max_workers=n_workers) as pool:
        futs = [
            pool.submit(worker_run, args.xlate, str(out_dir), ch, facts, tick)
            for ch in chunks
            if ch
        ]
        for fut in cf.as_completed(futs):
            ok, fails = fut.result()
            total_ok += ok
            all_fails.extend(fails)

    dt = time.time() - t0
    print(f"Translated {total_ok}/{total} shaders in {dt:.1f}s ({len(all_fails)} failed)")
    if all_fails:
        err_log = out_dir.parent / "translate_errors.txt"
        with open(err_log, "w", encoding="utf-8") as ef:
            for k, st, msg in all_fails:
                ef.write(f"{k}\t{st}\t{msg}\n")
        for k, st, msg in all_fails[:10]:
            print(f"  FAIL [{st}] {k}: {msg}")
        print(f"Full error list written to {err_log}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
