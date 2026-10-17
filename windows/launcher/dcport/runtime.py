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


def frame_gen_on(paths: AppPaths) -> bool:
    """dcport.ini's frame_gen (the SETUP page or the game's menu writes it)."""
    try:
        return read_ini(paths.ini).get("frame_gen", "0").strip() == "1"
    except OSError:
        return False


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
    })
    if s.fps == 30:
        env["BB_VBLANK_HZ"] = "60"
        env["BB_FPS_LIMIT"] = "0"
    else:
        # With frame generation the game makes half the frames and each gets a generated one.
        game_fps = max(FPS_MIN, s.fps // 2) if fsr and frame_gen_on(paths) else s.fps
        env["BB_VBLANK_HZ"] = str(game_fps)
        env["BB_FPS_LIMIT"] = str(game_fps)
    if fsr:
        env.pop("BB_UPSCALER", None)
        env["BB_DC_UPSCALE"] = "1"
        env["BB_FSR4_DIR"] = str(paths.fsr4_dir)
        env["BB_FSR411_DIR"] = str(paths.root / "fsr4_411")  # built by SETUP (dcport.fsr411)
    else:
        env.pop("BB_FSR4_DIR", None)
        env["BB_UPSCALER"] = "none"
        env["BB_DC_UPSCALE"] = "0"
    return env
