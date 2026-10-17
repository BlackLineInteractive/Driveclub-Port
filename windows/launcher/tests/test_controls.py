# SPDX-License-Identifier: GPL-2.0-or-later
from pathlib import Path

from dcport.controls import ACTIONS, InputConfig, tk_key_name

DEFAULT = Path(__file__).parent.parent / "input-default.ini"
DEFAULT_TEXT = DEFAULT.read_text(encoding="utf-8")


def _cfg(tmp_path):
    f = tmp_path / "input.ini"
    f.write_text(DEFAULT_TEXT, encoding="utf-8")
    return f, InputConfig.load(f)


def test_rebind_keyboard_keeps_mouse_and_controller(tmp_path):
    f, cfg = _cfg(tmp_path)
    assert cfg.keyboard("r1") == ["3"] and "r1" in cfg.controller("r1")
    cfg.set_keyboard("r1", "e")
    cfg.save(f)
    text = f.read_text(encoding="utf-8")
    assert "r1 = e" in text and "r1 = leftbutton" in text and "r1 = r1" in text and "r1 = 3" not in text
    assert text.splitlines()[0].startswith("# DCport controls")


def test_tk_key_names():
    assert tk_key_name("space") == "space" and tk_key_name("Shift_L") == "lshift"
    assert tk_key_name("Return") == "enter" and tk_key_name("a") == "a" and tk_key_name("F13") is None


def test_default_file_clean():
    assert "CUSA03173" not in DEFAULT_TEXT and "bb" + "port" not in DEFAULT_TEXT
    assert "walk" not in DEFAULT_TEXT and DEFAULT_TEXT.startswith("# DCport controls")


def test_multiple_keyboard_and_classification(tmp_path):
    f, cfg = _cfg(tmp_path)
    assert cfg.keyboard("r3") == ["q", "c"]
    assert cfg.keyboard("circle") == ["lshift"]
    assert cfg.controller("circle") == ["circle"]
    assert cfg.controller("touchpad_left") == ["back"]
    assert cfg.keyboard("axis_left_x_minus") == ["a"]


def test_untouched_lines_kept(tmp_path):
    f, cfg = _cfg(tmp_path)
    cfg.set_keyboard("r3", "x")
    cfg.save(f)
    text = f.read_text(encoding="utf-8")
    assert "mouse_movement_params = 0.5, 1, 0.125" in text
    assert "hotkey_reload_inputs = f8" in text and "axis_left_x = axis_left_x" in text
    assert text.count("r3 = ") == 2  # one keyboard, one controller
    assert "r3 = x" in text and "r3 = q" not in text and "r3 = c" not in text


def test_set_controller_replaces(tmp_path):
    f, cfg = _cfg(tmp_path)
    cfg.set_controller("l1", "r1")
    cfg.save(f)
    c2 = InputConfig.load(f)
    assert c2.controller("l1") == ["r1"] and c2.keyboard("l1") == ["1"]


def test_insert_after_last_of_kind(tmp_path):
    f = tmp_path / "input.ini"
    f.write_text("# k\ncross = space\ncircle = lshift\n\n# c\ncross = cross\n", encoding="utf-8")
    cfg = InputConfig.load(f)
    cfg.set_keyboard("triangle", "v")
    cfg.set_controller("triangle", "triangle")
    cfg.save(f)
    assert f.read_text(encoding="utf-8").splitlines() == [
        "# k", "cross = space", "circle = lshift", "triangle = v", "",
        "# c", "cross = cross", "triangle = triangle"]


def test_append_when_no_kind(tmp_path):
    f = tmp_path / "input.ini"
    f.write_text("# only\n", encoding="utf-8")
    cfg = InputConfig.load(f)
    cfg.set_keyboard("cross", "space")
    cfg.save(f)
    assert f.read_text(encoding="utf-8").splitlines() == ["# only", "cross = space"]


def test_reset_and_bom(tmp_path):
    f = tmp_path / "sub" / "input.ini"
    f.parent.mkdir()
    InputConfig.reset(f, DEFAULT)
    assert f.read_bytes() == DEFAULT.read_bytes()
    f.write_bytes(b"\xef\xbb\xbfcross = x\n")
    assert InputConfig.load(f).keyboard("cross") == ["x"]


def test_actions_labels():
    d = dict(ACTIONS)
    assert len(ACTIONS) == 24 and d["r2"] == "Throttle (R2)" and d["l2"] == "Brake / reverse (L2)"
    assert d["axis_left_x_plus"] == "Steer right" and d["options"] == "Pause menu (OPTIONS)"
