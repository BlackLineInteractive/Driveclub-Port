# SPDX-License-Identifier: GPL-2.0-or-later
"""Save profiles and backups: every test runs in a tmp_path port folder."""

import os
import zipfile
from datetime import datetime
from pathlib import Path

import pytest

from dcport import profiles
from dcport.garage import backup_saves
from dcport.paths import AppPaths
from dcport.profiles import (DEFAULT, create_profile, delete_backup, delete_profile,
                             list_backups, list_profiles, recover, rename_profile, restore_backup,
                             switch_profile, valid_name)
from dcport.settings import load_settings, save_settings, LauncherSettings

NOW = datetime(2026, 10, 17, 9, 5)


def port(tmp_path) -> AppPaths:
    return AppPaths.for_root(tmp_path / "port")


def put(folder: Path, files: dict[str, bytes]) -> None:
    for rel, data in files.items():
        f = folder / rel
        f.parent.mkdir(parents=True, exist_ok=True)
        f.write_bytes(data)


def tree(folder: Path) -> dict[str, bytes]:
    if not folder.is_dir():
        return {}
    return {p.relative_to(folder).as_posix(): p.read_bytes()
            for p in folder.rglob("*") if p.is_file()}


def live(paths) -> Path:
    return paths.user / "savedata"


def slot(paths, name) -> Path:
    return paths.root / "profiles" / name / "savedata"


SAVE_A = {"1/CUSA00003/SAVEDATA00/sdimg": b"career A", "1/CUSA00003/SAVEDATA00/param.sfo": b"A"}
SAVE_B = {"1/CUSA00003/SAVEDATA00/sdimg": b"career B"}


def make_zip(path: Path, entries: dict[str, bytes]) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, "w") as z:
        for name, data in entries.items():
            z.writestr(name, data)
    return path


def no_leftovers(paths) -> bool:
    user = paths.user
    return not any(p.name.startswith(".") for p in user.iterdir()) if user.is_dir() else True


# ---------------------------------------------------------------- names

@pytest.mark.parametrize("name", ["Default", "Player 2", "a", "x" * 24, "Sam_1-B", "José",
                                  "Nasser", "2"])
def test_valid_names(name):
    assert valid_name(name)


@pytest.mark.parametrize("name", ["", " ", "    ", "x" * 25, "a/b", "a\\b", "..", ".", "a.b",
                                  "CON", "con", "Nul", "COM1", "lpt9", "aux", "PRN",
                                  " lead", "trail ", "tab\there", "a:b", "e\u0301", "new\nline",
                                  "a*b", "a?b", 'a"b', "a<b", "a|b", None, 5])
def test_invalid_names(name):
    assert not valid_name(name)


# ---------------------------------------------------------------- listing and creating

def test_list_profiles_active_first_then_sorted_case_insensitive_unique(tmp_path):
    paths = port(tmp_path)
    assert list_profiles(paths, DEFAULT) == ["Default"]
    for name in ("zed", "Alice", "default", "bob"):
        (paths.root / "profiles" / name).mkdir(parents=True)
    (paths.root / "profiles" / ".dcport-deleting-1234").mkdir()   # never a profile
    (paths.root / "profiles" / "notes.txt").write_text("x", encoding="utf-8")
    assert list_profiles(paths, "Default") == ["Default", "Alice", "bob", "zed"]
    assert list_profiles(paths, "Bob") == ["Bob", "Alice", "default", "zed"]


def test_create_profile_is_empty_and_names_are_checked(tmp_path):
    paths = port(tmp_path)
    create_profile(paths, "Player 2", DEFAULT)
    assert slot(paths, "Player 2").is_dir() and tree(slot(paths, "Player 2")) == {}
    assert list_profiles(paths, DEFAULT) == ["Default", "Player 2"]
    for bad in ("player 2", "DEFAULT", "CON", "", "a/b"):
        with pytest.raises(ValueError):
            create_profile(paths, bad, DEFAULT)
    assert list_profiles(paths, DEFAULT) == ["Default", "Player 2"]


# ---------------------------------------------------------------- switching

