"""Write a metadata-only report for selected NSO modules."""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from collections import Counter
from pathlib import Path

import numpy as np

PROJECT_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))

from layout import layout  # noqa: E402
from nso import Nso  # noqa: E402


def scan_module(path: Path, name: str | None = None, base: int = 0, *, nso: Nso | None = None,
                peer_exports: dict[str, int] | None = None) -> dict[str, object]:
    """Return module layout, relocation and symbol metadata without writing game data."""
    path = Path(path)
    name = name or path.name
    nso = nso or Nso(str(path))
    peer_exports = peer_exports or {}
    sha256 = hashlib.sha256(path.read_bytes()).hexdigest()
    symbol_types = Counter((symbol[1] & 0xF) for symbol in nso.syms if symbol[0])
    relocations = Counter(relocation[1] for relocation in nso.relocs)
    words = np.frombuffer(nso.image, dtype="<u4", count=nso.text[1] // 4, offset=nso.text[0])
    svc_words = words[(words & 0xFFE0001F) == 0xD4000001]
    svc_ids = Counter(int(word >> 5 & 0xFFFF) for word in svc_words)
    branch_masks = {
        "br": (0xFFFFFC1F, 0xD61F0000),
        "blr": (0xFFFFFC1F, 0xD63F0000),
        "ret": (0xFFFFFC1F, 0xD65F0000),
    }
    branch_sites = {name: int(np.count_nonzero((words & mask) == value))
                    for name, (mask, value) in branch_masks.items()}
    branch_sites["retaa"] = int(np.count_nonzero(words == 0xD65F0BFF))
    branch_sites["retab"] = int(np.count_nonzero(words == 0xD65F0FFF))
    auth_words = {
        "pacibsp": 0xD503237F, "autibsp": 0xD50323FF, "autia1716": 0xD503219F,
        "autib1716": 0xD50321DF, "autiasp": 0xD50323BF, "xpaclri": 0xD50320FF,
    }
    auth_counts = {mnemonic: int(np.count_nonzero(words == word))
                   for mnemonic, word in auth_words.items() if np.any(words == word)}
    auth_counts["pacia"] = int(np.count_nonzero((words & 0xFFFFFC00) == 0xDAC10000))
    auth_counts["xpaci"] = int(np.count_nonzero((words & 0xFFFFFC00) == 0xDAC14000))
    imports = []
    for symbol in nso.imports():
        symbol_name, info = symbol[0], symbol[1]
        resolved_address = peer_exports.get(symbol_name)
        imports.append({
            "name": symbol_name,
            "binding": info >> 4,
            "type": info & 0xF,
            "resolved_module_address": f"{resolved_address:#x}" if resolved_address is not None else None,
        })
    interesting_names = (
        "nnMain", "nninitStartup", "_ZN2nn4init5StartEmmPFvvES2_S2_",
        "_ZN2nn2ro10LoadModuleEPNS0_6ModuleEPKvPvmi",
        "_ZN2nn2ro18RegisterModuleInfoEPNS0_16RegistrationInfoEPKv",
    )
    symbols_of_interest = {
        symbol_name: f"{base + symbol[3]:#x}"
        for symbol_name in interesting_names
        for symbol in nso.exports() if symbol[0] == symbol_name
    }
    return {
        "name": name,
        "source": str(path.resolve()),
        "sha256": sha256,
        "build_id": nso.build_id,
        "guest_base": f"{base:#x}",
        "image_size": f"{nso.size:#x}",
        "text": {"offset": f"{nso.text[0]:#x}", "size": f"{nso.text[1]:#x}",
                 "guest_start": f"{base + nso.text[0]:#x}",
                 "guest_end": f"{base + nso.text[0] + nso.text[1]:#x}",
                 "instruction_words": nso.text[1] // 4},
        "rodata": {"offset": f"{nso.rodata[0]:#x}", "size": f"{nso.rodata[1]:#x}"},
        "data": {"offset": f"{nso.data[0]:#x}", "size": f"{nso.data[1]:#x}"},
        "bss_size": f"{nso.bss_size:#x}",
        "needed": nso.needed,
        "symbols": {"total": len(nso.syms), "imports": len(nso.imports()),
                    "exports": len(nso.exports()),
                    "type_counts": {f"{kind:#x}": count for kind, count in sorted(symbol_types.items())}},
        "dynamic_init": {
            "init": f"{base + nso.dyn[12]:#x}" if nso.dyn.get(12) else None,
            "fini": f"{base + nso.dyn[13]:#x}" if nso.dyn.get(13) else None,
            "init_array": f"{base + nso.dyn[25]:#x}" if nso.dyn.get(25) else None,
            "init_array_entries": nso.dyn.get(27, 0) // 8,
        },
        "relocations": {f"{kind:#x}": count for kind, count in sorted(relocations.items())},
        "imports": imports,
        "resolved_import_count": sum(item["resolved_module_address"] is not None for item in imports),
        "unresolved_imports": sorted({item["name"] for item in imports
                                       if item["resolved_module_address"] is None}),
        "symbols_of_interest": symbols_of_interest,
        "instruction_candidates": {
            "svc_count": int(len(svc_words)),
            "svc_ids": {f"{imm:#x}": count for imm, count in sorted(svc_ids.items())},
            "indirect_branches": branch_sites,
            "simd_words": int(np.count_nonzero(((words >> 25) & 0x7) == 0x7)),
            "pointer_authentication": auth_counts,
        },
    }


def scan_modules(exefs: Path, names: list[str]) -> dict[str, object]:
    exefs = Path(exefs).resolve()
    if not exefs.is_dir():
        raise FileNotFoundError(f"ExeFS directory not found: {exefs}")
    modules = {name: (base, Nso(str(exefs / name))) for name, base, _ in layout(exefs)}
    missing = sorted(set(names) - modules.keys())
    if missing:
        raise ValueError(f"requested modules are missing from {exefs}: {', '.join(missing)}")
    exports = {
        name: {symbol[0]: base + symbol[3] for symbol in nso.exports() if symbol[0]}
        for name, (base, nso) in modules.items() if name in names
    }
    records = []
    for name in names:
        base, nso = modules[name]
        peer_exports = {}
        for peer_name in names:
            if peer_name != name:
                peer_exports.update(exports[peer_name])
        records.append(scan_module(exefs / name, name, base, nso=nso, peer_exports=peer_exports))
    nrr_record = None
    nrr_path = exefs.parent / "romfs" / ".nrr" / "nrr.nrr"
    if nrr_path.is_file():
        nrr = nrr_path.read_bytes()
        if nrr[:4] != b"NRR0":
            raise ValueError(f"invalid NRR header: {nrr_path}")
        hash_offset = 0x4C0 if nrr[5] == 1 else 0x340
        offset, count = struct.unpack_from("<II", nrr, hash_offset)
        if offset + count * 32 > len(nrr):
            raise ValueError(f"NRR hash table is out of bounds: {nrr_path}")
        nrr_record = {
            "version": nrr[5],
            "sha256": hashlib.sha256(nrr).hexdigest(),
            "registered_nro_sha256": [nrr[offset + 32 * i:offset + 32 * (i + 1)].hex()
                                      for i in range(count)],
        }
    return {
        "title_id": "0400D4201C6EA000",
        "profile_version": "1.1.1",
        "source_revision": 327680,
        "exefs": str(exefs),
        "modules": records,
        "nrr": nrr_record,
    }


def _inside_build_generated(path: Path) -> bool:
    build_generated = (PROJECT_ROOT / "build" / "generated").resolve()
    return path.resolve() == build_generated or build_generated in path.resolve().parents


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exefs", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--modules", default="main,sdk")
    args = parser.parse_args()
    output = args.out if args.out.is_absolute() else PROJECT_ROOT / args.out
    if not _inside_build_generated(output):
        parser.error("--out must stay under build/generated/")
    names = [name.strip() for name in args.modules.split(",") if name.strip()]
    report = scan_modules(args.exefs, names)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"Wrote {len(names)} module records to {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
