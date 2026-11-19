"""Data types shared by the local NUP audit and extraction commands."""

from __future__ import annotations

from dataclasses import asdict, dataclass
from typing import Any


TARGET_TITLE_ID = "0400D4201C6EA000"
PROFILE_VERSIONS = {(TARGET_TITLE_ID, 327680): "1.1.1"}


@dataclass(frozen=True, slots=True)
class NupFileRecord:
    path: str
    size: int
    sha256: str
    codec: str


@dataclass(frozen=True, slots=True)
class NupPackageRecord:
    index: int
    kind: str
    title_id: str
    version: int
    version_hex: str
    flags: int
    display_version: str | None
    profile_version: str | None
    source_dump_label: str | None
    build_id: str | None
    files: tuple[NupFileRecord, ...]


@dataclass(frozen=True, slots=True)
class NupAudit:
    input_path: str
    input_size: int
    input_mtime_ns: int
    title_id: str
    display_version: str | None
    profile_version: str | None
    source_dump_label: str | None
    packages: tuple[NupPackageRecord, ...]
    base_present: bool
    update_versions: tuple[int, ...]
    module_records: tuple[dict[str, Any], ...]
    verification_status: str
    payload_hash_status: str

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)
