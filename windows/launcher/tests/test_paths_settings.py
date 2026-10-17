# SPDX-License-Identifier: GPL-2.0-or-later
from dcport.paths import AppPaths
from dcport.settings import load_settings, save_settings


def test_paths_layout(tmp_path):
    p = AppPaths.for_root(tmp_path / "Drive Club port")
    assert p.runtime == tmp_path / "Drive Club port" / "bin" / "DriveClub.exe"
    assert p.ini.name == "dcport.ini" and p.settings_json.name == "dcport-launcher.json"


def test_settings_defaults_and_roundtrip(tmp_path):
    p = AppPaths.for_root(tmp_path)
    s = load_settings(p)
    assert (s.fps, s.fullscreen, s.theme) == (120, False, "dark")
    s.fps, s.theme, s.game_dir = 144, "light", r"D:\Games\CUSA00003"
    save_settings(p, s)
    assert load_settings(p) == s


def test_settings_migrates_game_dir_txt(tmp_path):
    (tmp_path / "game_dir.txt").write_text(r"..\driveclub files\CUSA00003", encoding="utf-8")
    s = load_settings(AppPaths.for_root(tmp_path))
    assert s.game_dir == str((tmp_path / ".." / "driveclub files" / "CUSA00003").resolve())


def test_settings_corrupt_json_gives_defaults(tmp_path):
    (tmp_path / "dcport-launcher.json").write_text("{not json", encoding="utf-8")
    assert load_settings(AppPaths.for_root(tmp_path)).fps == 120


def test_settings_wrong_types_and_unknown_keys(tmp_path):
    (tmp_path / "dcport-launcher.json").write_text(
        '{"fps": "fast", "theme": "light", "bogus": 1}', encoding="utf-8")
    s = load_settings(AppPaths.for_root(tmp_path))
    assert s.fps == 120 and s.theme == "light"


def test_current_uses_env(tmp_path, monkeypatch):
    monkeypatch.setenv("DCPORT_ROOT", str(tmp_path))
    assert AppPaths.current().root == tmp_path


def test_settings_fps_out_of_range_falls_back(tmp_path):
    paths = AppPaths.for_root(tmp_path)
    for bad in (0, -5, 29, 241, 100000):
        paths.settings_json.write_text('{"fps": %d}' % bad, encoding="utf-8")
        assert load_settings(paths).fps == 120
    paths.settings_json.write_text('{"fps": 165}', encoding="utf-8")
    assert load_settings(paths).fps == 165
