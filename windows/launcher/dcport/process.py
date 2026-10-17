# SPDX-License-Identifier: GPL-2.0-or-later
"""Single shared child-process runner for the prepare tool and the game."""

import subprocess
import threading
from pathlib import Path


class GameRunner:
    def __init__(self) -> None:
        self._lock = threading.RLock()
        self._proc: subprocess.Popen | None = None
        self._log = None
        self._code: int | None = None
        self._stopped = False

    def _reap(self) -> None:
        """Caller holds the lock. Collect exit status and close the log."""
        if self._proc is not None and self._proc.poll() is not None:
            self._code = self._proc.returncode
            self._proc = None
            self._close_log()

    def _close_log(self) -> None:
        if self._log is not None:
            try:
                self._log.close()
            finally:
                self._log = None

    def start(self, cmd: list[str], env: dict[str, str], log: Path, cwd: Path) -> bool:
        with self._lock:
            self._reap()
            if self._proc is not None:
                return False
            log = Path(log)
            log.parent.mkdir(parents=True, exist_ok=True)
            log_file = open(log, "wb")
            try:
                self._proc = subprocess.Popen(
                    cmd, env=env, cwd=cwd,
                    stdin=subprocess.DEVNULL, stdout=log_file,
                    stderr=subprocess.STDOUT,
                    creationflags=subprocess.CREATE_NO_WINDOW)
            except BaseException:
                log_file.close()
                raise
            self._log = log_file
            self._code = None
            self._stopped = False
            return True

    def running(self) -> bool:
        with self._lock:
            self._reap()
            return self._proc is not None

    def exit_code(self) -> int | None:
        with self._lock:
            self._reap()
            return self._code

    def was_stopped(self) -> bool:
        with self._lock:
            self._reap()
            return self._stopped

    def stop(self) -> None:
        with self._lock:
            proc = self._proc
            if proc is None:
                return
            if proc.poll() is None:
                self._stopped = True
                proc.terminate()
                try:
                    proc.wait(5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait(5)
            self._reap()

    def wait(self, timeout: float | None = None) -> int | None:
        with self._lock:
            proc = self._proc
        if proc is None:
            return self.exit_code()
        try:
            proc.wait(timeout)
        except subprocess.TimeoutExpired:
            return None
        return self.exit_code()


RUNNER = GameRunner()
