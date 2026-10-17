# SPDX-License-Identifier: GPL-2.0-or-later
from dcport import play
from dcport.settings import LauncherSettings, save_settings
from tests.helpers import make_port


def _setup(tmp_path, with_data=False, game=True):
    paths, g = make_port(tmp_path, with_data=with_data)
    save_settings(paths, LauncherSettings(game_dir=str(g) if game else ""))
    return paths


def test_play_prepares_then_runs_with_extra_args(tmp_path):
    paths, game = make_port(tmp_path, with_data=False)
    save_settings(paths, LauncherSettings(game_dir=str(game)))
    calls = []
    code = play.run(paths, fsr=False, extra=["--foo"], run_process=lambda cmd, env, log: calls.append(cmd) or 0,
                    show_progress=None, show_error=lambda msg: calls.append(("error", msg)))
    assert code == 0 and calls[0][0].endswith("dc-prepare.exe") and calls[1][-1] == "--foo"


def test_play_without_folder_errors(tmp_path):
    paths = _setup(tmp_path, game=False)
    calls, errs = [], []
    code = play.run(paths, False, [], run_process=lambda *a: calls.append(a) or 0,
                    show_error=errs.append)
    assert code == 1 and not calls and "game folder" in errs[0]


def test_play_prepare_failure(tmp_path):
    paths = _setup(tmp_path)
    calls, errs = [], []
    code = play.run(paths, False, [], run_process=lambda c, e, l: calls.append(c) or 3,
                    show_error=errs.append)
    assert code == 1 and len(calls) == 1 and "prepare.log" in errs[0]


def test_play_nonzero_exit(tmp_path):
    paths = _setup(tmp_path, with_data=True)
    from dcport import prepare_state
    (paths.data / "source.txt").write_text(prepare_state.source_stamp(
        tmp_path / "game" / "eboot.bin", paths.runtime, paths.patches_xml), encoding="utf-8")
    errs = []
    code = play.run(paths, False, [], run_process=lambda c, e, l: 7, show_error=errs.append)
    assert code == 7 and "code 7" in errs[0] and "last_run.log" in errs[0]
