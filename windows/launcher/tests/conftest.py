# SPDX-License-Identifier: GPL-2.0-or-later
"""Test windows open on a second monitor when there is one (DCPORT_WINDOW_POS)."""

import ctypes
import gc
from ctypes import wintypes

import pytest


class _MonitorInfo(ctypes.Structure):
    _fields_ = [("cbSize", wintypes.DWORD), ("rcMonitor", wintypes.RECT),
                ("rcWork", wintypes.RECT), ("dwFlags", wintypes.DWORD)]


def secondary_window_pos() -> "str | None":
    """A Tk position ("+x+y") inside a non-primary monitor's work area, or None."""
    try:
        user32 = ctypes.windll.user32
        user32.SetProcessDPIAware()  # monitor rectangles in real pixels, as Tk uses them
        found: list[tuple[int, int]] = []
        proc_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HMONITOR, wintypes.HDC,
                                       ctypes.POINTER(wintypes.RECT), wintypes.LPARAM)

        def visit(monitor, hdc, rect, data):
            info = _MonitorInfo()
            info.cbSize = ctypes.sizeof(info)
            if user32.GetMonitorInfoW(monitor, ctypes.byref(info)) and not info.dwFlags & 1:
                found.append((info.rcWork.left, info.rcWork.top))
            return True

        callback = proc_type(visit)
        user32.EnumDisplayMonitors(None, None, callback, 0)
    except Exception:  # not Windows, or the calls failed: windows open where they always do
        return None
    if not found:
        return None
    left, top = found[0]
    return f"+{left + 80}+{top + 60}"


@pytest.fixture(autouse=True)
def _free_tk_objects_on_main_thread():
    """A destroyed App leaves Tk objects (fonts, images) in reference cycles. If a later
    test's worker thread happens to run the garbage collector, their __del__ calls into the
    dead Tcl interpreter from that thread and blocks for good, so collect them here."""
    yield
    gc.collect()


@pytest.fixture(autouse=True, scope="session")
def _windows_on_second_monitor():
    patch = pytest.MonkeyPatch()
    pos = secondary_window_pos()
    if pos is not None:
        patch.setenv("DCPORT_WINDOW_POS", pos)
    yield
    patch.undo()
