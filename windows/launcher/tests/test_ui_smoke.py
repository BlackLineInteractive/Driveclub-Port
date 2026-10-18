# SPDX-License-Identifier: GPL-2.0-or-later
import re
import sys
import threading
import time
import types
import zipfile
from pathlib import Path

import pytest

import dcport
import dcport.profiles
from dcport.controls import InputConfig
from dcport.garage import SteamError
from dcport.ini import read_ini
from dcport.paths import AppPaths
from dcport.prepare_state import prepare_command, source_stamp
from dcport.profiles import create_profile
from dcport.runtime import game_command
from dcport.settings import LauncherSettings, load_settings, save_settings
from dcport.ui import theme
from dcport.ui.app import App as _App
from dcport.ui.log import read_tail
from tests.helpers import make_port

DEFAULT_INPUT = Path(__file__).parent.parent / "input-default.ini"


@pytest.fixture(autouse=True)
def _no_powershell(monkeypatch):
    """UI tests don't start PowerShell for the GPU name (slow, and machine-dependent)."""
    monkeypatch.setattr("dcport.sysinfo.gpu_name", lambda: "Test GPU")


@pytest.fixture(autouse=True)
def _no_real_system(monkeypatch, tmp_path):
    """UI tests never touch the real Desktop, Steam, Explorer or clipboard."""
    def refuse(*args, **kwargs):
        raise AssertionError("a UI test reached the real system")

    monkeypatch.setenv("USERPROFILE", str(tmp_path / "profile"))
    monkeypatch.setattr("dcport.garage.desktop_dir", lambda: tmp_path / "profile" / "Desktop")
    for name in ("steam_running", "steam_userdata_dirs", "add_to_steam",
                 "create_desktop_shortcut"):
        monkeypatch.setattr(f"dcport.garage.{name}", refuse)
    monkeypatch.setattr("os.startfile", refuse, raising=False)
    monkeypatch.setattr(_App, "clipboard_clear", refuse)
    monkeypatch.setattr(_App, "clipboard_append", refuse)


def test_app_builds_switches_tabs_and_theme(tmp_path):
    paths = AppPaths.for_root(tmp_path); app = App(paths, fsr=False)
    try:
        for tab in ("race", "setup", "controls", "garage", "log"):
            app.show(tab); app.update()
        app.toggle_theme(); app.update()
        assert load_settings(paths).theme == "light"
    finally:
        app.destroy()


class FakeRunner:
    """Stands in for process.RUNNER; each wait() returns the next exit code."""

    def __init__(self, codes, block=False):
        self.codes = list(codes)
        self.starts = []
        self._running = False
        self._stopped = False
        self._block = block
        self._release = threading.Event()

    def start(self, cmd, env, log, cwd):
        if self._running:
            return False
        self.starts.append((cmd, env, log, cwd))
        self._running, self._stopped = True, False
        self._release.clear()
        return True

    def running(self):
        return self._running

    def wait(self, timeout=None):
        if self._block:
            self._release.wait(5)
        self._running = False
        return self.codes.pop(0)

    def was_stopped(self):
        return self._stopped

    def stop(self):
        self._stopped = True
        self._release.set()


def App(paths, fsr):
    """An app whose startup save recovery has finished (the tests below start from there)."""
    app = _App(paths, fsr=fsr)
    pump(app, lambda: not app.recovering)
    return app


def pump(app, done, timeout=5.0):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        app.update()
        if done():
            return
        time.sleep(0.01)
    raise AssertionError("timed out")


def configured_port(tmp_path, prepared):
    paths, game = make_port(tmp_path, with_data=prepared)
    if prepared:
        stamp = source_stamp(game / "eboot.bin", paths.runtime, paths.patches_xml)
        (paths.data / "source.txt").write_text(stamp, encoding="utf-8")
    save_settings(paths, LauncherSettings(game_dir=str(game)))
    return paths, game


def test_play_prepares_then_starts_game_and_reports_exit(tmp_path):
    paths, game = configured_port(tmp_path, prepared=False)
    app = App(paths, fsr=False)
    try:
        app.runner = runner = FakeRunner([0, 3])
        app.play()
        pump(app, lambda: len(runner.starts) == 2 and app.phase == "idle")
        prep, run = runner.starts
        assert prep[0] == prepare_command(paths, game) and prep[2] == paths.prepare_log
        assert prep[3] == paths.root
        assert run[0] == game_command(paths, game, 120) and run[2] == paths.log
        assert run[1]["BB_UPSCALER"] == "none" and app.last_log == paths.log
        assert "DRIVECLUB stopped (code 3)" in app.pages["race"].status_text()
    finally:
        app.destroy()


def test_failed_prepare_does_not_start_game(tmp_path):
    paths, _ = configured_port(tmp_path, prepared=False)
    app = App(paths, fsr=False)
    try:
        app.runner = runner = FakeRunner([2])
        app.play()
        pump(app, lambda: app.phase == "idle" and runner.starts)
        assert len(runner.starts) == 1 and app.last_log == paths.prepare_log
        assert "Preparing game files failed (code 2)" in app.pages["race"].status_text()
    finally:
        app.destroy()


def test_play_refused_while_running_and_stop_is_quiet(tmp_path):
    paths, game = configured_port(tmp_path, prepared=True)
    app = App(paths, fsr=True)
    try:
        app.runner = runner = FakeRunner([1], block=True)
        app.play()
        assert app.phase == "running" and len(runner.starts) == 1
        assert runner.starts[0][1]["BB_DC_UPSCALE"] == "1"
        app.play()
        assert len(runner.starts) == 1
        app.stop_game()
        pump(app, lambda: app.phase == "idle")
        assert app.pages["race"].status_text() == ""
    finally:
        app.destroy()


