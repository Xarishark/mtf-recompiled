"""Generate C from selected game NSOs and summarize the emitted coverage."""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[1]
GENERATOR = Path(__file__).resolve().parent / "gen.py"


def _inside_build_generated(path: Path) -> bool:
    # MTF_GENERATED_ROOT: the installer's temp folder, outside any checkout
    build_generated = Path(os.environ.get("MTF_GENERATED_ROOT") or PROJECT_ROOT / "build" / "generated").resolve()
    resolved = path.resolve()
    return resolved == build_generated or build_generated in resolved.parents


def lift_module(exefs: Path, output_dir: Path, modules: list[str], limit: int, jobs: int) -> dict[str, object]:
    output_dir = output_dir.resolve()
    if not _inside_build_generated(output_dir):
        raise ValueError("generated game code must stay under build/generated/")
    if not GENERATOR.is_file():
        raise FileNotFoundError(f"generator not found: {GENERATOR}")
    output_dir.mkdir(parents=True, exist_ok=True)
    command = [sys.executable, str(GENERATOR), "--exefs", str(Path(exefs).resolve()),
               "--out", str(output_dir), "--modules", ",".join(modules), "--jobs", str(jobs)]
    if limit:
        command.extend(["--limit", str(limit)])
    subprocess.run(command, cwd=PROJECT_ROOT, check=True)

    module_reports = []
    for name in modules:
        manifest_path = output_dir / name / "manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        stats = manifest["stats"]
        module_reports.append({
            "module": name,
            "function_count": manifest["function_count"],
            "emitted_functions": stats["emitted_functions"],
            "instructions": stats["instructions"],
            "traps": stats["traps"],
            "unsupported": stats["unsupported"],
            "unsupported_census": manifest["unsupported_census"],
            "generated_c_bytes": stats["c_bytes"],
            "chunks": manifest["chunks"],
        })
    report = {
        "title_id": "0400D4201C6EA000",
        "profile_version": "1.1.1",
        "source_revision": 327680,
        "exefs": str(Path(exefs).resolve()),
        "output_dir": str(output_dir),
        "modules": module_reports,
    }
    (output_dir / "lift-report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exefs", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--modules", default="main")
    parser.add_argument("--limit", type=int, default=0, help="diagnostic subset; 0 emits every discovered function")
    parser.add_argument("--jobs", type=int, default=1)
    args = parser.parse_args()
    modules = [name.strip() for name in args.modules.split(",") if name.strip()]
    if not modules or args.limit < 0 or args.jobs < 1:
        parser.error("provide at least one module, a non-negative limit, and a positive worker count")
    output = args.out if args.out.is_absolute() else PROJECT_ROOT / args.out
    try:
        report = lift_module(args.exefs, output, modules, args.limit, args.jobs)
    except (OSError, ValueError, subprocess.CalledProcessError) as exc:
        parser.exit(1, f"lift failed: {exc}\n")
    for item in report["modules"]:
        print(f"{item['module']}: {item['emitted_functions']}/{item['function_count']} functions, "
              f"{item['instructions']} words, {item['unsupported']} unsupported")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
