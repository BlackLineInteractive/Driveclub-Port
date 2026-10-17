# SPDX-License-Identifier: GPL-2.0-or-later
"""Save profiles (separate DRIVECLUB careers) and the dated backups of each.

The game always reads user/savedata, so that folder holds the active profile's saves; the
others wait in profiles/<name>/savedata. Switching and restoring only rename folders on the
same volume (nothing is copied and then deleted). Before anything is replaced or removed a
zip of it is written to backups/<profile>/ and read back, and a step that fails undoes the
steps before it.
"""

import errno
import os
import shutil
import stat
import sys
import tempfile
import time
import uuid
import zipfile
import zlib
from datetime import datetime
from pathlib import Path

from . import garage
from .paths import AppPaths

DEFAULT = "Default"
NAME_MAX = 24
_RESERVED = {"CON", "PRN", "AUX", "NUL", *(f"COM{i}" for i in range(1, 10)),
             *(f"LPT{i}" for i in range(1, 10))}
_BAD_CHARS = set('<>:"/\\|?*') | {chr(i) for i in range(32)}
_RESTORE_PREFIX = ".dcport-restore-"   # user/<this>…: a restore in progress
_TRASH_PREFIX = ".dcport-deleting-"    # profiles/<this>…: a profile being removed
_SPARE_BYTES = 64 * 1024 * 1024        # free space a restore leaves on the disk


# ---------------------------------------------------------------- names and folders

def name_problem(name) -> "str | None":
    """Why name can't be a profile (and a folder) name, or None when it can: 1–24 letters,
    digits, spaces, - or _, no spaces at the ends, no Windows device name."""
    if not isinstance(name, str) or not name.strip(" "):
        return "Type a name"
    if len(name) > NAME_MAX:
        return f"Use at most {NAME_MAX} characters"
    if name != name.strip(" "):
        return "No spaces at the start or the end"
    if not all(c.isalpha() or c.isdecimal() or c in " -_" for c in name):
        return "Use only letters, digits, spaces, - and _"
    if name.upper() in _RESERVED:
        return f"{name} is a name Windows keeps for devices"
    return None


def valid_name(name) -> bool:
    return name_problem(name) is None


def same(a: str, b: str) -> bool:
    """Profile names compare like Windows folder names: without case."""
    return a.casefold() == b.casefold()


def profiles_dir(paths: AppPaths) -> Path:
    return paths.root / "profiles"


def _check_name(name) -> None:
    if not valid_name(name):
        raise ValueError(f"{name!r} can't be a profile name: use 1 to {NAME_MAX} letters, "
                         "digits, spaces, - or _")


def _find(parent: Path, name: str) -> Path:
    """parent/name, spelled like the folder on disk when one exists in any case."""
    try:
        for d in parent.iterdir():
            if same(d.name, name) and d.is_dir():
                return d
    except OSError:
        pass
    return parent / name


def _live(paths: AppPaths) -> Path:
    return paths.user / "savedata"


def _files(folder: Path) -> "list[Path]":
    return sorted(p for p in folder.rglob("*") if p.is_file()) if folder.is_dir() else []


def _has_files(folder: Path) -> bool:
    return folder.is_dir() and any(p.is_file() for p in folder.rglob("*"))


def _why(error: BaseException) -> str:
    return getattr(error, "strerror", None) or str(error) or type(error).__name__


def _remove_empty_tree(folder: Path) -> None:
    """Remove a folder that holds no files (only empty folders); rmdir never deletes data."""
    if not folder.is_dir():
        return
    for d in sorted((p for p in folder.rglob("*") if p.is_dir()),
                    key=lambda p: len(p.parts), reverse=True):
        d.rmdir()
    folder.rmdir()


def _remove_tree(path: Path) -> None:
    """Best-effort removal of DCport's own temporary folders (read-only files too)."""
    def again(func, p, exc):
        try:
            os.chmod(p, stat.S_IWRITE)
            func(p)
        except OSError:
            pass

    handler = {"onexc": again} if sys.version_info >= (3, 12) else {"onerror": again}
    try:
        shutil.rmtree(path, **handler)
    except OSError:
        pass


def _leftovers(folder: Path, prefix: str) -> "list[Path]":
    try:
        return [d for d in folder.iterdir() if d.is_dir() and d.name.startswith(prefix)]
    except OSError:
        return []


def _recover(paths: AppPaths, active: str) -> "str | None":
    """Saves that an interrupted switch or restore left outside user/savedata go back.
    Returns where they came from (None when nothing was moved)."""
    live = _live(paths)
    if live.exists():
        return None
    parked = _find(profiles_dir(paths), active) / "savedata"
    candidates = [parked] + [d / "old" for d in _leftovers(paths.user, _RESTORE_PREFIX)]
    for folder in candidates:
        if _has_files(folder):
            paths.user.mkdir(parents=True, exist_ok=True)
            os.rename(folder, live)
            return str(folder)
    return None