def test_session_card_saves_immediately(tmp_path):
    paths = AppPaths.for_root(tmp_path); app = App(paths, fsr=False)
    try:
        race = app.pages["race"]
        race.set_fps(144)
        assert load_settings(paths).fps == 144 and "tested up to 120" in race.fps_note_text()
        race.set_fullscreen(True)
        assert load_settings(paths).fullscreen is True
        race.set_fps_counter(False)
        assert read_ini(paths.ini)["show_fps"] == "0"
        race.set_fps(60)
        assert race.fps_note_text() == ""
    finally:
        app.destroy()


def test_main_play_routes_to_play_module(tmp_path, monkeypatch):
    from dcport.__main__ import main
    calls = []
    from dcport import play
    monkeypatch.setattr(play, "run",
                        lambda paths, fsr, extra: calls.append((paths.root, extra)) or 7)
    monkeypatch.setenv("DCPORT_ROOT", str(tmp_path))
    assert main(["--play", "--foo"]) == 7
    assert calls == [(tmp_path, ["--foo"])]


# ---------------------------------------------------------------- SETUP (Task 10)

def test_setup_custom_fps_validation(tmp_path):
    paths = AppPaths.for_root(tmp_path); app = App(paths, fsr=True)
    try:
        app.show("setup"); page = app.pages["setup"]
        assert page.apply_custom_fps("500") is False and load_settings(paths).fps == 120
        assert page.apply_custom_fps("165") is True and load_settings(paths).fps == 165
        assert "tested up to 120" in page.fps_note_text()
        page.set_upscaler("fsr4"); assert read_ini(paths.ini)["upscaler"] == "fsr4"
    finally:
        app.destroy()

def test_no_fsr_edition_hides_upscaler(tmp_path):
    app = App(AppPaths.for_root(tmp_path), fsr=False)
    try:
        app.show("setup"); assert not app.pages["setup"].has_upscaler_controls()
    finally:
        app.destroy()


def test_setup_custom_fps_error_message_and_not_saved(tmp_path):
    paths = AppPaths.for_root(tmp_path); app = App(paths, fsr=False)
    try:
        app.show("setup"); page = app.pages["setup"]
        for bad in ("29", "241", "abc", "", "12.5"):
            assert page.apply_custom_fps(bad) is False
            assert page.fps_error_text() == "Enter a whole number from 30 to 240"
        assert not paths.settings_json.exists() and app.settings.fps == 120
        assert page.apply_custom_fps(" 240 ") is True and page.fps_error_text() == ""
    finally:
        app.destroy()


def test_setup_and_session_card_share_one_settings_object(tmp_path):
    paths = AppPaths.for_root(tmp_path); app = App(paths, fsr=False)
    try:
        race, setup = app.pages["race"], app.pages["setup"]
        assert setup.app.settings is app.settings
        race.set_fps(144)
        app.show("setup")
        assert setup.selected_fps() == 144 and "tested up to 120" in setup.fps_note_text()
        setup.set_fps(60); setup.set_fullscreen(True); setup.set_fps_counter(False)
        assert app.settings.fps == 60 and load_settings(paths).fullscreen is True
        assert read_ini(paths.ini)["show_fps"] == "0"
        app.show("race")
        assert race.session_values() == {"profile": "Default", "fps": "60", "window": "Fullscreen",
                                         "counter": "Off"}
        race.set_fps_counter(True); race.set_fullscreen(False)
        app.show("setup")
        assert setup.selected_window() == "windowed" and setup.selected_fps_counter() is True
    finally:
        app.destroy()


def test_fps_counter_key_written_once_at_start(tmp_path):
    paths = AppPaths.for_root(tmp_path)
    assert not paths.ini.exists()
    app = App(paths, fsr=False)                           # R20: no dcport.ini yet
    try:
        assert read_ini(paths.ini)["show_fps"] == "1"
        app.show("setup")
        assert app.pages["setup"].selected_fps_counter() is True
    finally:
        app.destroy()
    paths.ini.write_text("# mine\nupscaler=fsr3\nshow_fps=0\n", encoding="utf-8")
    app = App(paths, fsr=False)                           # an existing value is left alone
    try:
        assert paths.ini.read_text(encoding="utf-8") == "# mine\nupscaler=fsr3\nshow_fps=0\n"
        app.show("setup")
        assert app.pages["setup"].selected_fps_counter() is False
    finally:
        app.destroy()
    paths.ini.write_text("upscaler=off\n", encoding="utf-8")
    App(paths, fsr=False).destroy()                       # added to an ini without the key
    assert paths.ini.read_text(encoding="utf-8") == "upscaler=off\nshow_fps=1\n"


def test_setup_upscaler_writes_jitter_and_sharpness_on_release(tmp_path):
    paths = AppPaths.for_root(tmp_path); app = App(paths, fsr=True)
    try:
        app.show("setup"); page = app.pages["setup"]
        page.set_upscaler("fsr3")
        assert read_ini(paths.ini)["upscaler"] == "fsr3" and read_ini(paths.ini)["jitter"] == "0"
        page.set_upscaler("off")
        assert read_ini(paths.ini)["upscaler"] == "off"
        page.sharpness.drag_to(0.8)                      # moving the slider writes nothing
        assert "sharpness" not in read_ini(paths.ini)
        assert page.sharpness_text() == "0.80"
        page.sharpness.release()                          # letting go writes 2 decimals
        assert read_ini(paths.ini)["sharpness"] == "0.80"
        page.set_sharpness(0.456)
        assert read_ini(paths.ini)["sharpness"] == "0.46"
        page.set_sharpness(3)
        assert read_ini(paths.ini)["sharpness"] == "1.00"
    finally:
        app.destroy()


def test_setup_fsr4_greyed_without_model_files(tmp_path):
    paths = AppPaths.for_root(tmp_path); app = App(paths, fsr=True)
    try:
        app.show("setup"); page = app.pages["setup"]
        assert page.fsr4_available() is False
        assert page.upscaler_note_text() == \
            "FSR 4 needs the fsr4_shaders folder next to DriveClub.exe"
        paths.fsr4_dir.mkdir()
        (paths.fsr4_dir / "LICENSE-FSR4-v07.txt").write_text("x", encoding="utf-8")
        app.show("race"); app.show("setup")
        assert page.fsr4_available() is False             # a folder without models is not enough
        (paths.fsr4_dir / "fsr4_model_v07_i8_balanced_1080_pass1.spv").write_bytes(b"spv")
        app.show("race"); app.show("setup")
        from dcport.ui.setup import AA_NOTE
        assert page.fsr4_available() is True and page.upscaler_note_text() == AA_NOTE
    finally:
        app.destroy()


