# SPDX-License-Identifier: GPL-2.0-or-later
"""Minimal reader/writer for Steam's binary VDF (shortcuts.vdf)."""

import struct
from dataclasses import dataclass

_MAP, _STR, _INT, _END = 0x00, 0x01, 0x02, 0x08
_RAW_SIZES = {0x03: 4, 0x07: 8, 0x0A: 8}  # float32, uint64, int64: kept byte-exact


@dataclass(frozen=True)
class Raw:
    type: int
    data: bytes


def _cstr(data: bytes, pos: int) -> tuple[str, int]:
    end = data.find(b"\0", pos)
    if end < 0:
        raise ValueError("unterminated string")
    return data[pos:end].decode("utf-8", errors="replace"), end + 1


def _load_map(data: bytes, pos: int, top: bool) -> tuple[dict, int]:
    out: dict = {}
    while True:
        if pos >= len(data):
            if top:
                return out, pos
            raise ValueError("unexpected end of data")
        t = data[pos]
        pos += 1
        if t == _END:
            return out, pos
        key, pos = _cstr(data, pos)
        if t == _MAP:
            out[key], pos = _load_map(data, pos, False)
        elif t == _STR:
            out[key], pos = _cstr(data, pos)
        elif t == _INT:
            if pos + 4 > len(data):
                raise ValueError("truncated int")
            out[key] = struct.unpack_from("<I", data, pos)[0]
            pos += 4
        elif t in _RAW_SIZES:
            n = _RAW_SIZES[t]
            if pos + n > len(data):
                raise ValueError("truncated value")
            out[key] = Raw(t, bytes(data[pos:pos + n]))
            pos += n
        else:
            raise ValueError(f"unknown type 0x{t:02x}")


def load(data: bytes) -> dict:
    if not data:
        raise ValueError("empty data")
    try:
        obj, pos = _load_map(bytes(data), 0, True)
    except (struct.error, IndexError) as e:
        raise ValueError(str(e)) from e
    if not obj:
        raise ValueError("no content")
    return obj


def _dump_map(obj: dict) -> bytes:
    out = bytearray()
    for key, val in obj.items():
        k = str(key).encode("utf-8") + b"\0"
        if isinstance(val, dict):
            out += bytes([_MAP]) + k + _dump_map(val)
        elif isinstance(val, Raw):
            if _RAW_SIZES.get(val.type) != len(val.data):
                raise ValueError("bad raw value")
            out += bytes([val.type]) + k + val.data
        elif isinstance(val, str):
            out += bytes([_STR]) + k + val.encode("utf-8") + b"\0"
        elif isinstance(val, int):
            out += bytes([_INT]) + k + struct.pack("<I", val & 0xFFFFFFFF)
        else:
            raise ValueError(f"unsupported value type {type(val).__name__}")
    out.append(_END)
    return bytes(out)


def dump(obj: dict) -> bytes:
    return _dump_map(obj)
