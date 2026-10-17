# SPDX-License-Identifier: GPL-2.0-or-later
import subprocess

_CMD = (
    "Get-CimInstance Win32_VideoController | "
    "Where-Object { $_.Name -and $_.Name -notlike '*Microsoft Basic*' } | "
    "Sort-Object AdapterRAM -Descending | "
    "Select-Object -First 1 -ExpandProperty Name"
)


def gpu_name() -> str | None:
    try:
        r = subprocess.run(
            ["powershell", "-NoProfile", "-NonInteractive", "-WindowStyle", "Hidden",
             "-Command", _CMD],
            capture_output=True, text=True, timeout=10,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        if r.returncode != 0:
            return None
        name = r.stdout.strip()
        return name or None
    except Exception:
        return None