def test_prepare_again_runs_prepare_only(tmp_path):
    paths, game = configured_port(tmp_path, prepared=True)
    app = App(paths, fsr=False)
    try:
        app.runner = runner = FakeRunner([0])
        app.show("setup")
        assert (paths.data / "source.txt").exists()
        assert app.pages["setup"].prepare_again() is True
        assert not (paths.data / "source.txt").exists()
        assert app.current == "race" and app.phase == "preparing"
        assert app.pages["setup"].prepare_enabled() is False
        pump(app, lambda: app.phase == "idle")
        assert len(runner.starts) == 1 and runner.starts[0][0] == prepare_command(paths, game)
        assert app.pages["race"].status_text() == "Game files prepared"
        assert app.pages["setup"].prepare_enabled() is True
    finally:
        app.destroy()


def test_prepare_again_refused_while_running(tmp_path):
    paths, _ = configured_port(tmp_path, prepared=True)
    app = App(paths, fsr=False)
    try:
        app.runner = runner = FakeRunner([0], block=True)
        app.play()
        assert app.phase == "running"
        app.show("setup")
        assert app.pages["setup"].prepare_enabled() is False
        assert app.pages["setup"].prepare_again() is False
        assert len(runner.starts) == 1 and (paths.data / "source.txt").exists()
        app.stop_game()
        pump(app, lambda: app.phase == "idle")
    finally:
        app.destroy()


# ---------------------------------------------------------------- App helpers (Task 10)

def test_run_task_reports_on_the_ui_thread(tmp_path):
    app = App(AppPaths.for_root(tmp_path), fsr=False)
    try:
        got = []
        app.run_task(lambda: threading.current_thread() is threading.main_thread(),
                     lambda result, error: got.append(
                         (result, error, threading.current_thread() is threading.main_thread())))

        def boom():
            raise OSError("disk full")

        app.run_task(boom, lambda result, error: got.append((result, str(error), True)))
        pump(app, lambda: len(got) == 2)
        assert (False, None, True) in got and (None, "disk full", True) in got
    finally:
        app.destroy()


def test_version_and_bundled_resources(monkeypatch, tmp_path):
    assert dcport.__version__ == "0.3.7"
    assert theme.default_controls() == DEFAULT_INPUT.resolve()
    assert theme.assets_dir() == DEFAULT_INPUT.resolve().parent / "assets"
    monkeypatch.setattr(sys, "_MEIPASS", str(tmp_path), raising=False)
    assert theme.default_controls() == tmp_path / "input-default.ini"
    assert theme.assets_dir() == tmp_path / "assets"


# ---------------------------------------------------------------- CONTROLS (Task 10)

def test_controls_create_input_ini_from_defaults(tmp_path):
    paths = AppPaths.for_root(tmp_path); app = App(paths, fsr=False)
    try:
        assert not paths.input_ini.exists()
        app.show("controls")
        assert paths.input_ini.read_bytes() == DEFAULT_INPUT.read_bytes()
        page = app.pages["controls"]
        assert page.binding_text("cross", "keyboard") == "Space"
        assert page.binding_text("r3", "keyboard") == "Q / C"
    finally:
        app.destroy()


def test_controls_rebind_keyboard_refusals_and_escape(tmp_path):
    paths = AppPaths.for_root(tmp_path); app = App(paths, fsr=False)
    try:
        app.show("controls"); page = app.pages["controls"]
        page.start_rebind("r2")
        assert page.press_key("F7") is False
        assert page.note_text() == "F7 and F8 are reserved (mouse toggle / reload controls)"
        assert page.press_key("F8") is False and page.pending() == "r2"
        assert page.press_key("F13") is False and page.pending() == "r2"
        assert "can't be used" in page.note_text()
        assert InputConfig.load(paths.input_ini).keyboard("r2") == ["f"]
        assert page.press_key("g") is True and page.pending() is None
        assert InputConfig.load(paths.input_ini).keyboard("r2") == ["g"]
        assert InputConfig.load(paths.input_ini).controller("r2") == ["r2"]
        page.start_rebind("cross")
        assert page.press_key("Escape") is False and page.pending() is None
        assert InputConfig.load(paths.input_ini).keyboard("cross") == ["space"]
        page.start_rebind("cross")
        app.show("race")                                  # leaving the page cancels too
        assert page.pending() is None
    finally:
        app.destroy()


def test_controls_controller_pick_and_reset(tmp_path, monkeypatch):
    paths = AppPaths.for_root(tmp_path); app = App(paths, fsr=False)
    try:
        app.show("controls"); page = app.pages["controls"]
        page.set_controller("l1", "r1")
        assert InputConfig.load(paths.input_ini).controller("l1") == ["r1"]
        asked = []
        monkeypatch.setattr("tkinter.messagebox.askyesno",
                            lambda *a, **k: asked.append(a) or False)
        assert page.reset_defaults() is False
        assert InputConfig.load(paths.input_ini).controller("l1") == ["r1"]
        monkeypatch.setattr("tkinter.messagebox.askyesno", lambda *a, **k: True)
        assert page.reset_defaults() is True and asked
        assert paths.input_ini.read_bytes() == DEFAULT_INPUT.read_bytes()
        assert page.binding_text("l1", "controller") == "L1"
    finally:
        app.destroy()


# ---------------------------------------------------------------- GARAGE (Task 10)

def _garage(tmp_path):
    paths = AppPaths.for_root(tmp_path / "port")
    app = App(paths, fsr=False)
    app.show("garage")
    return paths, app, app.pages["garage"]