def test_switch_round_trip_keeps_each_profiles_files(tmp_path):
    paths = port(tmp_path)
    put(live(paths), SAVE_A)
    put(paths.user / "cache" / "CUSA00003", {"a.spv": b"shader"})
    create_profile(paths, "Sam", DEFAULT)
    switch_profile(paths, DEFAULT, "Sam")
    assert tree(live(paths)) == {} and live(paths).is_dir()
    assert tree(slot(paths, "Default")) == SAVE_A
    put(live(paths), SAVE_B)                                  # Sam plays
    switch_profile(paths, "Sam", DEFAULT)
    assert tree(live(paths)) == SAVE_A and tree(slot(paths, "Sam")) == SAVE_B
    switch_profile(paths, DEFAULT, "Sam")
    assert tree(live(paths)) == SAVE_B and tree(slot(paths, "Default")) == SAVE_A
    assert (paths.user / "cache" / "CUSA00003" / "a.spv").read_bytes() == b"shader"
    assert list_profiles(paths, "Sam") == ["Sam", "Default"]


def test_switch_to_a_missing_profile_starts_empty_and_keeps_the_old_one_listed(tmp_path):
    paths = port(tmp_path)                                    # no saves at all yet
    switch_profile(paths, DEFAULT, "Kid")
    assert live(paths).is_dir() and tree(live(paths)) == {}
    assert list_profiles(paths, "Kid") == ["Kid", "Default"]


def test_switch_rolls_back_when_the_second_rename_fails(tmp_path, monkeypatch):
    paths = port(tmp_path)
    put(live(paths), SAVE_A)
    create_profile(paths, "Sam", DEFAULT)
    put(slot(paths, "Sam"), SAVE_B)
    real = os.rename
    calls = []

    def flaky(src, dst):
        calls.append((Path(src), Path(dst)))
        if len(calls) == 2:
            raise PermissionError(13, "Access is denied")
        return real(src, dst)

    monkeypatch.setattr(profiles.os, "rename", flaky)
    with pytest.raises(OSError):
        switch_profile(paths, DEFAULT, "Sam")
    monkeypatch.setattr(profiles.os, "rename", real)
    assert len(calls) >= 3                                    # moved, failed, moved back
    assert tree(live(paths)) == SAVE_A and tree(slot(paths, "Sam")) == SAVE_B
    assert tree(slot(paths, "Default")) == {}


def test_switch_refuses_when_the_parking_slot_holds_files(tmp_path):
    paths = port(tmp_path)
    put(live(paths), SAVE_A)
    put(slot(paths, "Default"), SAVE_B)                      # e.g. settings were reset
    with pytest.raises(RuntimeError):
        switch_profile(paths, DEFAULT, "Sam")
    assert tree(live(paths)) == SAVE_A and tree(slot(paths, "Default")) == SAVE_B
    assert not (paths.root / "profiles" / "Sam").exists()


def test_switch_rejects_bad_names_and_same_profile_is_a_no_op(tmp_path):
    paths = port(tmp_path)
    put(live(paths), SAVE_A)
    for cur, new in (("..", "Sam"), (DEFAULT, "../x"), (DEFAULT, "")):
        with pytest.raises(ValueError):
            switch_profile(paths, cur, new)
    switch_profile(paths, DEFAULT, "default")
    assert tree(live(paths)) == SAVE_A and not (paths.root / "profiles").exists()


def test_switch_puts_back_saves_left_parked_by_an_interrupted_switch(tmp_path):
    paths = port(tmp_path)
    put(slot(paths, DEFAULT), SAVE_A)                         # crash after the first rename
    create_profile(paths, "Sam", DEFAULT)
    put(slot(paths, "Sam"), SAVE_B)
    switch_profile(paths, DEFAULT, "Sam")
    assert tree(live(paths)) == SAVE_B and tree(slot(paths, DEFAULT)) == SAVE_A


# ---------------------------------------------------------------- renaming

def test_rename_inactive_profile_moves_folder_and_backups(tmp_path):
    paths = port(tmp_path)
    create_profile(paths, "Sam", DEFAULT)
    put(slot(paths, "Sam"), SAVE_B)
    make_zip(paths.backups / "Sam" / "saves-2026-10-16_10-00.zip", {"savedata/x": b"1"})
    rename_profile(paths, "sam", "Samuel", DEFAULT)
    assert tree(slot(paths, "Samuel")) == SAVE_B and not (paths.root / "profiles" / "Sam").exists()
    assert [p.name for p in list_backups(paths, "Samuel")] == ["saves-2026-10-16_10-00.zip"]
    assert list_profiles(paths, DEFAULT) == ["Default", "Samuel"]


