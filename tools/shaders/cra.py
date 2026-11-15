#!/usr/bin/env python3
"""Clean-room reader and decryptor for Camelot's rom:/42 archive (MTF Recomp, MIT).

Reads the encrypted file table and extension string table from the game's
main NRO (Update 1.1.1 / v327680) and decrypts entries on demand from
romfs/42 (strictly read-only).
"""

from __future__ import annotations

import argparse
import ctypes
import fnmatch
import os
import struct
from pathlib import Path
from typing import Iterator

_FAST_DLL = None
_dll_path = Path(__file__).resolve().parent.parent.parent / "build" / "cra_fast.dll"
if _dll_path.is_file():
    try:
        _FAST_DLL = ctypes.CDLL(str(_dll_path))
        _FAST_DLL.cra_decrypt_blocks.argtypes = [
            ctypes.c_void_p,
            ctypes.c_size_t,
            ctypes.c_size_t,
            ctypes.c_uint32,
        ]
        _FAST_DLL.cra_decrypt_blocks.restype = ctypes.c_uint32
    except OSError:
        _FAST_DLL = None

# Virtual address bias of nro0 in v327680 static recompilation.
NRO_VA_BIAS = 0x7001E00000
FILE_TABLE_VA = 0x70032080F2
ORACLE_TABLE_VA = 0x7003232C10
EXT_TABLE_VA = 0x7003556D18
ENTRY_COUNT = 0x21E0  # 8,672 entries (index 0 is root/empty)

# Key derivation constants from f_7002f96620 / f_7002f9d400.
_X11_C1 = 0x6F646E65746E694E  # "Nintend" (little-endian u64)
_X11_C2 = 0x544F4C454D41432F  # "/CAMELOT" (little-endian u64)
_X10_C1 = 0x316B537255657355  # "UesUrSk1"
_X10_C2 = 0x6430473034736C6C  # "lls40G0d"
_X10_XOR = 0x02052636 ^ 0xA756E6F0

_CASE_MUL_HI = (
    0x11111111,
    0x22222223,
    0x33333333,
    0x44444445,
    0x55555555,
    0x66666667,
    0x77777777,
    0x88888889,
)


def _make_crc_table(poly: int) -> list[int]:
    tab = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = ((c >> 1) ^ poly) if (c & 1) else (c >> 1)
        tab.append(c)
    return tab


_CRC32_TAB = _make_crc_table(0xEDB88320)
_CRC32C_TAB = _make_crc_table(0x82F63B78)


def _crc_u64(tab: list[int], init: int, val: int) -> int:
    c = init & 0xFFFFFFFF
    for shift in range(0, 64, 8):
        c = tab[(c ^ ((val >> shift) & 0xFF)) & 0xFF] ^ (c >> 8)
    return c


def _stem_key_bytes(name: str) -> bytes:
    base = name.rsplit("/", 1)[-1]
    out = []
    for ch in base:
        if ch in ("_", ".") or len(out) == 4:
            break
        out.append(ch)
    return "".join(out).encode("ascii")


def derive_file_key(name: str) -> int:
    """Derive the 32-bit per-file key (x26) from the archive entry path."""
    n_val = int.from_bytes(_stem_key_bytes(name), "big")
    x11 = _crc_u64(_CRC32_TAB, _crc_u64(_CRC32_TAB, 0xFFFFFFFF, _X11_C1), _X11_C2)
    x10 = _crc_u64(_CRC32_TAB, _crc_u64(_CRC32_TAB, n_val, _X10_C1), _X10_C2) ^ _X10_XOR
    return (x11 ^ x10) & 0xFFFFFFFF