def test_garage_backup_and_clear_cache(tmp_path):
    paths, app, page = _garage(tmp_path)
    try:
        page.backup_now()
        pump(app, lambda: not app.saves_busy)
        assert page.status_text() == "No saves to back up"
        save = paths.user / "savedata" / "1" / "slot.dat"
        save.parent.mkdir(parents=True); save.write_bytes(b"save")
        page.backup_now()
        pump(app, lambda: not app.saves_busy)
        zips = list((paths.backups / "Default").glob("saves-*.zip"))
        assert len(zips) == 1
        assert page.status_text() == f"Saves backed up to backups\\Default\\{zips[0].name}"
        with zipfile.ZipFile(zips[0]) as z:
            assert z.namelist() == ["savedata/1/slot.dat"]
        cache = paths.user / "cache" / "CUSA00003" / "a.spv"
        cache.parent.mkdir(parents=True); cache.write_bytes(b"x" * 10)
        page.clear_shader_cache()
        pump(app, lambda: not page.busy())
        assert not cache.exists() and page.status_text() == "Shader cache cleared (1 file)"
    finally:
        app.destroy()


def test_garage_open_saves_folder(tmp_path, monkeypatch):
    paths, app, page = _garage(tmp_path)
    try:
        opened = []
        monkeypatch.setattr("os.startfile", opened.append, raising=False)
        page.open_saves()
        assert opened == [] and page.status_text() == "No saves yet"
        (paths.user / "savedata").mkdir(parents=True)
        page.open_saves()
        assert opened == [str(paths.user / "savedata")]
    finally:
        app.destroy()


def test_garage_desktop_shortcut_uses_choice_and_desktop(tmp_path, monkeypatch):
    paths, app, page = _garage(tmp_path)
    try:
        calls = []

        def fake(p, direct, desktop):
            calls.append((p, direct, Path(desktop)))
            return Path(desktop) / "DRIVECLUB.lnk"

        monkeypatch.setattr("dcport.garage.create_desktop_shortcut", fake)
        # the Desktop Windows reports (here: moved by OneDrive), not USERPROFILE\Desktop
        monkeypatch.setattr("dcport.garage.desktop_dir", lambda: tmp_path / "OneDrive" / "Desktop")
        page.set_shortcut_direct(True)
        assert load_settings(paths).shortcut_direct is True
        page.create_shortcut()
        pump(app, lambda: not page.busy())
        assert calls == [(paths, True, tmp_path / "OneDrive" / "Desktop")]
        assert page.status_text() == "Desktop shortcut created (starts the game directly)"

        def broken(p, direct, desktop):
            raise RuntimeError("Access denied")

        monkeypatch.setattr("dcport.garage.create_desktop_shortcut", broken)
        page.create_shortcut()
        pump(app, lambda: not page.busy())
        assert page.status_text() == "Could not create the shortcut: Access denied"
    finally:
        app.destroy()


def test_garage_steam_checks_running_first(tmp_path, monkeypatch):
    paths, app, page = _garage(tmp_path)
    try:
        added = []
        monkeypatch.setattr("dcport.garage.steam_running", lambda: True)
        monkeypatch.setattr("dcport.garage.steam_userdata_dirs",
                            lambda root: [tmp_path / "u1", tmp_path / "u2"])
        monkeypatch.setattr("dcport.garage.add_to_steam",
                            lambda p, direct, dirs: added.append((direct, dirs)) or len(dirs))
        page.add_to_steam()
        pump(app, lambda: not page.busy())
        assert added == [] and page.status_text() == "Close Steam first, then try again"
        monkeypatch.setattr("dcport.garage.steam_running", lambda: False)
        page.set_steam_direct(False)
        assert load_settings(paths).steam_direct is False
        page.add_to_steam()
        pump(app, lambda: not page.busy())
        assert added == [(False, [tmp_path / "u1", tmp_path / "u2"])]
        assert page.status_text() == "Added to Steam for 2 user(s). Start Steam to see it."

        def no_steam(p, direct, dirs):
            raise SteamError("Steam is not installed")

        monkeypatch.setattr("dcport.garage.add_to_steam", no_steam)
        page.add_to_steam()
        pump(app, lambda: not page.busy())
        assert page.status_text() == "Steam is not installed"
    finally:
        app.destroy()


# ---------------------------------------------------------------- LOG (Task 10)

def test_read_tail_reads_only_the_end(tmp_path):
    f = tmp_path / "big.log"
    with open(f, "wb") as out:
        for i in range(20000):
            out.write(f"line {i} ".encode() + b"x" * 90 + b"\r\n")
    tail = read_tail(f, 6000)
    lines = tail.splitlines()
    assert len(lines) == 6000 and lines[0].startswith("line 14000 ")
    assert lines[-1].startswith("line 19999 ")
    f.write_bytes(b"one\ntwo\nthree")                     # no newline at the end
    assert read_tail(f, 2) == "two\nthree"
    f.write_bytes(b"caf\xc3\xa9 \xff\n")
    assert read_tail(f, 10).startswith("café ")


def test_log_page_shows_last_log_tail_and_missing(tmp_path):
    paths = AppPaths.for_root(tmp_path); app = App(paths, fsr=False)
    try:
        app.show("log"); page = app.pages["log"]
        assert page.text_content() == "No log yet — press PLAY first."
        paths.log.write_text("".join(f"line {i}\n" for i in range(7000)), encoding="utf-8")
        app.show("race"); app.show("log")
        lines = page.text_content().splitlines()
        assert len(lines) == 6000 and lines[0] == "line 1000" and lines[-1] == "line 6999"
        app.last_log = paths.prepare_log
        paths.data.mkdir(); paths.prepare_log.write_text("prepare says hi\n", encoding="utf-8")
        app.show("race"); app.show("log")
        assert page.text_content() == "prepare says hi"
    finally:
        app.destroy()