def test_rename_active_profile_only_moves_its_backups(tmp_path):
    paths = port(tmp_path)
    put(live(paths), SAVE_A)
    z = backup_saves(paths, NOW)
    old_layout = make_zip(paths.backups / "saves-2026-01-01_00-00.zip", {"savedata/x": b"1"})
    rename_profile(paths, DEFAULT, "Nasser", DEFAULT)
    assert tree(live(paths)) == SAVE_A and not (paths.root / "profiles").exists()
    names = sorted(p.name for p in list_backups(paths, "Nasser"))
    assert names == [old_layout.name, z.name]
    assert list_backups(paths, DEFAULT) == []


def test_rename_refuses_duplicates_and_bad_names_and_allows_a_case_change(tmp_path):
    paths = port(tmp_path)
    create_profile(paths, "Sam", DEFAULT)
    for new in ("default", "DEFAULT", "x" * 25, "NUL"):
        with pytest.raises(ValueError):
            rename_profile(paths, "Sam", new, DEFAULT)
    with pytest.raises(ValueError):
        rename_profile(paths, "Nobody", "Body", DEFAULT)
    rename_profile(paths, "Sam", "SAM", DEFAULT)
    assert list_profiles(paths, DEFAULT) == ["Default", "SAM"]


def test_rename_into_leftover_backups_of_a_deleted_profile_merges_them(tmp_path):
    paths = port(tmp_path)
    create_profile(paths, "Sam", DEFAULT)
    make_zip(paths.backups / "Sam" / "saves-2026-10-16_10-00.zip", {"savedata/x": b"sam"})
    make_zip(paths.backups / "Kim" / "saves-2026-10-16_10-00.zip", {"savedata/x": b"kim"})
    rename_profile(paths, "Sam", "Kim", DEFAULT)
    got = {p.name: zipfile.ZipFile(p).read("savedata/x") for p in list_backups(paths, "Kim")}
    assert got == {"saves-2026-10-16_10-00.zip": b"kim", "saves-2026-10-16_10-00-2.zip": b"sam"}


# ---------------------------------------------------------------- deleting profiles

def test_delete_refuses_the_active_profile(tmp_path):
    paths = port(tmp_path)
    put(live(paths), SAVE_A)
    with pytest.raises(ValueError):
        delete_profile(paths, "default", DEFAULT, NOW)
    assert tree(live(paths)) == SAVE_A


def test_delete_zips_the_profile_first_then_removes_it(tmp_path):
    paths = port(tmp_path)
    create_profile(paths, "Sam", DEFAULT)
    put(slot(paths, "Sam"), SAVE_B)
    z = delete_profile(paths, "Sam", DEFAULT, NOW)
    assert z == paths.backups / "Sam" / "deleted-2026-10-17_09-05.zip"
    with zipfile.ZipFile(z) as f:
        assert {n: f.read(n) for n in f.namelist()} == {f"savedata/{k}": v for k, v in SAVE_B.items()}
    assert not (paths.root / "profiles" / "Sam").exists()
    assert list_profiles(paths, DEFAULT) == ["Default"]
    create_profile(paths, "Empty", DEFAULT)
    assert delete_profile(paths, "Empty", DEFAULT, NOW) is None
    assert list_profiles(paths, DEFAULT) == ["Default"]
    with pytest.raises(ValueError):
        delete_profile(paths, "Nobody", DEFAULT, NOW)


def test_deleted_profile_can_be_restored_from_its_zip(tmp_path):
    paths = port(tmp_path)
    create_profile(paths, "Sam", DEFAULT)
    put(slot(paths, "Sam"), SAVE_B)
    z = delete_profile(paths, "Sam", DEFAULT, NOW)
    switch_profile(paths, DEFAULT, "Sam")                     # a new, empty "Sam"
    assert restore_backup(paths, "Sam", z, NOW) is None       # nothing to keep first
    assert tree(live(paths)) == SAVE_B


# ---------------------------------------------------------------- backups

def test_backups_go_to_the_profile_folder_with_unique_names(tmp_path):
    paths = port(tmp_path)
    put(live(paths), SAVE_A)
    a = backup_saves(paths, NOW)
    b = backup_saves(paths, NOW)
    c = backup_saves(paths, NOW, profile="Sam", label="before-restore")
    assert a == paths.backups / "Default" / "saves-2026-10-17_09-05.zip"
    assert b == paths.backups / "Default" / "saves-2026-10-17_09-05-2.zip"
    assert c == paths.backups / "Sam" / "before-restore-2026-10-17_09-05.zip"
    with zipfile.ZipFile(a) as z:
        assert sorted(z.namelist()) == sorted(f"savedata/{k}" for k in SAVE_A)
    assert not list(paths.backups.rglob("*.tmp"))
    with pytest.raises(ValueError):
        backup_saves(paths, NOW, profile="../evil")


