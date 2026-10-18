# SPDX-License-Identifier: GPL-2.0-or-later
"""Game folder information and pre-flight checks."""
import struct
from dataclasses import dataclass
from pathlib import Path

from dcport.paths import AppPaths

_HEADER = struct.Struct("<4sIIII")
_ENTRY = struct.Struct("<HHIII")


def read_sfo(data: bytes) -> dict:
    if len(data) < _HEADER.size:
        raise ValueError("param.sfo too short")
    magic, _ver, key_off, data_off, count = _HEADER.unpack_from(data, 0)
    if magic != b"\0PSF":
        raise ValueError("not a PSF file")
    out = {}
    for i in range(count):
        pos = _HEADER.size + i * _ENTRY.size
        if pos + _ENTRY.size > len(data):
            raise ValueError("truncated param.sfo")
        koff, fmt, used, _cap, doff = _ENTRY.unpack_from(data, pos)
        kstart = key_off + koff
        kend = data.find(b"\0", kstart)
        if kend < 0:
            raise ValueError("bad key table")
        key = data[kstart:kend].decode("utf-8", "replace")
        raw = data[data_off + doff:data_off + doff + used]
        if fmt == 0x0404:
            if len(raw) < 4:
                raise ValueError("truncated integer")
            out[key] = struct.unpack("<I", raw[:4])[0]
        else:
            out[key] = raw.decode("utf-8", "replace").rstrip("\0")
    return out


@dataclass(frozen=True)
class GameInfo:
    title_id: str
    app_ver: str
    title: str


def game_info(game_dir: Path) -> "GameInfo | None":
    try:
        d = read_sfo((Path(game_dir) / "sce_sys" / "param.sfo").read_bytes())
    except (OSError, ValueError):
        return None
    return GameInfo(str(d.get("TITLE_ID", "")), str(d.get("APP_VER", "")),
                    str(d.get("TITLE", "")))


def has_eboot(game_dir: Path) -> bool:
    return (Path(game_dir) / "eboot.bin").is_file()


UPDATE_ONLY = ("This folder is the 01.28 update without the base game (no game.chc). Copy the "
               "update's files over your base game (01.00) folder, replacing the old ones, and "
               "choose that folder.")


def is_update_only(game_dir: Path) -> bool:
    """An unpacked update package: the 01.28 eboot.bin but none of the base game's files
    (game.chc, the content index, ships with the base game only). The game crashes at boot."""
    return has_eboot(game_dir) and not (Path(game_dir) / "game.chc").is_file()


def art_path(game_dir: Path) -> "Path | None":
    # DRIVECLUB's pic0 is the race background; its pic1 is the Evolution Studios logo.
    for name in ("pic0.png", "pic1.png"):
        p = Path(game_dir) / "sce_sys" / name
        if p.is_file():
            return p
    return None


@dataclass(frozen=True)
class Check:
    label: str
    state: str  # "ok" | "warn" | "bad"


def _has_saves(paths: AppPaths) -> bool:
    try:
        return any(p.is_dir() for p in (paths.user / "savedata").iterdir())
    except OSError:
        return False


def ready_checks(game_dir: Path, paths: AppPaths, gpu: "str | None",
                 prepared: bool) -> "list[Check]":
    info = game_info(game_dir)
    if not has_eboot(game_dir):
        game = Check("Choose the DRIVECLUB folder", "bad")
    elif is_update_only(game_dir):
        game = Check("Update only: put it over the base game", "bad")
    elif info and info.app_ver == "01.28":  # any region; prepare/dc_patches.py checks the same
        game = Check(f"DRIVECLUB {info.title_id} v01.28", "ok")
    else:
        tid = info.title_id if info else "unknown"
        ver = info.app_ver if info else "unknown"
        game = Check(f"Game {tid} v{ver}: patches need v01.28", "warn")
    saves = Check("Saves found", "ok") if _has_saves(paths) else Check("No saves yet", "warn")
    graphics = Check(gpu, "ok") if gpu else Check("Graphics card: checking…", "warn")
    files = (Check("Game files prepared", "ok") if prepared
             else Check("Game files will be prepared on PLAY", "warn"))
    return [game, saves, graphics, files]
