# SPDX-License-Identifier: GPL-2.0-or-later
from dcport.paths import AppPaths
from dcport.runtime import fps_note, game_command, game_env, parse_fps
from dcport.settings import LauncherSettings
from tests.helpers import make_data


def test_parse_fps():
    assert parse_fps("144") == 144 and parse_fps(" 30 ") == 30
    assert parse_fps("29") is None and parse_fps("241") is None and parse_fps("fast") is None


def test_fps_note():
    assert fps_note(120) == "" and fps_note(30) == ""
    assert fps_note(144) == "DRIVECLUB was tested up to 120 FPS; above that its timing may suffer."


def test_command_keeps_spaces_and_passes_patches(tmp_path):
    paths = AppPaths.for_root(tmp_path / "Drive Club port é")
    make_data(paths)
    game = tmp_path / "driveclub files" / "CUSA00003"
    cmd = game_command(paths, game, 120)
    assert cmd[cmd.index("--app0") + 1] == str(game) and "--patches" in cmd
    # at 30 FPS the session's patches.bin is compiled without the 60 FPS patch
    assert "--patches" in game_command(paths, game, 30)
    assert cmd[0] == str(paths.runtime) and cmd[-2:] == ["--timeout", "0"]
    assert game_command(paths, game, 60, ["--x"])[-1] == "--x"


def test_command_no_patches_file(tmp_path):
    paths = AppPaths.for_root(tmp_path)
    assert "--patches" not in game_command(paths, tmp_path / "g", 120)


def test_env_editions_and_frame_rate(tmp_path):
    paths = AppPaths.for_root(tmp_path)
    s = LauncherSettings(fps=90, fullscreen=True)
    base = {"PATH": "x", "BB_FSR4_DIR": "old"}
    e = game_env(paths, s, fsr=False, base=base)
    assert (e["BB_VBLANK_HZ"], e["BB_FPS_LIMIT"], e["BB_FULLSCREEN"]) == ("90", "90", "1")
    assert e["BB_UPSCALER"] == "none" and e["BB_DC_UPSCALE"] == "0" and e["PATH"] == "x"
    assert "BB_FSR4_DIR" not in e and base == {"PATH": "x", "BB_FSR4_DIR": "old"}
    e = game_env(paths, LauncherSettings(fps=30), fsr=True, base={"BB_UPSCALER": "z"})
    assert (e["BB_VBLANK_HZ"], e["BB_FPS_LIMIT"], e["BB_DC_UPSCALE"]) == ("60", "0", "1")
    assert e["BB_FSR4_DIR"] == str(paths.fsr4_dir) and "BB_UPSCALER" not in e


def test_parse_fps_edges():
    assert parse_fps("30") == 30 and parse_fps("240") == 240
    for bad in ("+60", "60.0", "", "   ", "-60", "٦٠", "６０", "29", "241"):
        assert parse_fps(bad) is None, bad


def test_env_fixed_values(tmp_path):
    paths = AppPaths.for_root(tmp_path)
    for fullscreen, expected in ((True, "1"), (False, "0")):
        e = game_env(paths, LauncherSettings(fps=60, fullscreen=fullscreen), fsr=False, base={})
        assert e["BB_FULLSCREEN"] == expected
        assert e["BB_GPU_USER_DIR"] == str(paths.user)
        assert e["BB_CONFIG"] == str(paths.ini)
        assert e["BB_LENIENT_IMPORTS"] == "1"
        assert e["BB_READBACKS"] == "0"
        assert e["BB_DRAW_PIPE"] == "1"
        assert e["BB_PREP_WORKERS"] == "0"
        assert e["BB_WINDOW_SIZE"] == "1920x1080" and e["BB_DMEM_MB"] == "5056"
        assert all(isinstance(v, str) for v in e.values())


def test_env_window_and_render_resolution(tmp_path):
    paths = AppPaths.for_root(tmp_path)
    s = LauncherSettings(window_size="2560x1440", render_res="2160p")
    e = game_env(paths, s, fsr=True, base={})
    assert e["BB_WINDOW_SIZE"] == "2560x1440" and e["BB_DMEM_MB"] == "5056"
    assert e["BB_FSR1"] == "1" and "BB_FSR1" not in game_env(paths, s, fsr=False, base={})
    e = game_env(paths, LauncherSettings(window_size="99x1", render_res="8k"), fsr=True, base={})
    assert e["BB_WINDOW_SIZE"] == "1920x1080" and e["BB_DMEM_MB"] == "5056"


def test_player_name(tmp_path):
    paths = AppPaths.for_root(tmp_path)
    paths.ini.parent.mkdir(parents=True, exist_ok=True)
    assert game_env(paths, LauncherSettings(), fsr=True, base={})["BB_USER_NAME"] == "Player"
    paths.ini.write_text("player_name= qnfn " + chr(10), encoding="utf-8")
    assert game_env(paths, LauncherSettings(), fsr=True, base={})["BB_USER_NAME"] == "qnfn"
    paths.ini.write_text("player_name=" + "x" * 30 + chr(10), encoding="utf-8")
    assert game_env(paths, LauncherSettings(), fsr=True, base={})["BB_USER_NAME"] == "x" * 16


def test_threaded_recording_setting(tmp_path):
    paths = AppPaths.for_root(tmp_path)
    paths.ini.parent.mkdir(parents=True, exist_ok=True)
    for value, expected in (("0", "0"), ("1", "1"), ("auto", None), ("", None)):
        paths.ini.write_text(f"threaded_recording={value}" + chr(10), encoding="utf-8")
        e = game_env(paths, LauncherSettings(), fsr=True, base={"BB_THREADED_RECORDING": "1"})
        assert e.get("BB_THREADED_RECORDING") == expected


def test_language(tmp_path):
    paths = AppPaths.for_root(tmp_path)
    paths.ini.parent.mkdir(parents=True, exist_ok=True)
    assert game_env(paths, LauncherSettings(), fsr=True, base={})["DC_LANGUAGE"] == "1"
    paths.ini.write_text("language=2" + chr(10), encoding="utf-8")
    assert game_env(paths, LauncherSettings(), fsr=True, base={})["DC_LANGUAGE"] == "2"
    paths.ini.write_text("language=99" + chr(10), encoding="utf-8")
    assert game_env(paths, LauncherSettings(), fsr=True, base={})["DC_LANGUAGE"] == "1"


def test_languages_per_region():
    from dcport.runtime import LANGUAGES, languages_for
    eu, us = dict(languages_for("CUSA00003")), dict(languages_for("CUSA00093"))
    assert len(eu) == 14 and "19" in eu and "17" not in eu
    assert set(us) == {"1", "2", "17", "20"}
    assert set(dict(LANGUAGES)) == set(eu) | set(us)
