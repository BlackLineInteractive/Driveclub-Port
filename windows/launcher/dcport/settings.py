# SPDX-License-Identifier: GPL-2.0-or-later
import json
import os
import tempfile
from dataclasses import asdict, dataclass, field, fields

from .paths import AppPaths
from .profiles import DEFAULT as DEFAULT_PROFILE
from .profiles import valid_name


@dataclass
class LauncherSettings:
    game_dir: str = ""
    fps: int = 120
    fullscreen: bool = False
    theme: str = "dark"
    shortcut_direct: bool = False
    steam_direct: bool = True
    profile: str = DEFAULT_PROFILE  # whose saves are in user/savedata
    render_res: str = "1080p"  # dcport.patches.RESOLUTIONS
    window_size: str = "1920x1080"  # windowed mode only
    patches: list = field(default_factory=list)  # names of dcport.patches.TOGGLES
    patch_timing: str = "deltatime"  # dcport.patches.TIMING


def _valid(value, default) -> bool:
    if isinstance(default, bool):
        return isinstance(value, bool)
    if isinstance(default, int):
        return isinstance(value, int) and not isinstance(value, bool)
    return isinstance(value, type(default))


def load_settings(paths: AppPaths) -> LauncherSettings:
    s = LauncherSettings()
    try:
        data = json.loads(paths.settings_json.read_text(encoding="utf-8"))
    except FileNotFoundError:
        legacy = paths.root / "game_dir.txt"
        try:
            text = legacy.read_text(encoding="utf-8").strip()
        except OSError:
            text = ""
        if text:
            s.game_dir = str((paths.root / text).resolve())
        return s
    except (OSError, ValueError):
        return s
    if not isinstance(data, dict):
        return s
    for f in fields(LauncherSettings):
        if f.name in data and _valid(data[f.name], getattr(s, f.name)):
            setattr(s, f.name, data[f.name])
    if not 30 <= s.fps <= 240:
        s.fps = LauncherSettings.fps
    s.patches = [n for n in s.patches if isinstance(n, str)]
    if not valid_name(s.profile):
        s.profile = DEFAULT_PROFILE  # a hand-edited name; no folder is moved for it
    return s


def save_settings(paths: AppPaths, s: LauncherSettings) -> None:
    """Written to a temp file first, so a crash never leaves half a file (the active
    profile's name must survive)."""
    target = paths.settings_json
    target.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=target.parent, prefix=".dcport-launcher.", suffix=".tmp")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            f.write(json.dumps(asdict(s), indent=2))
        os.replace(tmp, target)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise
