# SPDX-License-Identifier: GPL-2.0-or-later
import xml.etree.ElementTree as ET
from pathlib import Path

from dcport import patches
from dcport.settings import LauncherSettings

XML = Path(__file__).resolve().parents[2] / "patches" / "Driveclub.xml"


def xml_names() -> set[str]:
    return {m.get("Name") for m in ET.parse(XML).getroot().iter("Metadata")
            if m.get("AppVer") == "01.28"}


def test_every_name_is_in_the_xml():
    names = xml_names()
    wanted = [t.name for t in patches.TOGGLES] + list(patches.TIMING.values())
    wanted += [p for r in patches.RESOLUTIONS for p in r.patches]
    assert [w for w in wanted if w not in names] == []


def test_default_session_is_the_60_fps_patch():
    assert patches.patch_names(LauncherSettings()) == ["60 FPS with deltatime"]
    assert patches.dmem_mb(LauncherSettings()) == patches.DEFAULT_DMEM_MB


def test_30_fps_has_no_frame_rate_patch_but_keeps_toggles():
    s = LauncherSettings(fps=30, patches=["Barrier Disable"])
    assert patches.patch_names(s) == ["Barrier Disable"]


def test_unknown_names_and_keys_are_ignored():
    s = LauncherSettings(patches=["Barrier Disable", "Run without sysmodules", "bogus"],
                         patch_timing="weird", render_res="16k")
    assert patches.patch_names(s) == ["60 FPS with deltatime", "Barrier Disable"]


def test_an_older_render_resolution_falls_back_to_1080p():
    s = LauncherSettings(patch_timing="fixed", render_res="1440p")
    assert patches.patch_names(s) == ["60 FPS with fixed tickrate"]
    assert patches.dmem_mb(s) == 5056


def test_window_size():
    assert patches.window_size("2560x1440") == (2560, 1440)
    assert patches.window_size("1234x567") == (1920, 1080)
