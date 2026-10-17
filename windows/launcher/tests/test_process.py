# SPDX-License-Identifier: GPL-2.0-or-later
import os
import sys
import time

from dcport.process import GameRunner


def test_second_start_refused(tmp_path):
    r = GameRunner()
    slow = [sys.executable, "-c", "import time; print('hi', flush=True); time.sleep(2)"]
    assert r.start(slow, dict(os.environ), tmp_path / "log.txt", tmp_path)
    assert not r.start(slow, dict(os.environ), tmp_path / "log.txt", tmp_path)
    for _ in range(100):
        if "hi" in (tmp_path / "log.txt").read_text():
            break
        time.sleep(0.1)
    r.stop()
    assert not r.running()
    assert "hi" in (tmp_path / "log.txt").read_text()


def test_exit_code_and_log(tmp_path):
    r = GameRunner()
    assert r.exit_code() is None
    r.start([sys.executable, "-c", "import sys; print('boom'); sys.exit(3)"], dict(os.environ), tmp_path / "l.txt", tmp_path)
    assert r.wait(10) == 3 and "boom" in (tmp_path / "l.txt").read_text()
    assert not r.running() and r.exit_code() == 3


def _py(code):
    return [sys.executable, "-c", code]


def test_was_stopped_after_stop_then_reset(tmp_path):
    r = GameRunner()
    assert not r.was_stopped()
    assert r.start(_py("import time; time.sleep(30)"), dict(os.environ), tmp_path / "l.txt", tmp_path)
    r.stop()
    assert not r.running() and r.was_stopped()
    assert r.start(_py("pass"), dict(os.environ), tmp_path / "l.txt", tmp_path)
    r.wait(10)
    assert not r.was_stopped()


def test_natural_exit_not_stopped(tmp_path):
    r = GameRunner()
    r.start(_py("pass"), dict(os.environ), tmp_path / "l.txt", tmp_path)
    r.wait(10)
    assert not r.was_stopped()


def test_log_truncated_on_restart(tmp_path):
    r = GameRunner()
    log = tmp_path / "l.txt"
    r.start(_py("print('first')"), dict(os.environ), log, tmp_path)
    r.wait(10)
    r.start(_py("print('second')"), dict(os.environ), log, tmp_path)
    r.wait(10)
    text = log.read_text()
    assert "second" in text and "first" not in text
