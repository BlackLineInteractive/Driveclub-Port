# SPDX-License-Identifier: GPL-2.0-or-later
"""Garage tools: save backup, shader cache, shortcuts, Steam integration."""

import ctypes
import os
import subprocess
import tempfile
import uuid
import zipfile
import zlib
from ctypes import wintypes
from datetime import datetime
from pathlib import Path

from . import steam_vdf
from .paths import AppPaths

STEAM_APP_NAME = "DRIVECLUB (DCport)"
_NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)
FOLDERID_DESKTOP = "{B4BFCC3A-DB2C-424C-B029-7FE99A87C641}"


class SteamError(Exception):
    pass


class _GUID(ctypes.Structure):
    _fields_ = [("Data1", wintypes.DWORD), ("Data2", wintypes.WORD), ("Data3", wintypes.WORD),
                ("Data4", ctypes.c_ubyte * 8)]


def _known_folder(folder_id: str) -> str:
    """SHGetKnownFolderPath: where Windows really keeps a folder (it may be redirected)."""
    shell32, ole32 = ctypes.WinDLL("shell32"), ctypes.WinDLL("ole32")
    get = shell32.SHGetKnownFolderPath
    get.argtypes = [ctypes.POINTER(_GUID), wintypes.DWORD, wintypes.HANDLE,
                    ctypes.POINTER(ctypes.c_void_p)]
    get.restype = ctypes.c_long
    ole32.CoTaskMemFree.argtypes = [ctypes.c_void_p]
    ole32.CoTaskMemFree.restype = None
    guid = _GUID.from_buffer_copy(uuid.UUID(folder_id).bytes_le)
    buf = ctypes.c_void_p()
    try:
        hr = get(ctypes.byref(guid), 0, None, ctypes.byref(buf))
        if hr != 0 or not buf.value:
            raise OSError(f"SHGetKnownFolderPath failed (0x{hr & 0xFFFFFFFF:08X})")
        return ctypes.wstring_at(buf.value)
    finally:
        if buf.value:
            ole32.CoTaskMemFree(buf)


def desktop_dir() -> Path:
    """The player's Desktop, also when it is redirected (OneDrive often moves it)."""
    try:
        return Path(_known_folder(FOLDERID_DESKTOP))
    except Exception:  # no shell32 (not Windows) or the call failed
        profile = os.environ.get("USERPROFILE")
        return (Path(profile) if profile else Path.home()) / "Desktop"


def _plain_name(name: str) -> bool:
    """One harmless folder or file-name part (no separators, drive, dots-only)."""
    return (isinstance(name, str) and name.strip(" .") != "" and name == name.strip()
            and not any(c in name for c in '\\/:*?"<>|') and Path(name).name == name)


def backup_folder(paths: AppPaths, profile: str) -> Path:
    """backups/<profile>, using the folder's real spelling when it exists (any case)."""
    try:
        for d in paths.backups.iterdir():
            if d.is_dir() and d.name.casefold() == profile.casefold():
                return d
    except OSError:
        pass
    return paths.backups / profile


def write_backup_zip(base: Path, files: "list[Path]", folder: Path, stem: str) -> Path:
    """Zip files (names relative to base) to folder/<stem>.zip, or <stem>-2.zip, -3… when
    taken. The zip is written to a temp file and read back before it gets its name; an
    existing zip is never replaced."""
    folder.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=folder, prefix=".backup-", suffix=".tmp")
    os.close(fd)
    try:
        names = [f.relative_to(base).as_posix() for f in files]
        with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as z:
            for f, name in zip(files, names):
                z.write(f, name)
        with zipfile.ZipFile(tmp) as z:
            if z.testzip() is not None or sorted(z.namelist()) != sorted(names):
                raise OSError("The backup could not be read back")
        n = 1
        while True:
            out = folder / (f"{stem}.zip" if n == 1 else f"{stem}-{n}.zip")
            if not out.exists():
                try:
                    os.rename(tmp, out)  # on Windows this never replaces a file
                    return out
                except FileExistsError:
                    pass
            n += 1
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


def backup_saves(paths: AppPaths, now: datetime, profile: str = "Default",
                 label: str = "saves") -> Path:
    """Zip user/savedata to backups/<profile>/<label>-YYYY-MM-DD_HH-MM.zip (entries
    savedata/...)."""
    if not _plain_name(profile) or not _plain_name(label):
        raise ValueError(f"Not a usable backup name: {profile!r} / {label!r}")
    src = paths.user / "savedata"
    files = sorted(p for p in src.rglob("*") if p.is_file()) if src.is_dir() else []
    if not files:
        raise FileNotFoundError("No saves to back up")
    return write_backup_zip(paths.user, files, backup_folder(paths, profile),
                            f"{label}-{now:%Y-%m-%d_%H-%M}")


def clear_shader_cache(paths: AppPaths) -> int:
    removed = 0
    for sub in ("cache", "shader"):
        base = paths.user / sub
        if not base.is_dir():
            continue
        for f in [p for p in base.rglob("*") if p.is_file()]:
            try:
                f.unlink()
                removed += 1
            except OSError:
                pass
        for d in sorted((p for p in base.rglob("*") if p.is_dir()),
                        key=lambda p: len(p.parts), reverse=True):
            try:
                d.rmdir()
            except OSError:
                pass
    return removed


