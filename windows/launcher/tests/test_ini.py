# SPDX-License-Identifier: GPL-2.0-or-later
from dcport.ini import read_ini, update_ini


def test_update_keeps_other_lines(tmp_path):
    f = tmp_path / "dcport.ini"
    f.write_text("# header\nupscaler=fsr4\nfoo=1\n\n# note\nshow_fps=0\nshow_fps=0\n", encoding="utf-8")
    update_ini(f, {"show_fps": "1", "sharpness": "0.30"})
    assert f.read_text(encoding="utf-8") == (
        "# header\nupscaler=fsr4\nfoo=1\n\n# note\nshow_fps=1\nsharpness=0.30\n")


def test_update_creates_missing_file(tmp_path):
    f = tmp_path / "dcport.ini"
    update_ini(f, {"show_fps": "1"})
    assert read_ini(f) == {"show_fps": "1"}
    assert f.read_text(encoding="utf-8").startswith("# DCport settings")


def test_read_empty_and_missing(tmp_path):
    assert read_ini(tmp_path / "none.ini") == {}
    (tmp_path / "e.ini").write_text("", encoding="utf-8")
    assert read_ini(tmp_path / "e.ini") == {}


def test_read_last_wins_and_strips(tmp_path):
    f = tmp_path / "a.ini"
    f.write_text("# c=1\n  a = 1 \nA=2\na=3\nnoequals\n", encoding="utf-8")
    assert read_ini(f) == {"a": "3", "A": "2"}
