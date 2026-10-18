# SPDX-License-Identifier: GPL-2.0-or-later
"""Modules linked into the game image (prepare/dc_prepare.py)."""
import dcport.prepare_state  # noqa: F401  (puts prepare/ on sys.path)
import dc_prepare


def test_modules_without_dlcldr(tmp_path):
    (tmp_path / "sce_module").mkdir()
    assert dc_prepare.modules(tmp_path) == dc_prepare.MODULES


def test_modules_link_dlcldr_when_present(tmp_path):
    (tmp_path / "sce_module").mkdir()
    (tmp_path / "dlcldr.prx").write_bytes(b"x")
    assert dc_prepare.modules(tmp_path) == dc_prepare.MODULES + ["../dlcldr.prx"]