def launch_target(paths: AppPaths, direct: bool) -> tuple[Path, str]:
    return paths.root / "DriveClub.exe", ("--play" if direct else "")


_LNK_PS = (
    "$ErrorActionPreference = 'Stop'; "
    "$s = New-Object -ComObject WScript.Shell; "
    "$l = $s.CreateShortcut($env:DC_LNK); "
    "$l.TargetPath = $env:DC_TARGET; "
    "$l.Arguments = $env:DC_ARGS; "
    "$l.WorkingDirectory = $env:DC_WORKDIR; "
    "$l.IconLocation = $env:DC_ICON; "
    "$l.Save()"
)


def create_desktop_shortcut(paths: AppPaths, direct: bool, desktop: Path) -> Path:
    exe, args = launch_target(paths, direct)
    lnk = Path(desktop) / "DRIVECLUB.lnk"
    lnk.unlink(missing_ok=True)
    env = dict(os.environ,
               DC_LNK=str(lnk), DC_TARGET=str(exe), DC_ARGS=args,
               DC_WORKDIR=str(paths.root), DC_ICON=f"{exe},0")
    r = subprocess.run(
        ["powershell", "-NoProfile", "-NonInteractive", "-WindowStyle", "Hidden",
         "-Command", _LNK_PS],
        env=env, capture_output=True, text=True, creationflags=_NO_WINDOW)
    if r.returncode != 0 or not lnk.exists():
        raise RuntimeError(r.stderr.strip() or "Could not create the shortcut")
    return lnk


def steam_running() -> bool:
    try:
        r = subprocess.run(
            ["tasklist", "/FI", "IMAGENAME eq steam.exe", "/NH"],
            capture_output=True, text=True, timeout=10, creationflags=_NO_WINDOW)
        return "steam.exe" in r.stdout.lower()
    except Exception:
        return False


def _registry_steam_root() -> Path | None:
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam") as k:
            return Path(winreg.QueryValueEx(k, "SteamPath")[0])
    except Exception:
        return None


def steam_userdata_dirs(steam_root: Path | None) -> list[Path]:
    root = Path(steam_root) if steam_root is not None else _registry_steam_root()
    if root is None:
        return []
    ud = root / "userdata"
    if not ud.is_dir():
        return []
    return sorted(p for p in ud.iterdir()
                  if p.is_dir() and p.name.isdigit() and p.name != "0")


def _quoted(p: str) -> str:
    return f'"{p}"'


def _entry_for(paths: AppPaths, direct: bool) -> dict:
    exe, args = launch_target(paths, direct)
    exe_s = _quoted(str(exe))
    return {
        "appid": zlib.crc32((exe_s + STEAM_APP_NAME).encode()) | 0x80000000,
        "AppName": STEAM_APP_NAME,
        "Exe": exe_s,
        "StartDir": _quoted(str(paths.root) + "\\"),
        "icon": str(exe),
        "ShortcutPath": "",
        "LaunchOptions": args,
        "IsHidden": 0,
        "AllowDesktopConfig": 1,
        "AllowOverlay": 1,
        "OpenVR": 0,
        "Devkit": 0,
        "DevkitGameID": "",
        "LastPlayTime": 0,
        "tags": {},
    }


def _write_atomic(target: Path, data: bytes) -> None:
    fd, tmp = tempfile.mkstemp(dir=target.parent, prefix="shortcuts.", suffix=".tmp")
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
        os.replace(tmp, target)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


def add_to_steam(paths: AppPaths, direct: bool, userdata: list[Path]) -> int:
    if not userdata:
        if _registry_steam_root() is None:
            raise SteamError("Steam is not installed")
        raise SteamError("No Steam user found")
    new = _entry_for(paths, direct)
    updated = 0
    for user in userdata:
        cfg = Path(user) / "config"
        cfg.mkdir(parents=True, exist_ok=True)
        vdf = cfg / "shortcuts.vdf"
        doc: dict = {"shortcuts": {}}
        if vdf.exists():
            raw = vdf.read_bytes()
            try:
                doc = steam_vdf.load(raw) if raw else {"shortcuts": {}}
                if not isinstance(doc.get("shortcuts"), dict):
                    raise ValueError("no shortcuts map")
            except ValueError:
                os.replace(vdf, cfg / "shortcuts.vdf.bad")
                doc = {"shortcuts": {}}
            else:
                bak = cfg / "shortcuts.vdf.bak"
                if not bak.exists():
                    bak.write_bytes(raw)
        entries = doc["shortcuts"]
        key = next((k for k, e in entries.items()
                    if isinstance(e, dict) and e.get("AppName") == STEAM_APP_NAME), None)
        if key is None:
            nums = [int(k) for k in entries if str(k).isdigit()]
            key = str(max(nums) + 1 if nums else 0)
            entries[key] = new
        else:
            entries[key].update({k: v for k, v in new.items() if k != "tags"})
        _write_atomic(vdf, steam_vdf.dump(doc))
        updated += 1
    return updated
