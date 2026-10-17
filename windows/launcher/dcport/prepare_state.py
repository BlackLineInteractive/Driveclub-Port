# SPDX-License-Identifier: GPL-2.0-or-later
"""Whether the data built by dc-prepare is current, and how to run the tool."""
import sys
from pathlib import Path

from dcport.paths import AppPaths

# dev/tests: the repo's prepare/ folder; a PyInstaller build uses --paths prepare instead.
_PREPARE = Path(__file__).resolve().parent.parent / "prepare"
if _PREPARE.is_dir() and str(_PREPARE) not in sys.path:
    sys.path.insert(0, str(_PREPARE))

from stamp import source_stamp  # noqa: E402

__all__ = ["source_stamp", "is_prepared", "prepare_command"]


def is_prepared(paths: AppPaths, game_dir: Path) -> bool:
    data = paths.data
    if not ((data / "boot-linked.bin").is_file() and (data / "content.bin").is_file()):
        return False
    try:
        saved = (data / "source.txt").read_text(encoding="utf-8")
    except OSError:
        return False
    return saved == source_stamp(Path(game_dir) / "eboot.bin", paths.runtime, paths.patches_xml)


def prepare_command(paths: AppPaths, game_dir: Path) -> list[str]:
    return [str(paths.prepare_tool), str(game_dir), str(paths.data), str(paths.runtime),
            str(paths.patches_xml)]