def _preserve_leftovers(paths: AppPaths, profile: str, now: datetime) -> "list[str]":
    """Restore leftovers (user/.dcport-restore-*): one that still holds any file is moved
    to backups/<profile>/recovered-<time>/ (never deleted; a failed move leaves it in
    place); only empty ones are removed."""
    messages = []
    for leftover in _leftovers(paths.user, _RESTORE_PREFIX):
        try:
            if not _has_files(leftover):
                _remove_empty_tree(leftover)
                continue
            parent = garage.backup_folder(paths, profile)
            parent.mkdir(parents=True, exist_ok=True)
            target = _free(parent, f"recovered-{now:%Y-%m-%d_%H-%M-%S}")
            os.rename(leftover, target)
            messages.append(f"Kept saves from an interrupted restore in {target}")
        except OSError:
            pass  # left in place; the files are still on disk
    return messages


def recover(paths: AppPaths, active: str) -> "list[str]":
    """Startup: put back saves an interrupted switch or restore moved out of user/savedata
    and keep any other leftover that holds files. Returns what it did, for the player."""
    messages = []
    try:
        source = _recover(paths, active)
        if source:
            messages.append(f"Moved your saves back to user\savedata from {source}")
    except OSError as e:
        messages.append(f"Could not move your saves back ({_why(e)}); they are still in "
                        f"profiles\{active} or user\.dcport-restore-*")
    messages += _preserve_leftovers(paths, active, datetime.now())
    return messages


# ---------------------------------------------------------------- profiles

def list_profiles(paths: AppPaths, active: str) -> "list[str]":
    """The active profile, then every profiles/<name> folder (sorted, without case)."""
    names, seen = [active], {active.casefold()}
    try:
        found = sorted((d.name for d in profiles_dir(paths).iterdir()
                        if d.is_dir() and valid_name(d.name)), key=str.casefold)
    except OSError:
        found = []
    for name in found:
        if name.casefold() not in seen:
            names.append(name)
            seen.add(name.casefold())
    return names


def create_profile(paths: AppPaths, name: str, active: str) -> None:
    """A new, empty profile (a new career) in profiles/<name>/savedata."""
    _check_name(name)
    if any(same(name, n) for n in list_profiles(paths, active)):
        raise ValueError(f"There is already a profile called {name}")
    folder = profiles_dir(paths) / name
    folder.mkdir(parents=True)
    (folder / "savedata").mkdir()


def switch_profile(paths: AppPaths, current: str, new: str) -> None:
    """Park user/savedata in profiles/<current>/savedata and move <new>'s saves in.

    Only while the game is closed. If the second move fails the first is undone."""
    _check_name(current)
    _check_name(new)
    if same(current, new):
        return
    _recover(paths, current)
    base = profiles_dir(paths)
    parked = _find(base, current) / "savedata"
    incoming = _find(base, new) / "savedata"
    live = _live(paths)
    if _has_files(parked):
        raise RuntimeError(f"profiles\\{parked.parent.name}\\savedata already holds saves, so "
                           f"the saves in use can't be parked there. Nothing was changed.")
    if live.exists() and not live.is_dir():
        raise RuntimeError("user\\savedata is not a folder. Nothing was changed.")
    _remove_empty_tree(parked)
    parked.parent.mkdir(parents=True, exist_ok=True)
    paths.user.mkdir(parents=True, exist_ok=True)
    moved = live.is_dir()
    if moved:
        os.rename(live, parked)
    else:
        parked.mkdir()  # an empty slot keeps the profile in the list
    try:
        if incoming.is_dir():
            os.rename(incoming, live)
        else:
            live.mkdir()  # a profile without saves yet starts a new career
    except BaseException as e:
        try:
            if moved:
                os.rename(parked, live)
            else:
                parked.rmdir()
        except OSError:
            raise RuntimeError(f"Switching failed ({_why(e)}) and the saves could not be moved "
                               f"back. They are safe in {parked}") from e
        raise
    try:
        incoming.parent.rmdir()  # the new active profile's folder, when it is empty now
    except OSError:
        pass


def _free(folder: Path, name: str) -> Path:
    target = folder / name
    stem, suffix = (target.stem, target.suffix) if target.suffix.lower() == ".zip" else (name, "")
    n = 2
    while target.exists():
        target = folder / f"{stem}-{n}{suffix}"
        n += 1
    return target