class CraArchive:
    """Read-only reader for Camelot's rom:/42 archive."""

    def __init__(self, nro_path: str | Path, rom42_path: str | Path) -> None:
        self.nro_path = Path(nro_path)
        self.rom42_path = Path(rom42_path)
        self._nro = self.nro_path.read_bytes()
        self._base = FILE_TABLE_VA - NRO_VA_BIAS
        self._oracle_base = ORACLE_TABLE_VA - NRO_VA_BIAS
        self._ext_base = EXT_TABLE_VA - NRO_VA_BIAS
        self._names: dict[int, str] = {}
        self._by_name: dict[str, int] | None = None

    def _raw_entry(self, idx: int) -> tuple[int, int, int, int, int, int]:
        delta = struct.unpack_from("<h", self._nro, self._base + idx * 2)[0]
        e = self._base + 0x43C0 + idx * 18 + delta
        pdelta, plen, ext_id = struct.unpack_from("<HBb", self._nro, e)
        size = struct.unpack_from("<I", self._nro, e + 4)[0] ^ 0xFF00FF00
        off = (struct.unpack_from("<Q", self._nro, e + 8)[0] & 0xFFFFFFFFFF) ^ 0x00FF00FF00FF
        return e, pdelta, plen, ext_id, size, off

    def decode_name(self, idx: int) -> str:
        cached = self._names.get(idx)
        if cached is not None:
            return cached
        e, pd, pl, ext_id, _, _ = self._raw_entry(idx)
        own = []
        p = e + 0xD
        while self._nro[p] != 0:
            own.append(chr((idx - self._nro[p]) & 0x7F))
            p += 1
        parts = []
        cur = idx - pd
        limit = pl
        while limit > 0:
            ec, cpd, cpl, _, _, _ = self._raw_entry(cur)
            take = limit - cpl
            parts.append("".join(chr((cur - self._nro[ec + 0xD + i]) & 0x7F) for i in range(take)))
            cur -= cpd
            limit = cpl
        ext = ""
        if ext_id >= 0:
            eoff = struct.unpack_from("<Q", self._nro, self._ext_base + ext_id * 8)[0]
            ep = eoff
            ech = []
            while self._nro[ep] != 0:
                ech.append(chr((ext_id - self._nro[ep]) & 0x7F))
                ep += 1
            ext = "".join(ech)
        full = "".join(reversed(parts)) + "".join(own) + ext
        segs = full.split("/")
        out = []
        for seg in segs:
            if "$" in seg and out:
                seg = seg.replace("$", out[-1])
            out.append(seg)
        res = "/".join(out)
        self._names[idx] = res
        return res

    def iter_entries(self) -> Iterator[tuple[int, str, int, int]]:
        for idx in range(1, ENTRY_COUNT):
            _, _, _, _, size, off = self._raw_entry(idx)
            yield idx, self.decode_name(idx), size, off

    def find_index(self, name: str) -> int:
        if self._by_name is None:
            self._by_name = {self.decode_name(i): i for i in range(1, ENTRY_COUNT)}
        return self._by_name[name]

    def read_entry(self, idx_or_name: int | str, verify_oracle: bool = True) -> bytes:
        idx = self.find_index(idx_or_name) if isinstance(idx_or_name, str) else idx_or_name
        _, _, _, _, size, off = self._raw_entry(idx)
        if size == 0:
            return b""
        name = self.decode_name(idx)
        x26 = derive_file_key(name)
        aligned_size = (size + 15) & ~15
        with open(self.rom42_path, "rb") as f:
            f.seek(off)
            buf = bytearray(f.read(aligned_size))

        if _FAST_DLL is not None:
            raw_arr = (ctypes.c_uint8 * len(buf)).from_buffer(buf)
            chk = _FAST_DLL.cra_decrypt_blocks(raw_arr, len(buf), size, x26)
        else:
            case = x26 & 7
            tab = _CRC32_TAB if case < 4 else _CRC32C_TAB
            x22 = 1 | (((x26 & 0xFFFFFFFE) ^ _CASE_MUL_HI[case]) << 32)
            nblk = (size // 16) * 16
            x23 = 0
            swap_in = (case & 2) == 0
            odd_out = (case & 1) != 0
            for o in range(0, len(buf), 16):
                a, b = struct.unpack_from("<QQ", buf, o)
                if swap_in:
                    a, b = b, a
                x0 = (_crc_u64(tab, x26, b) * x22) & 0xFFFFFFFFFFFFFFFF
                b2 = x0 ^ ((~((b << 32) & 0xFFFFFFFFFFFFFFFF)) & 0xFFFFFFFFFFFFFFFF)
                a2 = a ^ (x0 >> 3)
                if odd_out:
                    struct.pack_into("<QQ", buf, o, a2, b2)
                else:
                    struct.pack_into("<QQ", buf, o, b2, a2)
                if o < nblk:
                    x23 = (x23 + a2) & 0xFFFFFFFFFFFFFFFF
            chk = ((x23 >> 32) ^ x23) & 0xFFFFFFFF

        if verify_oracle and (size % 16 == 0):
            oracle = struct.unpack_from("<I", self._nro, self._oracle_base + idx * 4)[0]
            if chk != oracle:
                raise ValueError(
                    f"Checksum mismatch for entry {idx} ({name}): got {chk:#010x}, expected {oracle:#010x}"
                )
        return bytes(buf[:size])


def main() -> int:
    ap = argparse.ArgumentParser(description="Read/extract Camelot rom:/42 archive")
    ap.add_argument("--nro", required=True, help="Path to nro0.nro")
    ap.add_argument("--rom42", required=True, help="Path to romfs/42")
    ap.add_argument("--list", action="store_true", help="List entries")
    ap.add_argument("--pattern", default="*", help="Glob filter for --list / --extract")
    ap.add_argument("--extract", metavar="OUT_DIR", help="Extract matching entries")
    ap.add_argument("--verify", action="store_true", help="Verify checksums for matching entries")
    args = ap.parse_args()

    arc = CraArchive(args.nro, args.rom42)
    matched = [
        (idx, name, size, off)
        for idx, name, size, off in arc.iter_entries()
        if fnmatch.fnmatch(name, args.pattern)
    ]

    if args.list:
        for idx, name, size, off in matched:
            print(f"{idx:4d} {size:10d} {off:#012x} {name}")
        print(f"Total matched: {len(matched)}")

    if args.extract:
        out_root = Path(args.extract)
        for idx, name, _, _ in matched:
            dst = out_root / name
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes(arc.read_entry(idx))
        print(f"Extracted {len(matched)} files to {out_root}")

    if args.verify:
        checked = 0
        for idx, name, size, _ in matched:
            if size % 16 == 0:
                arc.read_entry(idx, verify_oracle=True)
                checked += 1
        print(f"Verified {checked} mod-16 entries with 0 errors")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