def test_list_backups_newest_first_and_old_layout_counts_as_default(tmp_path):
    paths = port(tmp_path)
    old = make_zip(paths.backups / "saves-2026-01-01_00-00.zip", {"savedata/x": b"1"})
    mid = make_zip(paths.backups / "Default" / "saves-2026-05-01_00-00.zip", {"savedata/x": b"2"})
    new = make_zip(paths.backups / "Default" / "before-restore-2026-09-01_00-00.zip",
                   {"savedata/x": b"3"})
    (paths.backups / "Default" / "saves-x.tmp").write_bytes(b"half")
    for i, p in enumerate((old, mid, new)):
        os.utime(p, (1_700_000_000 + i * 1000,) * 2)
    assert list_backups(paths, DEFAULT) == [new, mid, old]
    assert list_backups(paths, "default") == [new, mid, old]
    assert list_backups(paths, "Sam") == []


def test_delete_backup_removes_only_that_zip(tmp_path):
    paths = port(tmp_path)
    put(live(paths), SAVE_A)
    a, b = backup_saves(paths, NOW), backup_saves(paths, NOW)
    delete_backup(a)
    assert list_backups(paths, DEFAULT) == [b] and tree(live(paths)) == SAVE_A
    with pytest.raises(ValueError):
        delete_backup(paths.user / "savedata")


# ---------------------------------------------------------------- restoring

def test_restore_keeps_current_saves_first_and_replaces_them(tmp_path):
    paths = port(tmp_path)
    put(live(paths), SAVE_A)
    put(paths.user / "cache" / "CUSA00003", {"a.spv": b"shader"})
    older = make_zip(paths.backups / "Default" / "saves-2026-10-01_08-00.zip",
                     {f"savedata/{k}": v for k, v in SAVE_B.items()})
    safety = restore_backup(paths, DEFAULT, older, NOW)
    assert safety == paths.backups / "Default" / "before-restore-2026-10-17_09-05.zip"
    with zipfile.ZipFile(safety) as z:
        assert {n: z.read(n) for n in z.namelist()} == {f"savedata/{k}": v for k, v in SAVE_A.items()}
    assert tree(live(paths)) == SAVE_B
    assert (paths.user / "cache" / "CUSA00003" / "a.spv").read_bytes() == b"shader"
    assert no_leftovers(paths)
    assert restore_backup(paths, DEFAULT, safety, NOW) is not None   # and back again
    assert tree(live(paths)) == SAVE_A


@pytest.mark.parametrize("bad", ["../x", "savedata/../../x", "/savedata/x", "C:/x",
                                 "savedata/C:x", "other/x", "x", "savedata/a/../b",
                                 "savedata//x", "savedata/NUL", "savedata/sp ace "])
def test_restore_rejects_unsafe_entries_before_touching_anything(tmp_path, bad):
    paths = port(tmp_path)
    put(live(paths), SAVE_A)
    z = make_zip(tmp_path / "evil.zip", {"savedata/1/ok": b"fine", bad: b"evil"})
    with pytest.raises(ValueError):
        restore_backup(paths, DEFAULT, z, NOW)
    assert tree(live(paths)) == SAVE_A
    assert list_backups(paths, DEFAULT) == []                 # no safety zip was made either
    assert no_leftovers(paths)
    assert not (tmp_path / "x").exists() and not (paths.root / "x").exists()


def test_restore_rejects_backslash_absolute_and_symlink_entries(tmp_path):
    paths = port(tmp_path)
    put(live(paths), SAVE_A)
    z = tmp_path / "link.zip"
    with zipfile.ZipFile(z, "w") as f:
        info = zipfile.ZipInfo("savedata/link")
        info.external_attr = (0o120777 << 16)
        f.writestr(info, "../../outside")
    with pytest.raises(ValueError):
        restore_backup(paths, DEFAULT, z, NOW)
    z2 = tmp_path / "back.zip"
    with zipfile.ZipFile(z2, "w") as f:
        info = zipfile.ZipInfo("savedata/ok")
        f.writestr(info, b"1")
        raw = zipfile.ZipInfo("x")
        raw.filename = "\\windows\\evil"                    # as some Windows tools write it
        f.writestr(raw, b"2")
    with pytest.raises(ValueError):
        restore_backup(paths, DEFAULT, z2, NOW)
    assert tree(live(paths)) == SAVE_A and no_leftovers(paths)


