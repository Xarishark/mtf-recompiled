"""Record the title's NRR-registered NRO hashes and verify a locally captured NRO."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[1]


def _inside_build_generated(path: Path) -> bool:
    """Game-derived code may only be written under build/generated/ (git ignores build/)."""
    try:
        Path(path).resolve().relative_to(Path(os.environ.get("MTF_GENERATED_ROOT") or PROJECT_ROOT / "build" / "generated").resolve())
        return True
    except ValueError:
        return False

DEFAULT_PROFILE = PROJECT_ROOT / "profiles" / "0400D4201C6EA000" / "profile.json"


def verify_capture(nrr_path: Path, output_dir: Path, candidate: Path | None,
                   profile_path: Path = DEFAULT_PROFILE) -> dict[str, object]:
    nrr_path = Path(nrr_path).resolve()
    output_dir = Path(output_dir).resolve()
    profile = json.loads(Path(profile_path).read_text(encoding="utf-8"))
    nrr = nrr_path.read_bytes()
    if nrr[:4] != b"NRR0":
        raise ValueError(f"invalid NRR header: {nrr_path}")
    version = nrr[5]
    hash_header = 0x4C0 if version == 1 else 0x340
    offset, count = struct.unpack_from("<II", nrr, hash_header)
    if offset + count * 32 > len(nrr):
        raise ValueError("NRR hash table exceeds the file")
    registered = [nrr[offset + i * 32:offset + (i + 1) * 32].hex() for i in range(count)]
    nrr_hash = hashlib.sha256(nrr).hexdigest()
    expected = profile["nrr"]
    if version != expected["version"] or nrr_hash != expected["sha256"]:
        raise ValueError("NRR file does not match the pinned MTF 1.1.1 profile")
    if registered != expected["registered_nro_sha256"]:
        raise ValueError("registered NRO hashes differ from the pinned profile")
    if not _inside_build_generated(output_dir):
        raise ValueError("captured game code must stay under build/generated/")

    record: dict[str, object] = {
        "title_id": profile["title_id"],
        "profile_version": profile["profile_version"],
        "nrr_sha256": nrr_hash,
        "registered_nro_sha256": registered,
        "capture_status": "awaiting_candidate",
    }
    if candidate is not None:
        candidate = Path(candidate).resolve()
        raw = candidate.read_bytes()
        if len(raw) < 0x40 or raw[0x10:0x14] != b"NRO0":
            raise ValueError(f"candidate does not have an NRO0 header at +0x10: {candidate}")
        declared_size = struct.unpack_from("<I", raw, 0x18)[0]
        if declared_size != len(raw):
            raise ValueError(f"NRO header declares {declared_size:#x} bytes; candidate has {len(raw):#x}")
        candidate_hash = hashlib.sha256(raw).hexdigest()
        if candidate_hash not in registered:
            raise ValueError(f"captured NRO SHA-256 {candidate_hash} is not registered by this NRR")
        output_dir.mkdir(parents=True, exist_ok=True)
        destination = output_dir / f"{candidate_hash}.nro"
        destination.write_bytes(raw)
        record.update({
            "capture_status": "verified",
            "candidate_sha256": candidate_hash,
            "candidate_size": len(raw),
            "candidate_build_id": raw[0x40:0x60].hex(),
            "nro_path": str(destination.resolve()),
        })
    output_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / "capture-manifest.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    return record


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--nrr", type=Path, required=True)
    parser.add_argument("--out", type=Path, default=Path("build/generated/nro/0400D4201C6EA000/1.1.1"))
    parser.add_argument("--candidate", type=Path, help="exact NRO bytes captured from the runtime load")
    parser.add_argument("--profile", type=Path, default=DEFAULT_PROFILE)
    args = parser.parse_args()
    out = args.out if args.out.is_absolute() else PROJECT_ROOT / args.out
    try:
        record = verify_capture(args.nrr, out, args.candidate, args.profile)
    except (OSError, ValueError, KeyError, struct.error) as exc:
        parser.exit(1, f"NRO capture verification failed: {exc}\n")
    print(f"NRR version {record['nrr_sha256']} registers {len(record['registered_nro_sha256'])} NRO hash(es)")
    if record["capture_status"] == "verified":
        print(f"Verified NRO {record['candidate_sha256']} ({record['candidate_size']} bytes)")
    else:
        print("No candidate supplied; capture is still pending")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
