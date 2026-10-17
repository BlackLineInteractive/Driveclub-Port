# SPDX-License-Identifier: GPL-2.0-or-later
"""The source stamp shared by dc-prepare (writes it) and the launcher (compares it)."""
from pathlib import Path


def _one(path) -> str:
    try:
        st = Path(path).stat()
    except OSError:
        return "-|-\n"
    return f"{st.st_size}|{int(st.st_mtime)}\n"


def source_stamp(eboot: Path, runtime: Path, patches_xml: Path) -> str:
    """One "size|int mtime" line each for eboot.bin, the runtime exe and the patches xml."""
    return _one(eboot) + _one(runtime) + _one(patches_xml)
