# SPDX-License-Identifier: GPL-2.0-or-later
"""`--play`: prepare if needed, run the game, report problems; no main window."""

import os
import threading
from collections.abc import Callable
from pathlib import Path

from dcport import profiles
from dcport.game import has_eboot
from dcport.ini import read_ini, update_ini
from dcport.paths import AppPaths
from dcport.prepare_state import is_prepared, prepare_command
from dcport.runtime import game_command, game_env
from dcport.settings import load_settings

TITLE = "DCport"


def _default_run_process(cmd: list[str], env: dict[str, str], log: Path, cwd: Path) -> int:
    from dcport.process import RUNNER
    if not RUNNER.start(cmd, env, log, cwd):
        return 1
    code = RUNNER.wait()
    return 1 if code is None else code


def _default_show_error(msg: str) -> None:
    import tkinter as tk
    from tkinter import messagebox
    root = tk.Tk()
    root.withdraw()
    try:
        messagebox.showerror(TITLE, msg, parent=root)
    finally:
        root.destroy()


def _default_show_progress(work: Callable[[], int]) -> int:
    """Show a small window while work() runs on a thread; Tk stays on this thread."""
    import tkinter as tk
    from tkinter import ttk

    from dcport.ui import theme
    bg, accent = "#101318", "#d81f2a"
    root = tk.Tk()
    root.title(TITLE)
    root.configure(bg=bg)
    root.resizable(False, False)
    try:
        root.iconbitmap(default=str(theme.assets_dir() / "dcport.ico"))
    except tk.TclError:
        pass
    tk.Label(root, text="Preparing DRIVECLUB… this happens once", bg=bg, fg="#e8eaed",
             padx=28, pady=18).pack()
    style = ttk.Style(root)
    style.theme_use("clam")
    style.configure("Play.Horizontal.TProgressbar", troughcolor="#1b2028", background=accent,
                    bordercolor=bg, lightcolor=accent, darkcolor=accent)
    bar = ttk.Progressbar(root, mode="indeterminate", length=320,
                          style="Play.Horizontal.TProgressbar")
    bar.pack(padx=28, pady=(0, 24))
    bar.start(15)
    root.update_idletasks()
    root.geometry(f"+{(root.winfo_screenwidth() - root.winfo_reqwidth()) // 2}"
                  f"+{(root.winfo_screenheight() - root.winfo_reqheight()) // 3}")
    result: list[int] = [1]
    done = threading.Event()

    def target() -> None:
        try:
            result[0] = work()
        finally:
            done.set()

    def poll() -> None:
        if done.is_set():
            root.quit()
        else:
            root.after(100, poll)

    threading.Thread(target=target, daemon=True).start()
    root.after(100, poll)
    root.mainloop()
    root.destroy()
    return result[0]


def _default_fps_counter(paths: AppPaths) -> None:
    try:
        if "show_fps" not in read_ini(paths.ini):
            update_ini(paths.ini, {"show_fps": "1"})
    except (OSError, ValueError):
        pass


def run(paths: AppPaths, fsr: bool, extra: list[str], *, run_process=None,
        show_progress=None, show_error=None) -> int:
    show_error = show_error or _default_show_error
    s = load_settings(paths)
    try:
        profiles.recover(paths, s.profile)
    except Exception as e:  # noqa: BLE001 - tell the player rather than crash silently
        show_error(f"Could not check your saves: {e}")
        return 1

    if not s.game_dir or not has_eboot(Path(s.game_dir)):
        show_error("Open DriveClub.exe once to choose the game folder.")
        return 1
    game_dir = Path(s.game_dir)
    real = run_process is None
    if real:
        def run_process(cmd, env, log):
            return _default_run_process(cmd, env, log, paths.root)

    if not is_prepared(paths, game_dir):
        def prepare() -> int:
            try:
                paths.data.mkdir(parents=True, exist_ok=True)
                return run_process(prepare_command(paths, game_dir), dict(os.environ),
                                   paths.prepare_log)
            except OSError:
                return 1
        if show_progress is None and real:
            show_progress = _default_show_progress
        code = show_progress(prepare) if show_progress else prepare()
        if code != 0:
            show_error(f"DRIVECLUB could not be prepared. Details are in: {paths.prepare_log}")
            return 1

    _default_fps_counter(paths)
    from dcport import patches
    error = patches.build_for_session(paths, game_dir, s)
    if error:
        print(error)  # the game still starts, with the patches it had
    cmd = game_command(paths, game_dir, s.fps, extra)
    code = run_process(cmd, game_env(paths, s, fsr, dict(os.environ)), paths.log)
    if code != 0:
        show_error(f"DRIVECLUB stopped (code {code}). Details are in: {paths.log}")
    return code