def test_log_page_live_only_while_running_and_keeps_scroll(tmp_path):
    paths, _ = configured_port(tmp_path, prepared=True)
    paths.log.write_text("".join(f"old {i}\n" for i in range(300)), encoding="utf-8")
    app = App(paths, fsr=False)
    try:
        app.show("log"); page = app.pages["log"]; app.update()
        assert page.live() is False
        app.runner = FakeRunner([0], block=True)
        app.play()
        assert page.live() is True
        page.text.yview_moveto(0.0); app.update()        # the player scrolled up
        with open(paths.log, "a", encoding="utf-8") as f:
            f.write("new line\n")
        page.refresh(); app.update()
        assert page.text_content().endswith("new line") and page.text.yview()[0] == 0.0
        page.text.yview_moveto(1.0); app.update()        # at the bottom: follows new lines
        with open(paths.log, "a", encoding="utf-8") as f:
            f.write("another\n")
        page.refresh(); app.update()
        assert page.text.yview()[1] == 1.0
        app.stop_game()
        pump(app, lambda: app.phase == "idle")
        assert page.live() is False
    finally:
        app.destroy()


def test_log_copy_for_bug_report(tmp_path, monkeypatch):
    paths = AppPaths.for_root(tmp_path); app = App(paths, fsr=True)
    try:
        clip = []
        monkeypatch.setattr(app, "clipboard_clear", lambda: clip.clear(), raising=False)
        monkeypatch.setattr(app, "clipboard_append", clip.append, raising=False)
        paths.log.write_text("boot\ncrash here\n", encoding="utf-8")
        app.gpu = "Test GPU"; app.settings.fps = 144; app.settings.fullscreen = True
        app.show("log"); page = app.pages["log"]
        page.copy_report()
        text = "".join(clip)
        assert text.startswith(f"DCport {dcport.__version__}\n")
        for part in ("Edition: FSR\n", "GPU: Test GPU\n", "Frame rate: 144 FPS\n",
                     "Window: Fullscreen\n", "boot\ncrash here\n"):
            assert part in text
        assert page.status_text() == "Copied"
        app.fsr = False; app.gpu = None
        assert "Edition: no FSR\n" in page.report_text()
        assert "GPU: unknown\n" in page.report_text()
    finally:
        app.destroy()


def test_log_open_folder(tmp_path, monkeypatch):
    paths = AppPaths.for_root(tmp_path); app = App(paths, fsr=False)
    try:
        opened = []
        monkeypatch.setattr("os.startfile", opened.append, raising=False)
        app.show("log"); app.pages["log"].open_folder()
        assert opened == [str(paths.log.parent)]
    finally:
        app.destroy()


def test_window_icon_for_taskbar(tmp_path):
    app = App(AppPaths.for_root(tmp_path), fsr=False)
    try:
        assert app.icon_images                            # iconphoto(True, ...) for taskbar/Alt-Tab
        assert max(img.width() for img in app.icon_images) >= 256
    finally:
        app.destroy()


def test_window_position_from_environment(tmp_path, monkeypatch):
    monkeypatch.setenv("DCPORT_WINDOW_POS", "+-1840+60")
    app = App(AppPaths.for_root(tmp_path), fsr=False)
    try:
        assert app.wm_geometry().endswith("+-1840+60")  # set before the window first shows
    finally:
        app.destroy()
    monkeypatch.setenv("DCPORT_WINDOW_POS", "somewhere")
    app = App(AppPaths.for_root(tmp_path), fsr=False)
    try:
        assert re.search(r"\+\d+\+\d+$", app.wm_geometry())  # ignored: centred as usual
    finally:
        app.destroy()


def test_every_page_is_laid_out_when_shown(tmp_path):
    app = App(AppPaths.for_root(tmp_path), fsr=True)
    try:
        app.update()
        for tab in ("setup", "controls", "garage", "log", "race"):
            app.show(tab)
            end = time.monotonic() + 0.5
            while time.monotonic() < end:
                app.update(); time.sleep(0.01)
            page = app.pages[tab]
            assert page.winfo_ismapped() and page.winfo_height() > 300, tab
    finally:
        app.destroy()


def test_session_custom_goes_to_setup_custom_field(tmp_path):
    app = App(AppPaths.for_root(tmp_path), fsr=False)
    try:
        app.pages["race"]._custom_fps()
        assert app.current == "setup" and app.pages["setup"].fps_choice.value == "custom"
    finally:
        app.destroy()


def test_controls_keys_pass_through_when_nothing_waits(tmp_path):
    app = App(AppPaths.for_root(tmp_path), fsr=False)
    try:
        app.show("controls"); page = app.pages["controls"]
        key = types.SimpleNamespace(keysym="Tab")
        assert page._on_key(key) is None                  # Tab, Alt+F4 ... work as usual
        page.start_rebind("cross")
        assert page._on_key(key) == "break" and page.binding_text("cross", "keyboard") == "Tab"
    finally:
        app.destroy()


# ---------------------------------------------------------------- fix round 1

def test_failed_prepare_only_reports_and_never_starts_the_game(tmp_path):
    paths, _ = configured_port(tmp_path, prepared=True)
    app = App(paths, fsr=False)
    try:
        app.runner = runner = FakeRunner([2])
        app.show("setup")
        assert app.pages["setup"].prepare_again() is True
        pump(app, lambda: app.phase == "idle")
        assert len(runner.starts) == 1 and runner.starts[0][2] == paths.prepare_log
        assert app.phase == "idle" and app.last_log == paths.prepare_log
        assert app.pages["race"].status_text() == "Preparing game files failed (code 2)"
        assert app.pages["setup"].prepare_enabled() is True
    finally:
        app.destroy()


def test_setup_keeps_half_typed_custom_fps(tmp_path):
    paths = AppPaths.for_root(tmp_path); app = App(paths, fsr=False)
    try:
        app.show("setup"); page = app.pages["setup"]
        page.focus_custom(); page.custom.entry.insert(0, "17"); app.update()
        app.save_settings()                               # refreshes every page
        app.toggle_theme(); app.update()
        assert page.custom.get() == "17" and page.fps_choice.value == "custom"
        page._pick_fps(60); app.update()                  # a preset replaces the typing
        assert page.custom.get() == "" and app.settings.fps == 60
        assert page.fps_choice.value == 60 and load_settings(paths).fps == 60
        page.focus_custom(); page.custom.entry.insert(0, "90"); app.update()
        app.show("race")                                  # leaving the page applies it
        assert app.settings.fps == 90
    finally:
        app.destroy()


