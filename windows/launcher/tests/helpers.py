# SPDX-License-Identifier: GPL-2.0-or-later
"""Shared test builders (extended by later tasks)."""

import struct
from pathlib import Path


def make_sfo(values: dict) -> bytes:
    keys = b""
    data = b""
    entries = b""
    for key, val in values.items():
        koff = len(keys)
        keys += key.encode() + b"\0"
        if isinstance(val, int):
            fmt, raw, used, cap = 0x0404, struct.pack("<I", val), 4, 4
        else:
            raw = val.encode("utf-8") + b"\0"
            fmt, used = 0x0204, len(raw)
            cap = used + 4
            raw = raw.ljust(cap, b"\0")
        entries += struct.pack("<HHIII", koff, fmt, used, cap, len(data))
        data += raw
    key_off = 20 + len(entries)
    data_off = key_off + len(keys)
    header = struct.pack("<4sIIII", b"\0PSF", 0x101, key_off, data_off, len(values))
    return header + entries + keys + data


def fake_game(dir: Path, title_id: str = "CUSA00003", app_ver: str = "01.28",
              title: str = "DRIVECLUB") -> Path:
    dir = Path(dir)
    (dir / "sce_sys").mkdir(parents=True, exist_ok=True)
    (dir / "eboot.bin").write_bytes(b"\x7fELF")
    (dir / "sce_sys" / "param.sfo").write_bytes(
        make_sfo({"TITLE_ID": title_id, "APP_VER": app_ver, "TITLE": title}))
    return dir


def make_port(tmp_path, with_data: bool = True):
    """A game dir plus a port folder with runtime, patches xml and (optionally) prepared data."""
    from dcport.paths import AppPaths
    tmp_path = Path(tmp_path)
    game = fake_game(tmp_path / "game")
    paths = AppPaths.for_root(tmp_path / "port")
    paths.runtime.parent.mkdir(parents=True, exist_ok=True)
    paths.runtime.write_bytes(b"MZ runtime")
    paths.patches_xml.parent.mkdir(parents=True, exist_ok=True)
    paths.patches_xml.write_text(
        '<Patch><Metadata Name="60 FPS with deltatime" AppVer="01.28"><PatchList>'
        '<Line Type="bytes" Address="0x400010" Value="90"/></PatchList></Metadata></Patch>',
        encoding="utf-8")
    paths.data.mkdir(parents=True, exist_ok=True)
    if with_data:
        (paths.data / "boot-linked.bin").write_bytes(b"boot")
        (paths.data / "content.bin").write_bytes(b"content")
    return paths, game


def make_data(paths) -> None:
    """Create the prepared data files the runtime expects."""
    paths.data.mkdir(parents=True, exist_ok=True)
    (paths.data / "boot-linked.bin").write_bytes(b"boot")
    (paths.data / "content.bin").write_bytes(b"content")
    (paths.data / "patches.bin").write_bytes(b"patches")
