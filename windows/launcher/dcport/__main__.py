# SPDX-License-Identifier: GPL-2.0-or-later
"""Entry point: the launcher window, or `--play` to start the game without it."""

import sys


def _taskbar_identity() -> None:
    """Group the window under DCport's own taskbar icon (not python.exe's)."""
    try:
        import ctypes
        ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID("DCport.Launcher")
    except (AttributeError, OSError):
        pass


def main(argv: list[str]) -> int:
    from dcport import edition
    from dcport.paths import AppPaths

    _taskbar_identity()  # before any window, in both modes
    paths = AppPaths.current()
    if "--play" in argv:
        from dcport import play
        extra = list(argv[argv.index("--play") + 1:])
        return play.run(paths, edition.FSR, extra)

    from dcport.ui.app import App

    app = App(paths, edition.FSR)
    if app.game_dir() is None:
        app.after(300, app.ask_game_folder)
    app.mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
