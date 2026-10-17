# SPDX-License-Identifier: GPL-2.0-or-later
import os

from dcport.prepare_state import is_prepared, prepare_command, source_stamp
from tests.helpers import make_port


def test_prepared_tracks_all_three_inputs(tmp_path):
    paths, game = make_port(tmp_path)
    (paths.data / "source.txt").write_text(
        source_stamp(game / "eboot.bin", paths.runtime, paths.patches_xml), encoding="utf-8")
    assert is_prepared(paths, game)
    os.utime(paths.runtime, (1, 1))
    assert not is_prepared(paths, game)


def test_not_prepared_without_data(tmp_path):
    paths, game = make_port(tmp_path, with_data=False)
    assert not is_prepared(paths, game)


def test_stamp_format_and_missing(tmp_path):
    paths, game = make_port(tmp_path)
    lines = source_stamp(game / "eboot.bin", paths.runtime, tmp_path / "nope").splitlines()
    assert len(lines) == 3 and lines[2] == "-|-"
    assert lines[0].startswith("4|")


def test_prepare_command(tmp_path):
    paths, game = make_port(tmp_path)
    assert prepare_command(paths, game) == [
        str(paths.prepare_tool), str(game), str(paths.data), str(paths.runtime),
        str(paths.patches_xml)]


def _stamped(tmp_path):
    paths, game = make_port(tmp_path)
    (paths.data / "source.txt").write_text(
        source_stamp(game / "eboot.bin", paths.runtime, paths.patches_xml), encoding="utf-8")
    assert is_prepared(paths, game)
    return paths, game


def test_eboot_change_invalidates(tmp_path):
    paths, game = _stamped(tmp_path)
    os.utime(game / "eboot.bin", (1, 1))
    assert not is_prepared(paths, game)


def test_patches_xml_change_invalidates(tmp_path):
    paths, game = _stamped(tmp_path)
    os.utime(paths.patches_xml, (1, 1))
    assert not is_prepared(paths, game)


def test_crlf_source_txt_is_accepted(tmp_path):
    paths, game = make_port(tmp_path)
    stamp = source_stamp(game / "eboot.bin", paths.runtime, paths.patches_xml)
    (paths.data / "source.txt").write_bytes(stamp.replace("\n", "\r\n").encode("utf-8"))
    assert is_prepared(paths, game)
