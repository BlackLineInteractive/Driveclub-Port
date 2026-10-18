# SPDX-License-Identifier: GPL-2.0-or-later
"""SSE4a EXTRQ translation for CPUs without it (prepare/extrq.py)."""
import random
import struct

import dcport.prepare_state  # noqa: F401  (puts prepare/ on sys.path)
import extrq

MASK = (1 << 64) - 1


def run_trampoline(body: bytes, x: int) -> int:
    """Runs the psllq/psrlq instructions of a trampoline body on the low qword x."""
    pos = 0
    while pos < len(body):
        assert body[pos] == 0x66
        pos += 1
        if body[pos] & 0xf0 == 0x40:
            pos += 1
        assert body[pos:pos + 2] == b"\x0f\x73"
        modrm, shift = body[pos + 2], body[pos + 3]
        x = (x << shift) & MASK if modrm & 0x38 == 0x30 else x >> shift
        pos += 4
    return x


def test_trampoline_matches_extrq_low_qword():
    rng = random.Random(1)
    for length, index in ((8, 0), (8, 32), (1, 63), (64, 0), (16, 8)):
        body = extrq.trampoline(0, 0xc4, length, index)
        for _ in range(2000):
            x = rng.getrandbits(64)
            assert run_trampoline(body, x) == (x >> index) & ((1 << length) - 1)


def test_register_encoding():
    assert extrq.trampoline(0, 0xc4, 8, 0) == bytes.fromhex("660f73f438660f73d438")
    assert extrq.trampoline(0x41, 0xc1, 8, 32) == bytes.fromhex("66410f73f118" "66410f73d138")


def test_patch_rewrites_sites_into_a_new_segment():
    code = bytearray(b"\x90" * 64)
    code[8:14] = bytes.fromhex("660f78c40800")       # extrq xmm4, 8, 0
    code[20:27] = bytes.fromhex("66410f78c10820")    # extrq xmm9, 8, 32
    image, segments, report = extrq.patch(bytes(code), [(0, 64, 5)], 64, [], active=True)
    assert report["sites"] == 2 and report["by_form"] == {"len 8 idx 0": 1, "len 8 idx 32": 1}
    assert segments[-1] == (0x10000, 0x1000, 5)
    for start, end in ((8, 14), (20, 27)):
        assert image[start] == 0xe9 and image[start + 5:end] == b"\x90" * (end - start - 5)
        target = start + 5 + struct.unpack_from("<i", image, start + 1)[0]
        body = image[target:target + 10 + (end - start - 6) * 2]
        jmp = target + len(body)
        assert image[jmp] == 0xe9
        assert jmp + 5 + struct.unpack_from("<i", image, jmp + 1)[0] == end


def test_inactive_or_absent_leaves_the_image():
    code = bytes.fromhex("660f78c40800") + b"\x90" * 10
    image, segments, report = extrq.patch(code, [(0, 16, 5)], 16, [], active=False)
    assert image == code and segments == [(0, 16, 5)] and report["sites"] == 0
    image, segments, report = extrq.patch(b"\x90" * 16, [(0, 16, 5)], 16, [], active=True)
    assert image == b"\x90" * 16 and len(segments) == 1


def test_cpu_check():
    def cpu(ecx):
        return lambda leaf, sub=0: (0x80000008, 0, 0, 0) if leaf == 0x80000000 else (0, 0, ecx, 0)
    assert extrq.sse4a_missing(cpu(1 << 6)) is False
    assert extrq.sse4a_missing(cpu(0)) is True