def _loose_zips(paths: AppPaths) -> "list[Path]":
    """Zips straight in backups/ (before profiles existed); they belong to Default."""
    try:
        return [p for p in paths.backups.iterdir()
                if p.is_file() and p.suffix.lower() == ".zip"]
    except OSError:
        return []


def _undo(moves: "list[tuple[Path | None, Path]]") -> None:
    for src, dst in reversed(moves):
        try:
            if src is None:
                dst.rmdir()  # a folder this move created (empty again by now)
            else:
                src.parent.mkdir(parents=True, exist_ok=True)
                os.rename(dst, src)
        except OSError:
            pass  # renames only: whatever stays put is still on disk


def _move_backups(paths: AppPaths, old: str, new: str) -> "list[tuple[Path | None, Path]]":
    """Give old's backups to new (merging with backups a deleted <new> left behind);
    returns the moves so a caller can undo them."""
    src = garage.backup_folder(paths, old)
    moves: "list[tuple[Path | None, Path]]" = []
    if same(old, new):  # only the spelling changes
        if src.is_dir() and src.name != new:
            os.rename(src, paths.backups / new)
            moves.append((src, paths.backups / new))
        return moves
    loose = _loose_zips(paths) if same(old, DEFAULT) else []
    items = (sorted(src.iterdir()) if src.is_dir() else []) + loose
    if not items:
        return moves
    dst = garage.backup_folder(paths, new)
    try:
        if not dst.exists() and not loose:
            os.rename(src, dst)
            moves.append((src, dst))
            return moves
        if not dst.exists():
            dst.mkdir(parents=True)
            moves.append((None, dst))
        for item in items:
            target = _free(dst, item.name)
            os.rename(item, target)
            moves.append((item, target))
    except BaseException:
        _undo(moves)
        raise
    try:
        src.rmdir()
    except OSError:
        pass
    return moves


def rename_profile(paths: AppPaths, old: str, new: str, active: str) -> None:
    """Rename a profile and its backups folder. For the active profile (its saves are in
    user/savedata) only the backups move; the caller stores the new name."""
    _check_name(new)
    names = list_profiles(paths, active)
    old = next((n for n in names if same(n, old)), None) if isinstance(old, str) else None
    if old is None:
        raise ValueError("There is no such profile")
    if any(same(n, new) for n in names if not same(n, old)):
        raise ValueError(f"There is already a profile called {new}")
    if old == new:
        return
    base = profiles_dir(paths)
    folder = _find(base, old)
    if same(old, active):
        moves = _move_backups(paths, old, new)
        if folder.is_dir():  # an empty folder left by an earlier switch
            try:
                os.rename(folder, base / new)
            except BaseException:
                _undo(moves)
                raise
        return
    os.rename(folder, base / new)
    try:
        _move_backups(paths, old, new)
    except BaseException:
        os.rename(base / new, folder)
        raise


def delete_profile(paths: AppPaths, name: str, active: str, now: datetime) -> "Path | None":
    """Zip an inactive profile to backups/<name>/deleted-….zip, then remove its folder.
    Returns the zip (None when the profile had no files)."""
    _check_name(name)
    if same(name, active):
        raise ValueError("The profile in use can't be deleted; switch to another one first")
    base = profiles_dir(paths)
    folder = _find(base, name)
    if not folder.is_dir():
        raise ValueError(f"There is no profile called {name}")
    for leftover in _leftovers(base, _TRASH_PREFIX):
        _remove_tree(leftover)
    files = _files(folder)
    saved = None
    if files:
        saved = garage.write_backup_zip(folder, files, garage.backup_folder(paths, folder.name),
                                        f"deleted-{now:%Y-%m-%d_%H-%M}")
    trash = base / f"{_TRASH_PREFIX}{uuid.uuid4().hex[:12]}"
    os.rename(folder, trash)  # gone from the list at once; the zip has its files
    _remove_tree(trash)
    return saved


# ---------------------------------------------------------------- backups

def list_backups(paths: AppPaths, profile: str) -> "list[Path]":
    """The profile's backup zips, newest first (by the time they were written)."""
    folder = garage.backup_folder(paths, profile)
    try:
        zips = [p for p in folder.iterdir() if p.is_file() and p.suffix.lower() == ".zip"] \
            if folder.is_dir() else []
    except OSError:
        zips = []
    if same(profile, DEFAULT):
        zips += _loose_zips(paths)

    def written(p: Path) -> float:
        try:
            return p.stat().st_mtime
        except OSError:
            return 0.0

    return sorted(zips, key=lambda p: (written(p), p.name), reverse=True)


def delete_backup(path: Path) -> None:
    path = Path(path)
    if path.suffix.lower() != ".zip" or not path.is_file():
        raise ValueError(f"{path.name} is not a backup zip")
    path.unlink()