def test_setup_checks_prepared_only_on_show_and_after_a_run(tmp_path, monkeypatch):
    paths, _ = configured_port(tmp_path, prepared=True)
    calls = []
    monkeypatch.setattr("dcport.ui.setup.is_prepared",
                        lambda p, gd: calls.append(gd) or True)
    app = App(paths, fsr=False)
    try:
        app.show("setup"); page = app.pages["setup"]
        assert len(calls) == 1
        page.set_fps(60); page.set_fullscreen(True); page.set_fps_counter(False)
        app.toggle_theme()
        assert len(calls) == 1                            # saves don't stat the game files
        app.show("race"); app.show("setup")
        assert len(calls) == 2
        app._set_phase("running"); app._set_phase("idle")
        assert len(calls) == 3                            # a process ended
    finally:
        app.destroy()


def test_garage_cache_size_error_and_choice_save_failure(tmp_path, monkeypatch):
    def unreadable(user):
        raise OSError("denied")

    monkeypatch.setattr("dcport.ui.garage.cache_bytes", unreadable)
    paths, app, page = _garage(tmp_path)
    try:
        pump(app, lambda: page.cache_info.cget("text") != "Cache size: …")
        assert page.cache_info.cget("text") == "Cache size unknown (the folder could not be read)"
        paths.settings_json.mkdir(parents=True)          # dcport-launcher.json can't be written
        page.set_shortcut_direct(True)
        assert page.status_text() == "Could not save this choice (dcport-launcher.json)"
        page._show("shortcut", "", "muted")
        page.set_steam_direct(False)
        assert page.status_text() == "Could not save this choice (dcport-launcher.json)"
    finally:
        app.destroy()


# ---------------------------------------------------------------- profiles and backups (Task 10b)

def _put(folder, files):
    for rel, data in files.items():
        f = folder / rel
        f.parent.mkdir(parents=True, exist_ok=True)
        f.write_bytes(data)


def _tree(folder):
    if not folder.is_dir():
        return {}
    return {p.relative_to(folder).as_posix(): p.read_bytes()
            for p in folder.rglob("*") if p.is_file()}


def _check_labels(app):
    return [c.label for c in app.pages["race"]._checks]


def test_garage_switch_profile_updates_settings_race_and_checks(tmp_path):
    paths, app, page = _garage(tmp_path)
    try:
        _put(paths.user / "savedata", {"1/a": b"A"})
        create_profile(paths, "Sam", "Default")
        app.refresh_checks()
        assert "Saves found" in _check_labels(app)
        assert page.profile_text() == "Default" and page.profile_controls_enabled()
        assert page.switch_profile("Sam") is True
        assert page.profile_controls_enabled() is False          # while the folders move
        pump(app, lambda: not app.saves_busy)
        assert app.settings.profile == "Sam" and load_settings(paths).profile == "Sam"
        assert page.profile_text() == "Sam"
        assert app.pages["race"].session_values()["profile"] == "Sam"
        assert "No saves yet" in _check_labels(app)
        assert page.status_text() == "Now playing as Sam"
        assert _tree(paths.root / "profiles" / "Default" / "savedata") == {"1/a": b"A"}
        race = app.pages["race"]
        assert race.set_profile("Default") is True                # the Session card's menu
        pump(app, lambda: not app.saves_busy)
        assert load_settings(paths).profile == "Default" and page.profile_text() == "Default"
        assert _tree(paths.user / "savedata") == {"1/a": b"A"}
        assert "Saves found" in _check_labels(app)
    finally:
        app.destroy()


def test_profile_controls_disabled_while_the_game_runs(tmp_path):
    paths, _ = configured_port(tmp_path, prepared=True)
    _put(paths.user / "savedata", {"1/a": b"A"})
    create_profile(paths, "Sam", "Default")
    app = App(paths, fsr=False)
    try:
        app.runner = FakeRunner([0], block=True)
        app.play()
        assert app.phase == "running"
        app.show("garage"); page = app.pages["garage"]
        assert page.profile_controls_enabled() is False
        assert page.switch_profile("Sam") is False and page.backup_now() is False
        assert app.pages["race"].set_profile("Sam") is False
        assert load_settings(paths).profile == "Default"
        assert _tree(paths.user / "savedata") == {"1/a": b"A"}
        app.stop_game()
        pump(app, lambda: app.phase == "idle")
        assert page.profile_controls_enabled() is True
    finally:
        app.destroy()


def test_play_waits_for_a_saves_task(tmp_path, monkeypatch):
    paths, _ = configured_port(tmp_path, prepared=True)
    release = threading.Event()
    real = dcport.profiles.switch_profile

    def slow(*args):
        release.wait(5)
        return real(*args)

    monkeypatch.setattr("dcport.profiles.switch_profile", slow)
    app = App(paths, fsr=False)
    try:
        app.runner = runner = FakeRunner([0])
        said = []
        assert app.switch_profile("Sam", lambda text, role: said.append(text)) is True
        assert app.saves_busy and app.switch_profile("Kim", lambda text, role: None) is False
        app.play()
        assert runner.starts == []
        assert "saves" in app.pages["race"].status_text().lower()
        release.set()
        pump(app, lambda: not app.saves_busy)
        assert said == ["Now playing as Sam"] and load_settings(paths).profile == "Sam"
    finally:
        release.set()
        app.destroy()