def test_restore_rejects_damaged_empty_and_non_zip_files(tmp_path):
    paths = port(tmp_path)
    put(live(paths), SAVE_A)
    good = make_zip(tmp_path / "good.zip", {"savedata/1/a": b"x" * 5000})
    raw = bytearray(good.read_bytes())
    i = raw.index(b"x" * 100)
    raw[i:i + 10] = b"y" * 10                                 # stored data: CRC mismatch
    damaged = tmp_path / "damaged.zip"
    damaged.write_bytes(bytes(raw))
    empty = make_zip(tmp_path / "empty.zip", {"savedata/": b""})
    text = tmp_path / "notes.zip"
    text.write_text("not a zip", encoding="utf-8")
    for z in (damaged, empty, text):
        with pytest.raises(ValueError):
            restore_backup(paths, DEFAULT, z, NOW)
    assert tree(live(paths)) == SAVE_A and list_backups(paths, DEFAULT) == []
    assert no_leftovers(paths)


def test_restore_rolls_back_when_the_swap_fails(tmp_path, monkeypatch):
    paths = port(tmp_path)
    put(live(paths), SAVE_A)
    z = make_zip(tmp_path / "b.zip", {f"savedata/{k}": v for k, v in SAVE_B.items()})
    real = os.rename
    calls = []

    def flaky(src, dst):
        if Path(dst) == live(paths) and not calls:            # bringing the new saves in
            calls.append(dst)
            raise PermissionError(13, "Access is denied")
        return real(src, dst)

    monkeypatch.setattr(profiles.os, "rename", flaky)
    with pytest.raises(OSError):
        restore_backup(paths, DEFAULT, z, NOW)
    monkeypatch.setattr(profiles.os, "rename", real)
    assert calls and tree(live(paths)) == SAVE_A and no_leftovers(paths)
    assert len(list_backups(paths, DEFAULT)) == 1             # the safety zip stays


def test_restore_keeps_moved_saves_when_even_the_rollback_fails(tmp_path, monkeypatch):
    paths = port(tmp_path)
    put(live(paths), SAVE_A)
    z = make_zip(tmp_path / "b.zip", {f"savedata/{k}": v for k, v in SAVE_B.items()})
    real = os.rename

    def stuck(src, dst):
        if Path(dst) == live(paths):
            raise PermissionError(13, "Access is denied")
        return real(src, dst)

    monkeypatch.setattr(profiles.os, "rename", stuck)
    with pytest.raises(RuntimeError, match="safe in"):
        restore_backup(paths, DEFAULT, z, NOW)
    monkeypatch.setattr(profiles.os, "rename", real)
    parked = [d / "old" for d in paths.user.iterdir() if d.name.startswith(".dcport-restore-")]
    assert len(parked) == 1 and tree(parked[0]) == SAVE_A      # not deleted
    z2 = make_zip(tmp_path / "c.zip", {"savedata/1/c": b"C"})
    restore_backup(paths, DEFAULT, z2, NOW)                   # the next restore puts it back first
    assert tree(live(paths)) == {"1/c": b"C"} and no_leftovers(paths)
    kept = [zipfile.ZipFile(p) for p in list_backups(paths, DEFAULT)]
    assert any(sorted(k.namelist()) == sorted(f"savedata/{n}" for n in SAVE_A) for k in kept)


def test_restore_cleans_up_after_a_crash_and_recovers_moved_saves(tmp_path):
    paths = port(tmp_path)
    put(paths.user / ".dcport-restore-abc" / "savedata", {"half": b"partial"})
    put(paths.user / ".dcport-restore-abc" / "old", SAVE_A)   # crashed mid-swap
    z = make_zip(tmp_path / "b.zip", {f"savedata/{k}": v for k, v in SAVE_B.items()})
    safety = restore_backup(paths, DEFAULT, z, NOW)
    assert safety is not None                                 # the recovered saves were kept
    with zipfile.ZipFile(safety) as f:
        assert {n: f.read(n) for n in f.namelist()} == {f"savedata/{k}": v for k, v in SAVE_A.items()}
    assert tree(live(paths)) == SAVE_B and no_leftovers(paths)