def _entry_parts(info: zipfile.ZipInfo) -> "list[str]":
    """The path parts of a zip entry, or ValueError when it could land anywhere but in
    savedata/ (absolute, drive, .., device names, links, odd characters)."""
    name = info.filename
    unsafe = ValueError(f"The backup holds an unsafe entry: {name}")
    if not name or name[0] in "/\\" or "\\" in name or ":" in name:
        raise unsafe
    if stat.S_ISLNK(info.external_attr >> 16):
        raise unsafe
    parts = name.split("/")
    if info.is_dir():
        parts = parts[:-1]
    for part in parts:
        if part in ("", ".", "..") or part[-1] in " ." or any(c in _BAD_CHARS for c in part):
            raise unsafe
        if part.split(".")[0].rstrip(" ").upper() in _RESERVED:
            raise unsafe
    if not same(parts[0], "savedata"):
        raise ValueError(f"The backup holds a file outside savedata: {name}")
    if not info.is_dir() and len(parts) < 2:
        raise unsafe
    return parts


def _check_backup(z: zipfile.ZipFile) -> "tuple[list, int]":
    """Every entry checked (and every byte read once) before anything on disk changes."""
    members, seen, size = [], set(), 0
    for info in z.infolist():
        parts = _entry_parts(info)
        if info.flag_bits & 0x1:
            raise ValueError("The backup is password-protected")
        if not info.is_dir():
            key = "/".join(parts[1:]).casefold()
            if key in seen:
                raise ValueError(f"The backup holds {info.filename} twice")
            seen.add(key)
            size += info.file_size
        members.append((info, parts))
    if not seen:
        raise ValueError("The backup holds no saves")
    try:
        bad = z.testzip()
    except (zipfile.BadZipFile, zlib.error, EOFError, NotImplementedError, RuntimeError) as e:
        raise ValueError(f"The backup is damaged ({_why(e)})") from e
    if bad is not None:
        raise ValueError(f"The backup is damaged ({bad})")
    return members, size


def _extract(z: zipfile.ZipFile, members: list, dest: Path) -> None:
    """Write the checked entries under dest (the parts after savedata/, nothing else)."""
    dest.mkdir()
    for info, parts in members:
        target = dest.joinpath(*parts[1:])
        if info.is_dir():
            target.mkdir(parents=True, exist_ok=True)
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        with z.open(info) as src, open(target, "xb") as out:
            shutil.copyfileobj(src, out)
        try:
            stamp = time.mktime(info.date_time + (0, 0, -1))
            os.utime(target, (stamp, stamp))
        except (OSError, OverflowError, ValueError):
            pass


def restore_backup(paths: AppPaths, profile: str, zip_path: Path,
                   now: datetime) -> "Path | None":
    """Replace user/savedata with a backup's saves.

    The zip is checked completely first (ValueError, nothing changed). The current saves
    are then zipped to backups/<profile>/before-restore-….zip (returned; None when there
    were none), the backup is unpacked into a temporary folder in user/ and the folders
    are swapped by renaming."""
    _check_name(profile)
    zip_path = Path(zip_path)
    try:
        z = zipfile.ZipFile(zip_path)
    except zipfile.BadZipFile as e:
        raise ValueError(f"{zip_path.name} is not a readable zip") from e
    with z:
        members, size = _check_backup(z)
        where = paths.user if paths.user.is_dir() else paths.root
        if shutil.disk_usage(where).free < size + _SPARE_BYTES:
            raise OSError(errno.ENOSPC, "Not enough free disk space to restore this backup")
        _recover(paths, profile)
        _preserve_leftovers(paths, profile, now)
        live = _live(paths)
        safety = None
        if _has_files(live):
            safety = garage.backup_saves(paths, now, profile, label="before-restore")
        paths.user.mkdir(parents=True, exist_ok=True)
        work = Path(tempfile.mkdtemp(dir=paths.user, prefix=_RESTORE_PREFIX))
        try:
            _extract(z, members, work / "savedata")
        except BaseException:
            _remove_tree(work)
            raise
    old = work / "old"
    had_live = live.is_dir()
    try:
        if had_live:
            os.rename(live, old)
    except BaseException:
        _remove_tree(work)
        raise
    try:
        os.rename(work / "savedata", live)
    except BaseException as e:
        if had_live:
            try:
                os.rename(old, live)
            except OSError:
                raise RuntimeError(f"Restoring failed ({_why(e)}) and the saves could not be "
                                   f"moved back. They are safe in {old}") from e
        _remove_tree(work)
        raise
    _remove_tree(work)  # the replaced saves; the before-restore zip keeps them
    return safety
