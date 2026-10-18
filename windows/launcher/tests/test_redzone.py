# SPDX-License-Identifier: GPL-2.0-or-later
"""Red-zone sites found at prepare time (prepare/redzone.py)."""
import pytest

import dcport.prepare_state  # noqa: F401  (puts prepare/ on sys.path)

capstone = pytest.importorskip("capstone")
import redzone  # noqa: E402

# A leaf that keeps a local below rsp across a load from memory (DRIVECLUB's 0x64d980 shape):
LEAF = bytes.fromhex(
    "48894424f8"   # mov [rsp-8], rax        red-zone local
    "488b07"       # mov rax, [rdi]          load: may fault on a GPU-tracked page
    "4883c001"     # add rax, 1
    "48895c24f0"   # mov [rsp-0x10], rbx
    "488b5c24f8"   # mov rbx, [rsp-8]        the local, read back
    "c3")          # ret


def analyse(code):
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    return redzone._analyse(md, bytearray(code), 0, len(code))


def test_a_red_zone_leafs_load_is_a_site():
    sites, uncovered = analyse(LEAF)
    assert uncovered == []
    [(offset, original, lengths)] = sites
    assert offset == 5 and lengths[0] == -3          # the load, run with rsp 0x80 lower
    assert bytes.fromhex(original).startswith(bytes.fromhex("488b07"))


def test_functions_with_calls_or_no_red_zone_are_left_alone():
    assert analyse(bytes.fromhex("e800000000") + LEAF) is None      # calls: not a leaf
    assert analyse(bytes.fromhex("488b074883c001c3")) is None       # nothing below rsp
