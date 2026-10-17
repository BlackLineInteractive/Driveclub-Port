# SPDX-License-Identifier: GPL-2.0-or-later
from pathlib import Path


def test_no_forbidden_names():
    root = Path(__file__).parent.parent / "dcport"
    for f in root.rglob("*.py"):
        text = f.read_text(encoding="utf-8").lower()
        assert "bloodborne" not in text and "bbport" not in text, f
