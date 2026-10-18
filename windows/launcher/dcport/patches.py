# SPDX-License-Identifier: GPL-2.0-or-later
"""Which of DRIVECLUB's community patches (patches/Driveclub.xml) a session runs with.

The PATCHES page picks the optional ones, SETUP's Render resolution picks a resolution
group (its internal and output patches and the memory patch it needs), and the frame rate
decides on the 60 FPS patch. data/patches.bin is compiled from that list before each start,
so a change applies on the next PLAY without preparing the game again.
"""

from dataclasses import dataclass
from pathlib import Path

from dcport.settings import LauncherSettings

DEFAULT_DMEM_MB = 5056  # the PS4's direct memory, the runtime's default


@dataclass(frozen=True)
class Toggle:
    name: str  # <Metadata Name=...> in Driveclub.xml
    label: str
    note: str


MOTION_BLUR = "Disable motion blur and depth of field"
TOGGLES = (
    Toggle(MOTION_BLUR, "No motion blur or depth of field",
           "A sharper picture in motion. The game uses one shader for both, and for its "
           "anti-aliasing: with this on, FSR cannot run."),
    Toggle("Barrier Disable", "No track barriers",
           "Drive off the track without the invisible walls."),
    Toggle("Reset Disable", "No reset to track",
           "The car is not put back on the track when you leave it."),
    Toggle("Disable Photomode Free Camera Collision", "Photo mode: free camera",
           "The camera passes through cars, so you can look inside them."),
    Toggle("Disable Photomode Processing", "Photo mode: no processing",
           "Photo mode shows the frame as it is, without its filters."),
)
DEFAULT_TOGGLES = ()

# The frame-rate patch above 30 FPS; "fixed" is the engine's own 60 Hz tick, which keeps the
# few features that the deltatime version breaks.
TIMING = {
    "deltatime": "60 FPS with deltatime",
    "fixed": "60 FPS with fixed tickrate",
}
DEFAULT_TIMING = "deltatime"


@dataclass(frozen=True)
class Resolution:
    key: str
    label: str
    size: tuple[int, int]
    patches: tuple[str, ...]
    extra_dmem_mb: int


# The game renders at its own 1920x1080: the community render patches above it leave some of
# its passes at 1080p (a square at x 1920 / y 1080 in the picture) and lost the device in races.
# A bigger window or screen gets the 1080p frame scaled up with FSR 1 instead. An older saved
# render_res (1440p, 4K) falls back to this one.
RESOLUTIONS = (
    Resolution("1080p", "1080p", (1920, 1080), (), 0),
)
DEFAULT_RESOLUTION = "1080p"

# The window in windowed mode; fullscreen always covers the monitor.
WINDOW_SIZES = ((1280, 720), (1600, 900), (1920, 1080), (2560, 1440), (3840, 2160))
DEFAULT_WINDOW = "1920x1080"


def resolution(key: str) -> Resolution:
    for r in RESOLUTIONS:
        if r.key == key:
            return r
    return RESOLUTIONS[0]


def window_size(text: str) -> tuple[int, int]:
    for w, h in WINDOW_SIZES:
        if text == f"{w}x{h}":
            return w, h
    return 1920, 1080


def patch_names(s: LauncherSettings) -> list[str]:
    """The patches of a session, in Driveclub.xml's own names."""
    names: list[str] = []
    if s.fps != 30:  # 30 FPS is the game's original timing
        names.append(TIMING.get(s.patch_timing, TIMING[DEFAULT_TIMING]))
    known = {t.name for t in TOGGLES}
    names += [n for n in s.patches if n in known]
    names += resolution(s.render_res).patches
    return names


def dmem_mb(s: LauncherSettings) -> int:
    return DEFAULT_DMEM_MB + resolution(s.render_res).extra_dmem_mb


def build(game_dir: Path, xml: Path, out: Path, names: list[str]) -> None:
    """Compile data/patches.bin; rewritten only when its bytes change."""
    import dcport.prepare_state  # noqa: F401  (puts the prepare tool on sys.path in dev)
    import dc_patches  # from the bundled prepare tool (launcher-app/prepare)

    tmp = out.with_suffix(".new")
    try:
        dc_patches.compile_patches(game_dir, xml, tmp, names)
    except SystemExit as e:  # its error for a name missing from the xml
        tmp.unlink(missing_ok=True)
        raise ValueError(str(e.code)) from None
    data = tmp.read_bytes()
    if out.exists() and out.read_bytes() == data:
        tmp.unlink()
        return
    tmp.replace(out)


def build_for_session(paths, game_dir: Path, s: LauncherSettings) -> "str | None":
    """data/patches.bin for the settings; returns an error to show, the game still starts
    with the previous patches.bin."""
    try:
        build(game_dir, paths.patches_xml, paths.data / "patches.bin", patch_names(s))
    except (OSError, ValueError, ImportError) as e:
        return f"Could not apply the patches: {e}"
    return None
