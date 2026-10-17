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
    if "--setup-fsr411" in argv:
        # DriveClub.exe --setup-fsr411 <amd_fidelityfx_upscaler_dx12.dll>: SETUP's button without
        # the window; progress and the result in fsr411-setup.log next to the launcher.
        from pathlib import Path
        from dcport import fsr411
        dll = argv[argv.index("--setup-fsr411") + 1:][:1]
        with open(paths.root / "fsr411-setup.log", "w", encoding="utf-8") as log:
            def report(text: str) -> None:
                log.write(text + "\n")
                log.flush()
            if not dll:
                report("usage: DriveClub.exe --setup-fsr411 <amd_fidelityfx_upscaler_dx12.dll>")
                return 2
            try:
                fsr411.build(paths, Path(dll[0]), report)
            except fsr411.SetupError as e:
                report(f"FAILED: {e}")
                return 1
            report("OK")
        return 0
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