def recovered(paths) -> list[Path]:
    base = paths.backups / "Default"
    return sorted(base.glob("recovered-*")) if base.is_dir() else []


def test_leftover_with_saves_is_kept_under_recovered_never_deleted(tmp_path):
    paths = port(tmp_path)
    put(paths.user / ".dcport-restore-abc" / "old", SAVE_A)   # crashed mid-restore...
    put(live(paths), SAVE_B)                                  # ...then the game made new saves
    z = make_zip(tmp_path / "c.zip", {"savedata/1/c": b"C"})
    restore_backup(paths, DEFAULT, z, NOW)
    assert tree(live(paths)) == {"1/c": b"C"} and no_leftovers(paths)
    kept = recovered(paths)
    assert len(kept) == 1 and tree(kept[0] / "old") == SAVE_A


def test_empty_leftovers_are_removed(tmp_path):
    paths = port(tmp_path)
    (paths.user / ".dcport-restore-e1" / "old" / "sub").mkdir(parents=True)
    (paths.user / ".dcport-restore-e2").mkdir()
    put(live(paths), SAVE_A)
    assert recover(paths, DEFAULT) == [] and no_leftovers(paths)
    (paths.user / ".dcport-restore-e3" / "savedata").mkdir(parents=True)
    z = make_zip(tmp_path / "b.zip", {f"savedata/{k}": v for k, v in SAVE_B.items()})
    restore_backup(paths, DEFAULT, z, NOW)
    assert no_leftovers(paths) and recovered(paths) == []


def test_recover_moves_parked_saves_back_after_an_interrupted_switch(tmp_path):
    paths = port(tmp_path)
    put(slot(paths, DEFAULT), SAVE_A)                         # savedata missing
    msgs = recover(paths, DEFAULT)
    assert len(msgs) == 1 and tree(live(paths)) == SAVE_A and tree(slot(paths, DEFAULT)) == {}
    assert recover(paths, DEFAULT) == []


def test_recover_moves_back_old_from_a_restore_leftover(tmp_path):
    paths = port(tmp_path)
    put(paths.user / ".dcport-restore-x" / "old", SAVE_A)
    put(paths.user / ".dcport-restore-x" / "savedata", {"half": b"p"})
    msgs = recover(paths, DEFAULT)
    assert tree(live(paths)) == SAVE_A and msgs
    assert [tree(d) for d in recovered(paths)] == [{"savedata/half": b"p"}]
    assert no_leftovers(paths)


def test_recover_preserves_non_empty_leftover_when_savedata_exists(tmp_path):
    paths = port(tmp_path)
    put(live(paths), SAVE_B)
    put(paths.user / ".dcport-restore-x" / "old", SAVE_A)
    assert len(recover(paths, DEFAULT)) == 1
    assert tree(live(paths)) == SAVE_B and tree(recovered(paths)[0] / "old") == SAVE_A


def test_restore_refuses_when_the_disk_is_too_full(tmp_path, monkeypatch):
    paths = port(tmp_path)
    put(live(paths), SAVE_A)
    z = make_zip(tmp_path / "b.zip", {"savedata/1/a": b"x" * 1000})
    monkeypatch.setattr(profiles.shutil, "disk_usage",
                        lambda p: type("U", (), {"free": 10, "total": 10, "used": 0})())
    with pytest.raises(OSError):
        restore_backup(paths, DEFAULT, z, NOW)
    assert tree(live(paths)) == SAVE_A and list_backups(paths, DEFAULT) == []


# ---------------------------------------------------------------- settings

def test_settings_profile_default_old_files_and_bad_values(tmp_path):
    paths = port(tmp_path)
    assert load_settings(paths).profile == "Default"
    paths.root.mkdir(parents=True)
    paths.settings_json.write_text('{"fps": 90}', encoding="utf-8")     # before profiles
    s = load_settings(paths)
    assert s.profile == "Default" and s.fps == 90
    save_settings(paths, LauncherSettings(profile="Sam"))
    assert load_settings(paths).profile == "Sam"
    for bad in ('"../x"', '"CON"', '""', "5", '"' + "x" * 40 + '"'):
        paths.settings_json.write_text('{"profile": %s}' % bad, encoding="utf-8")
        assert load_settings(paths).profile == "Default"