def test_closing_the_window_waits_for_a_saves_task(tmp_path, monkeypatch):
    paths = AppPaths.for_root(tmp_path / "port")
    release = threading.Event()
    real = dcport.profiles.switch_profile

    def slow(*args):
        release.wait(5)
        return real(*args)

    monkeypatch.setattr("dcport.profiles.switch_profile", slow)
    app = App(paths, fsr=False)
    real_destroy = app.destroy
    closed = []
    app.destroy = lambda: closed.append(True) or real_destroy()
    try:
        app.switch_profile("Sam", lambda text, role: None)
        app._on_close()
        app.update()
        assert not closed and app.winfo_exists()                   # still switching
        assert "saves" in app.pages["race"].status_text()
        release.set()
        pump(app, lambda: closed)
        assert load_settings(paths).profile == "Sam"
        assert _tree(paths.root / "profiles" / "Default") == {}    # the switch finished
    finally:
        release.set()
        if not closed:
            real_destroy()


def test_garage_backup_restore_and_delete_ask_first(tmp_path, monkeypatch):
    paths, app, page = _garage(tmp_path)
    try:
        assert page.backup_items() == [] and page.restore_enabled() is False
        _put(paths.user / "savedata", {"1/a": b"A"})
        page.backup_now()
        pump(app, lambda: not app.saves_busy)
        items = page.backup_items()
        assert len(items) == 1 and items[0][2] == "Backup"
        when = items[0][1]
        _put(paths.user / "savedata", {"1/a": b"B"})
        asked = []
        monkeypatch.setattr("dcport.ui.dialogs.confirm",
                            lambda app_, title, text, ok_text: asked.append(text) or False)
        assert page.restore_selected() is False                    # nothing picked yet
        page.select_backup(0)
        assert page.restore_enabled() is True
        assert page.restore_selected() is False                    # the player said no
        assert when in asked[-1] and _tree(paths.user / "savedata") == {"1/a": b"B"}
        monkeypatch.setattr("dcport.ui.dialogs.confirm",
                            lambda app_, title, text, ok_text: asked.append(text) or True)
        assert page.restore_selected() is True
        pump(app, lambda: not app.saves_busy)
        assert _tree(paths.user / "savedata") == {"1/a": b"A"}
        assert page.status_text().startswith(f"Restored the backup from {when}")
        kinds = [item[2] for item in page.backup_items()]
        assert sorted(kinds) == ["Backup", "Before restore"]
        before = page.backup_items()[kinds.index("Before restore")]
        page.select_backup(kinds.index("Before restore"))
        assert page.delete_selected() is True
        assert before[1] in asked[-1]                              # names the backup's date
        pump(app, lambda: not app.saves_busy)
        assert [item[2] for item in page.backup_items()] == ["Backup"]
        assert len(list((paths.backups / "Default").glob("*.zip"))) == 1
    finally:
        app.destroy()


def test_garage_restore_error_shows_and_changes_nothing(tmp_path, monkeypatch):
    paths, app, page = _garage(tmp_path)
    try:
        _put(paths.user / "savedata", {"1/a": b"A"})
        bad = paths.backups / "Default" / "saves-2026-10-17_09-05.zip"
        bad.parent.mkdir(parents=True)
        with zipfile.ZipFile(bad, "w") as z:
            z.writestr("../evil.txt", b"x")
        page.reload()
        pump(app, lambda: page.backup_items())
        monkeypatch.setattr("dcport.ui.dialogs.confirm", lambda *a: True)
        page.select_backup(0)
        assert page.restore_selected() is True
        pump(app, lambda: not app.saves_busy)
        assert page.status_text().startswith("Could not restore")
        assert _tree(paths.user / "savedata") == {"1/a": b"A"}
        assert not (paths.root / "evil.txt").exists()
        assert not (paths.backups / "Default" / "evil.txt").exists()
    finally:
        app.destroy()


def test_garage_new_rename_and_delete_profiles(tmp_path, monkeypatch):
    paths, app, page = _garage(tmp_path)
    try:
        _put(paths.user / "savedata", {"1/a": b"A"})
        names = iter(["Sam", "Samuel"])
        checks = []

        def ask(app_, title, prompt, initial="", ok_text="OK", check=None):
            checks.append(check)
            return next(names)

        monkeypatch.setattr("dcport.ui.dialogs.ask_name", ask)
        assert page.new_profile() is True                          # creates it and plays it
        pump(app, lambda: not app.saves_busy)
        assert load_settings(paths).profile == "Sam" and page.profile_names() == ["Sam", "Default"]
        assert checks[0]("default") and checks[0]("CON") and checks[0]("Kim") is None
        assert page.rename_profile() is True                       # the active one
        pump(app, lambda: not app.saves_busy)
        assert app.settings.profile == "Samuel" and load_settings(paths).profile == "Samuel"
        assert app.pages["race"].session_values()["profile"] == "Samuel"
        monkeypatch.setattr("dcport.ui.dialogs.confirm", lambda *a: False)
        assert page.delete_profile("Default") is False
        monkeypatch.setattr("dcport.ui.dialogs.confirm", lambda *a: True)
        assert page.delete_profile("Samuel") is False              # never the active one
        assert page.delete_profile("Default") is True
        pump(app, lambda: not app.saves_busy)
        assert page.profile_names() == ["Samuel"]
        zips = list((paths.backups / "Default").glob("deleted-*.zip"))
        assert len(zips) == 1 and "deleted-" in page.status_text()
        with zipfile.ZipFile(zips[0]) as z:
            assert z.namelist() == ["savedata/1/a"]
    finally:
        app.destroy()


def test_name_and_confirm_dialogs(tmp_path):
    from dcport.ui import dialogs
    app = App(AppPaths.for_root(tmp_path), fsr=False)
    try:
        d = dialogs.NameDialog(app, "New profile", "Name",
                               check=lambda n: None if n == "ok" else "bad")
        d.entry.insert(0, " nope ")
        d.ok()
        assert d.result is None and d.error_text() == "bad" and d.winfo_exists()
        d.entry.delete(0, "end"); d.entry.insert(0, "  ok ")
        d.ok()
        assert d.result == "ok"
        c = dialogs.ConfirmDialog(app, "Restore backup", "Restore it?", "Restore")
        c.ok()
        assert c.result is True
        c = dialogs.ConfirmDialog(app, "Delete backup", "Delete it?", "Delete")
        c.cancel()
        assert c.result is False
    finally:
        app.destroy()


