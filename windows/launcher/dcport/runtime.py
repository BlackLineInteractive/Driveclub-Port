# SPDX-License-Identifier: GPL-2.0-or-later
"""Game command line, environment and frame-rate rules."""

from collections.abc import Mapping, Sequence
from pathlib import Path

from dcport.ini import read_ini
from dcport.paths import AppPaths
from dcport.patches import dmem_mb, window_size
from dcport.settings import LauncherSettings

FPS_PRESETS = (30, 60, 75, 90, 120, 144)
FPS_MIN = 30
FPS_MAX = 240
FPS_TESTED = 120


def parse_fps(text: str) -> int | None:
    text = text.strip()
    if not (text.isascii() and text.isdigit()):
        return None
    value = int(text)
    return value if FPS_MIN <= value <= FPS_MAX else None


def fps_note(fps: int) -> str:
    if fps > FPS_TESTED:
        return "DRIVECLUB was tested up to 120 FPS; above that its timing may suffer."
    return ""


def _ini_value(paths: AppPaths, key: str, default: str) -> str:
    try:
        return read_ini(paths.ini).get(key, default).strip()
    except OSError:
        return default


DEFAULT_PLAYER_NAME = "Player"


def player_name(paths: AppPaths) -> str:
    """dcport.ini player_name (SETUP > Player name): the name the game shows for the player.
    Printable characters only, at most 16 (a PSN online ID's length)."""
    raw = _ini_value(paths, "player_name", DEFAULT_PLAYER_NAME)
    name = "".join(c for c in raw if c.isprintable()).strip()[:16]
    return name or DEFAULT_PLAYER_NAME


# PS4 system language codes (SystemService parameter 1); the game picks its text from it.
# Each region's game has its own languages: other codes show English.
LANGUAGES_EU = (  # CUSA00003
    ("1", "English"), ("2", "Français"), ("4", "Deutsch"), ("5", "Italiano"), ("3", "Español"),
    ("7", "Português"), ("6", "Nederlands"), ("16", "Polski"), ("8", "Русский"),
    ("19", "Türkçe"), ("13", "Svenska"), ("14", "Dansk"), ("15", "Norsk"), ("12", "Suomi"),
)
LANGUAGES_US = (  # CUSA00093
    ("1", "English"), ("2", "Français"), ("17", "Português (Brasil)"),
    ("20", "Español (Latinoamérica)"),
)
LANGUAGES = LANGUAGES_EU + LANGUAGES_US[2:]


def languages_for(title_id: str) -> tuple[tuple[str, str], ...]:
    """The languages of this region's game (CUSA00003 EU, CUSA00093 US); both if unknown."""
    if title_id == "CUSA00003":
        return LANGUAGES_EU
    if title_id == "CUSA00093":
        return LANGUAGES_US
    return LANGUAGES


DEFAULT_LANGUAGE = "1"


def game_language(paths: AppPaths) -> str:
    """dcport.ini language (SETUP > Language): a PS4 system language code, English (US) if
    missing or unknown."""
    value = _ini_value(paths, "language", DEFAULT_LANGUAGE)
    return value if value in dict(LANGUAGES) else DEFAULT_LANGUAGE



def game_command(paths: AppPaths, game_dir: Path, fps: int,
                 extra: Sequence[str] = ()) -> list[str]:
    cmd = [str(paths.runtime), str(paths.data / "boot-linked.bin"),
           "--content-profile", str(paths.data / "content.bin")]
    patches = paths.data / "patches.bin"  # compiled for this session (dcport.patches)
    if patches.exists():
        cmd += ["--patches", str(patches)]
    cmd += ["--app0", str(game_dir), "--user", str(paths.user), "--timeout", "0"]
    cmd += list(extra)
    return cmd


def game_env(paths: AppPaths, s: LauncherSettings, fsr: bool,
             base: Mapping[str, str]) -> dict[str, str]:
    env = dict(base)
    env.update({
        "BB_GPU_USER_DIR": str(paths.user),
        "BB_CONFIG": str(paths.ini),
        "BB_LENIENT_IMPORTS": "1",
        "BB_READBACKS": "0",
        "BB_DRAW_PIPE": "1",
        "BB_PREP_WORKERS": "0",
        "BB_FULLSCREEN": "1" if s.fullscreen else "0",
        "BB_WINDOW_SIZE": "{}x{}".format(*window_size(s.window_size)),
        "BB_DMEM_MB": str(dmem_mb(s)),
        "BB_USER_NAME": player_name(paths),
        "DC_LANGUAGE": game_language(paths),
    })
    # dcport.ini threaded_recording=0/1 (SETUP > Threading); Auto (anything else) lets the
    # runtime decide: off (it can crash mid-race).
    env.pop("BB_THREADED_RECORDING", None)
    threading = _ini_value(paths, "threaded_recording", "auto")
    if threading in ("0", "1"):
        env["BB_THREADED_RECORDING"] = threading
    if s.fps == 30:
        env["BB_VBLANK_HZ"] = "60"
        env["BB_FPS_LIMIT"] = "0"
    else:
        env["BB_VBLANK_HZ"] = str(s.fps)
        env["BB_FPS_LIMIT"] = str(s.fps)
    if fsr:
        env.pop("BB_UPSCALER", None)
        env["BB_DC_UPSCALE"] = "1"
        # The 1080p frame scaled to a bigger window or screen with FSR 1 and RCAS.
        env["BB_FSR1"] = "1"
        env["BB_FSR4_DIR"] = str(paths.fsr4_dir)
        env["BB_FSR411_DIR"] = str(paths.root / "fsr4_411")  # built by SETUP (dcport.fsr411)
    else:
        env.pop("BB_FSR4_DIR", None)
        env.pop("BB_FSR1", None)
        env["BB_UPSCALER"] = "none"
        env["BB_DC_UPSCALE"] = "0"
    return env
