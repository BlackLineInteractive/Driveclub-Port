# SPDX-License-Identifier: GPL-2.0-or-later
import os
import shutil
import subprocess
import zipfile
from datetime import datetime
from pathlib import Path

import pytest

from dcport import steam_vdf, sysinfo
from dcport import garage
from dcport.garage import (SteamError, add_to_steam, backup_saves, clear_shader_cache,
                           create_desktop_shortcut, desktop_dir, launch_target,
                           steam_running, steam_userdata_dirs)
from dcport.paths import AppPaths


def test_vdf_roundtrip_and_existing_entries_kept(tmp_path):
    user = tmp_path / "userdata" / "123" / "config"; user.mkdir(parents=True)
    other = {"shortcuts": {"0": {"appid": 7, "AppName": "Other Game", "Exe": '"C:/x.exe"'}}}
    (user / "shortcuts.vdf").write_bytes(steam_vdf.dump(other))
    paths = AppPaths.for_root(tmp_path / "Drive Club port")
    assert add_to_steam(paths, True, [tmp_path / "userdata" / "123"]) == 1
    assert add_to_steam(paths, True, [tmp_path / "userdata" / "123"]) == 1      # updates, no duplicate
    entries = steam_vdf.load((user / "shortcuts.vdf").read_bytes())["shortcuts"]
    names = [e["AppName"] for e in entries.values()]
    assert names == ["Other Game", "DRIVECLUB (DCport)"]
    assert entries["1"]["LaunchOptions"] == "--play"
    assert (user / "shortcuts.vdf.bak").exists()
    exe = str(paths.root / "DriveClub.exe")
    assert entries["1"]["Exe"] == f'"{exe}"'
    assert entries["1"]["icon"] == exe
    assert entries["1"]["appid"] >= 2 ** 31


def test_vdf_unknown_keys_and_high_ints_roundtrip():
    obj = {"shortcuts": {"0": {"appid": 0xDEADBEEF, "tags": {"0": "fav"}, "x": 5}}}
    assert steam_vdf.load(steam_vdf.dump(obj)) == obj


def test_vdf_malformed():
    with pytest.raises(ValueError):
        steam_vdf.load(b"\x00garbage")


def test_no_steam_users():
    with pytest.raises(SteamError):
        add_to_steam(AppPaths.for_root(Path("x")), False, [])


def test_corrupt_vdf_is_set_aside(tmp_path):
    cfg = tmp_path / "u" / "config"; cfg.mkdir(parents=True)
    (cfg / "shortcuts.vdf").write_bytes(b"\x00garbage")
    assert add_to_steam(AppPaths.for_root(tmp_path), False, [tmp_path / "u"]) == 1
    assert (cfg / "shortcuts.vdf.bad").exists()
    names = [e["AppName"] for e in steam_vdf.load(
        (cfg / "shortcuts.vdf").read_bytes())["shortcuts"].values()]
    assert "DRIVECLUB (DCport)" in names


def test_steam_userdata_dirs(tmp_path):
    for n in ("0", "123", "abc", "456"):
        (tmp_path / "userdata" / n).mkdir(parents=True)
    got = sorted(p.name for p in steam_userdata_dirs(tmp_path))
    assert got == ["123", "456"]
    assert steam_userdata_dirs(tmp_path / "missing") == []


def test_backup_and_clear_cache(tmp_path):
    paths = AppPaths.for_root(tmp_path)
    with pytest.raises(FileNotFoundError):
        backup_saves(paths, datetime(2026, 10, 17, 9, 5))
    (paths.user / "savedata" / "1" / "CUSA00003").mkdir(parents=True)
    (paths.user / "savedata" / "1" / "CUSA00003" / "s.bin").write_bytes(b"1")
    z = backup_saves(paths, datetime(2026, 10, 17, 9, 5))
    assert z.name == "saves-2026-10-17_09-05.zip" and zipfile.ZipFile(z).namelist()
    (paths.user / "cache" / "CUSA00003").mkdir(parents=True)
    (paths.user / "cache" / "CUSA00003" / "p.spv").write_bytes(b"1")
    (paths.user / "cache" / "p.bin").write_bytes(b"1")
    (paths.user / "shader").mkdir()
    (paths.user / "shader" / "a.bin").write_bytes(b"1")
    assert clear_shader_cache(paths) == 3
    assert (paths.user / "savedata" / "1" / "CUSA00003" / "s.bin").exists()


def test_launch_target(tmp_path):
    paths = AppPaths.for_root(tmp_path)
    assert launch_target(paths, True) == (tmp_path / "DriveClub.exe", "--play")
    assert launch_target(paths, False) == (tmp_path / "DriveClub.exe", "")


@pytest.mark.skipif(shutil.which("powershell") is None, reason="no powershell")
def test_desktop_shortcut_odd_path(tmp_path):
    paths = AppPaths.for_root(tmp_path / "Drive Club's port")
    paths.root.mkdir()
    desktop = tmp_path / "desk"; desktop.mkdir()
    lnk = create_desktop_shortcut(paths, True, desktop)
    assert lnk.exists() and lnk.name == "DRIVECLUB.lnk"
    env = dict(os.environ, DC_LNK=str(lnk))
    r = subprocess.run(
        ["powershell", "-NoProfile", "-NonInteractive", "-Command",
         "$l = (New-Object -ComObject WScript.Shell).CreateShortcut($env:DC_LNK); "
         "$l.TargetPath; $l.Arguments"],
        env=env, capture_output=True, text=True, creationflags=0x08000000)
    target, args = (r.stdout.splitlines() + ["", ""])[:2]
    assert target.endswith("DriveClub.exe") and args == "--play"


def test_steam_running_is_bool():
    assert isinstance(steam_running(), bool)


def test_gpu_name_str_or_none():
    r = sysinfo.gpu_name()
    assert r is None or isinstance(r, str)


def test_bak_keeps_original_bytes(tmp_path):
    cfg = tmp_path / "u" / "config"; cfg.mkdir(parents=True)
    orig = steam_vdf.dump({"shortcuts": {"0": {"appid": 7, "AppName": "Other"}}})
    (cfg / "shortcuts.vdf").write_bytes(orig)
    paths = AppPaths.for_root(tmp_path / "p")
    add_to_steam(paths, True, [tmp_path / "u"])
    add_to_steam(paths, False, [tmp_path / "u"])
    assert (cfg / "shortcuts.vdf.bak").read_bytes() == orig


def test_other_value_types_survive(tmp_path):
    cfg = tmp_path / "u" / "config"; cfg.mkdir(parents=True)
    entry = (b"\x00" + b"0\x00"
             + b"\x07big\x00" + bytes(range(1, 9))
             + b"\x03f\x00" + b"\x00\x00\x80\x3f"
             + b"\x0aneg\x00" + b"\xff" * 8
             + b"\x01AppName\x00Other\x00" + b"\x08")
    raw = b"\x00shortcuts\x00" + entry + b"\x08\x08"
    (cfg / "shortcuts.vdf").write_bytes(raw)
    assert add_to_steam(AppPaths.for_root(tmp_path / "p"), True, [tmp_path / "u"]) == 1
    assert not (cfg / "shortcuts.vdf.bad").exists()
    out = (cfg / "shortcuts.vdf").read_bytes()
    assert out.startswith(b"\x00shortcuts\x00" + entry)
    with pytest.raises(ValueError):
        steam_vdf.load(b"\x00s\x00\x09k\x00\x00\x08\x08")


def test_vdf_golden_bytes():
    obj = {"shortcuts": {"0": {"appid": 0x80000001, "AppName": "A", "tags": {}}}}
    expected = (b"\x00shortcuts\x00"
                b"\x000\x00"
                b"\x02appid\x00\x01\x00\x00\x80"
                b"\x01AppName\x00A\x00"
                b"\x00tags\x00\x08"
                b"\x08\x08\x08")
    assert steam_vdf.dump(obj) == expected


def test_desktop_dir_is_the_real_desktop_folder():
    d = desktop_dir()                                     # read only: nothing is written there
    assert d.is_absolute() and d.is_dir()


def test_desktop_dir_falls_back_to_the_profile(tmp_path, monkeypatch):
    def broken(folder_id):
        raise OSError("no shell32")

    monkeypatch.setattr(garage, "_known_folder", broken)
    monkeypatch.setenv("USERPROFILE", str(tmp_path))
    assert desktop_dir() == tmp_path / "Desktop"
