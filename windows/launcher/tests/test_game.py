# SPDX-License-Identifier: GPL-2.0-or-later
from pathlib import Path

import pytest

from dcport.game import art_path, game_info, has_eboot, read_sfo, ready_checks
from dcport.paths import AppPaths
from tests.helpers import fake_game, make_sfo

REAL = Path(r"D:\DriveClub WIndows port\driveclub files\CUSA00003\sce_sys\param.sfo")


def test_read_sfo_values():
    d = read_sfo(make_sfo({"TITLE_ID": "CUSA00003", "APP_VER": "01.28", "ATTRIBUTE": 5}))
    assert d["TITLE_ID"] == "CUSA00003" and d["APP_VER"] == "01.28" and d["ATTRIBUTE"] == 5


def test_read_sfo_bad_magic():
    with pytest.raises(ValueError):
        read_sfo(b"nope" * 8)


@pytest.mark.skipif(not REAL.exists(), reason="real param.sfo not present")
def test_read_real_sfo():
    d = read_sfo(REAL.read_bytes())
    assert d["TITLE_ID"] == "CUSA00003" and d["APP_VER"] == "01.28"


def test_game_info_and_eboot(tmp_path):
    assert game_info(tmp_path) is None and not has_eboot(tmp_path)
    game = fake_game(tmp_path / "g", "CUSA00003", "01.28")
    assert has_eboot(game)
    info = game_info(game)
    assert (info.title_id, info.app_ver, info.title) == ("CUSA00003", "01.28", "DRIVECLUB")
    (game / "sce_sys" / "param.sfo").write_bytes(b"garbage!" * 4)
    assert game_info(game) is None


def test_art_fallbacks(tmp_path):
    (tmp_path / "sce_sys").mkdir()
    assert art_path(tmp_path) is None
    (tmp_path / "sce_sys" / "pic1.png").write_bytes(b"x")
    assert art_path(tmp_path).name == "pic1.png"
    # pic0 is DRIVECLUB's race background; pic1 is only the studio logo
    (tmp_path / "sce_sys" / "pic0.png").write_bytes(b"x")
    assert art_path(tmp_path).name == "pic0.png"


def test_ready_checks_other_version(tmp_path):
    game = fake_game(tmp_path / "Drive Club é", title_id="CUSA00093", app_ver="01.00")
    checks = ready_checks(game, AppPaths.for_root(tmp_path / "port"), gpu=None, prepared=False)
    assert [c.state for c in checks] == ["warn", "warn", "warn", "warn"]
    assert "patches need v01.28" in checks[0].label
    assert checks[0].label == "Game CUSA00093 v01.00: patches need v01.28"
    assert checks[2].label == "Graphics card: checking…"
    assert checks[3].label == "Game files will be prepared on PLAY"


def test_ready_checks_no_eboot(tmp_path):
    checks = ready_checks(tmp_path, AppPaths.for_root(tmp_path / "port"), gpu="RX 6750 XT", prepared=True)
    assert checks[0].state == "bad"
    assert checks[0].label == "Choose the DRIVECLUB folder"


def test_ready_checks_all_ok(tmp_path):
    paths = AppPaths.for_root(tmp_path / "port")
    (paths.user / "savedata" / "slot1").mkdir(parents=True)
    game = fake_game(tmp_path / "g")
    checks = ready_checks(game, paths, gpu="RX 6750 XT", prepared=True)
    assert [c.state for c in checks] == ["ok"] * 4
    assert [c.label for c in checks] == [
        "DRIVECLUB CUSA00003 v01.28", "Saves found", "RX 6750 XT", "Game files prepared"]


def test_other_region_at_0128_is_ready(tmp_path):
    game = fake_game(tmp_path / "g", title_id="CUSA00093", app_ver="01.28")
    checks = ready_checks(game, AppPaths.for_root(tmp_path / "app"), None, False)
    assert (checks[0].label, checks[0].state) == ("DRIVECLUB CUSA00093 v01.28", "ok")


def test_update_only_folder_is_refused(tmp_path):
    from dcport.game import is_update_only, ready_checks
    from dcport.paths import AppPaths
    from tests.helpers import fake_game
    update = fake_game(tmp_path / "CUSA00093-patch", title_id="CUSA00093", base_game=False)
    full = fake_game(tmp_path / "CUSA00093", title_id="CUSA00093")
    assert is_update_only(update) and not is_update_only(full)
    assert not is_update_only(tmp_path / "empty")
    paths = AppPaths.for_root(tmp_path / "port")
    game = ready_checks(update, paths, "GPU", prepared=False)[0]
    assert game.state == "bad" and "Update only" in game.label
    assert ready_checks(full, paths, "GPU", prepared=False)[0].state == "ok"
