"""Read-only validator for the user's Mario Tennis Fever + Update 1.1.1 dump.

Verifies the extracted NUP folder (or config.local.toml) against the expected
profile hashes and Switch 2 NSO0/NRR1/CRA headers without modifying any game file.
"""

import argparse
import hashlib
import json
import struct
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_PROFILE = ROOT / "profiles" / "0400D4201C6EA000" / "profile.json"


def md5_file(path: Path) -> str:
    h = hashlib.md5()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def parse_nrr_v1(path: Path) -> tuple[int, int, list[str]]:
    data = path.read_bytes()
    if data[:4] != b"NRR0":
        raise ValueError(f"{path}: expected NRR0 magic, got {data[:4]!r}")
    version = data[5]
    if version != 1:
        raise ValueError(f"{path}: expected NRR version 1 (Switch 2), got {version}")
    program_id = struct.unpack_from("<Q", data, 0x4B0)[0]
    hashes_offset = struct.unpack_from("<I", data, 0x4C0)[0]
    hashes_count = struct.unpack_from("<I", data, 0x4C4)[0]
    hashes = []
    for i in range(hashes_count):
        off = hashes_offset + i * 32
        hashes.append(data[off : off + 32].hex())
    return program_id, hashes_count, hashes


def load_dump_path(cli_path: str | None) -> Path:
    if cli_path:
        return Path(cli_path)
    local_cfg = ROOT / "config.local.toml"
    if not local_cfg.is_file():
        raise FileNotFoundError(
            "No --dump path provided and config.local.toml not found. "
            "Copy config.example.toml to config.local.toml and set [game].dump_path."
        )
    cfg = tomllib.loads(local_cfg.read_text(encoding="utf-8"))
    dump_path = cfg.get("game", {}).get("dump_path", "")
    if not dump_path:
        raise ValueError("config.local.toml [game].dump_path is empty.")
    return Path(dump_path)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--dump", help="Path to extracted Mario Tennis Fever + Update 1.1.1 folder")
    parser.add_argument("--profile", default=str(DEFAULT_PROFILE), help="Path to profile.json")
    args = parser.parse_args()

    profile = json.loads(Path(args.profile).read_text(encoding="utf-8"))
    dump_dir = load_dump_path(args.dump)
    if not dump_dir.is_dir():
        print(f"ERROR: Dump directory not found: {dump_dir}", file=sys.stderr)
        return 1

    upd_ver = str(profile["target_update"]["version"])
    upd_dir = dump_dir / "updates" / upd_ver
    manifest_path = dump_dir / "nup-manifest.json"
    if not manifest_path.is_file():
        print(f"ERROR: Missing nup-manifest.json in {dump_dir}", file=sys.stderr)
        return 1

    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    pkgs = {p["path"]: p for p in manifest.get("packages", [])}
    print(f"Manifest format : {manifest.get('format')}")
    print(f"Title ID        : {profile['title_id']} ({profile['name']})")
    print(f"Update folder   : updates/{upd_ver} (display {profile['target_update']['display_version']})")

    if f"updates/{upd_ver}" not in pkgs:
        print(f"ERROR: updates/{upd_ver} not listed in nup-manifest.json", file=sys.stderr)
        return 1

    # Validate main and sdk NSO0 modules
    ok = True
    for mod_name, info in profile["target_update"]["modules"].items():
        mod_path = upd_dir / "exefs" / mod_name
        if not mod_path.is_file():
            print(f"ERROR: Missing {mod_path}", file=sys.stderr)
            ok = False
            continue
        magic = mod_path.open("rb").read(4)
        actual_md5 = md5_file(mod_path)
        expected_md5 = info["md5"]
        match = actual_md5 == expected_md5 and magic == b"NSO0"
        status = "OK" if match else "MISMATCH"
        print(f"Module {mod_name:6s}  : {status} (magic={magic!r}, md5={actual_md5}, size={mod_path.stat().st_size:,} B)")
        if not match:
            print(f"  expected md5={expected_md5}", file=sys.stderr)
            ok = False

    # Validate main.npdm
    npdm_path = upd_dir / "exefs" / "main.npdm"
    if npdm_path.is_file():
        magic = npdm_path.open("rb").read(4)
        print(f"NPDM            : {'OK' if magic == b'META' else 'BAD'} (magic={magic!r})")
    else:
        print(f"ERROR: Missing {npdm_path}", file=sys.stderr)
        ok = False

    # Validate NRR v1
    nrr_path = upd_dir / "romfs" / profile["target_update"]["romfs"]["nrr"]
    if nrr_path.is_file():
        prog_id, count, hashes = parse_nrr_v1(nrr_path)
        print(f"NRR v1          : OK (program_id={prog_id:016X}, registered_nros={count})")
        for idx, h in enumerate(hashes):
            print(f"  nro[{idx}] sha256: {h}")
    else:
        print(f"ERROR: Missing {nrr_path}", file=sys.stderr)
        ok = False

    # Validate CRA archive header
    cra_path = upd_dir / "romfs" / profile["target_update"]["romfs"]["archive"]
    if cra_path.is_file():
        magic = cra_path.open("rb").read(4)
        expected_magic = profile["target_update"]["romfs"]["archive_magic"].encode("ascii")
        print(f"RomFS archive   : {'OK' if magic == expected_magic else 'BAD'} (magic={magic!r}, size={cra_path.stat().st_size:,} B)")
    else:
        print(f"ERROR: Missing {cra_path}", file=sys.stderr)
        ok = False

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