def test_garage_never_cuts_off_its_cards(tmp_path):
    paths, app, page = _garage(tmp_path)
    try:
        for size in ("900x600", "1100x700"):
            app.geometry(size)
            end = time.monotonic() + 0.6
            while time.monotonic() < end:
                app.update(); time.sleep(0.01)
            board_h, view_h = page.board.winfo_height(), page.view.winfo_height()
            assert board_h <= view_h or page.view.yview()[1] < 1.0, size
    finally:
        app.destroy()


def test_long_backup_list_does_not_loop_while_the_page_is_hidden(tmp_path):
    paths = AppPaths.for_root(tmp_path / "port")
    folder = paths.backups / "Default"
    for i in range(8):
        with zipfile.ZipFile(folder / f"saves-2026-10-0{i + 1}_10-00.zip"
                             if folder.mkdir(parents=True, exist_ok=True) is None else None,
                             "w") as z:
            z.writestr("savedata/x", b"1")
    app = App(paths, fsr=False)                           # RACE shows; GARAGE is hidden
    try:
        started = time.monotonic()
        app.update()
        assert time.monotonic() - started < 5
        app.show("garage"); app.update()
        assert len(app.pages["garage"].backup_items()) == 8
    finally:
        app.destroy()


def test_startup_recovery_locks_saves_and_reports_on_garage(tmp_path):
    paths = AppPaths.for_root(tmp_path)
    (paths.root / "profiles" / "Default" / "savedata").mkdir(parents=True)
    (paths.root / "profiles" / "Default" / "savedata" / "f").write_bytes(b"1")
    app = _App(paths, fsr=False)
    try:
        assert app.recovering and app.saves_locked()
        pump(app, lambda: not app.recovering)
        assert (paths.user / "savedata" / "f").read_bytes() == b"1"
        assert "Moved your saves back" in app.pages["garage"].status_text()
    finally:
        app.destroy()


def test_setup_threading_switch_shows_the_choice(tmp_path):
    paths, _ = configured_port(tmp_path, prepared=True)
    app = App(paths, fsr=False)
    try:
        app.show("setup"); page = app.pages["setup"]
        assert page.threading_choice.value == "auto"
        for value in ("1", "0", "auto"):
            page.set_threading(value)
            assert page.threading_choice.value == value
            assert read_ini(paths.ini)["threaded_recording"] == value
    finally:
        app.destroy()


# ---------------------------------------------------------------- updates

def test_update_downloads_in_the_background_and_installs_on_restart(tmp_path, monkeypatch):
    from dcport import updater
    from tests.test_updater import RELEASE, make_install, make_zip
    root = tmp_path / "DCport"
    make_install(root)
    zip_path = make_zip(tmp_path / "u.zip", RELEASE)
    rel = updater.Release("0.4.0", "https://dl/DCport-0.4.0-windows-fsr.zip", 1, "", "")
    monkeypatch.setenv("DCPORT_UPDATE_TEST", "1")
    monkeypatch.delenv("DCPORT_NO_UPDATES", raising=False)
    monkeypatch.setattr(updater, "check", lambda current, fsr: rel)
    monkeypatch.setattr(updater, "download", lambda release, folder, fsr: zip_path)
    app = App(AppPaths.for_root(root), fsr=True)
    destroyed = []
    real_destroy = app.destroy
    monkeypatch.setattr(app, "destroy", lambda: destroyed.append(True))
    try:
        pump(app, lambda: app.update_text() != "")
        assert "DCport 0.4.0 is ready" in app.update_text()
        app.apply_update()
        pump(app, lambda: destroyed)
        assert (root / "DriveClub.exe").read_text() == "new launcher"
        assert (root / "dcport.ini").read_text().startswith("upscaler=fsr411")  # the player's
        assert (root / "user" / "savedata" / "PROFILE").read_text() == "my save"
    finally:
        real_destroy()


def test_no_update_check_outside_the_packaged_launcher(tmp_path, monkeypatch):
    from dcport import updater
    monkeypatch.delenv("DCPORT_UPDATE_TEST", raising=False)
    called = []
    monkeypatch.setattr(updater, "check", lambda current, fsr: called.append(1))
    app = App(AppPaths.for_root(tmp_path), fsr=True)
    try:
        app.update()
        assert called == [] and app.update_text() == ""
    finally:
        app.destroy()


def test_check_for_updates_button_reports(tmp_path, monkeypatch):
    from dcport import updater
    from tests.test_updater import RELEASE, make_install, make_zip
    root = tmp_path / "DCport"
    make_install(root)
    monkeypatch.setenv("DCPORT_UPDATE_TEST", "1")
    monkeypatch.setenv("DCPORT_NO_UPDATES", "1")  # no check at start; only the button
    found = []
    monkeypatch.setattr(updater, "check", lambda current, fsr: found[0] if found else None)
    monkeypatch.setattr(updater, "download",
                        lambda release, folder, fsr: make_zip(tmp_path / "u.zip", RELEASE))
    app = App(AppPaths.for_root(root), fsr=True)
    try:
        app.show("setup"); page = app.pages["setup"]
        page.check_updates()
        pump(app, lambda: "up to date" in page.status.cget("text"))
        assert app.update_text() == ""
        found.append(updater.Release("0.9.0", "https://dl/x.zip", 1, "", ""))
        page.check_updates()
        pump(app, lambda: "0.9.0 is ready" in page.status.cget("text"))
        assert "DCport 0.9.0 is ready to install" in app.update_text()

        def fail(current, fsr):
            raise OSError("offline")
        monkeypatch.setattr(updater, "check", fail)
        app.update_ready = None
        page.check_updates()
        pump(app, lambda: "Could not check" in page.status.cget("text"))
    finally:
        app.destroy()
