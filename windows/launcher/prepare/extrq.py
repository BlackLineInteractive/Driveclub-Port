"""SSE4a EXTRQ for CPUs without it (Intel): DRIVECLUB's eboot uses `extrq xmm, len, idx`
(AMD only; 316 sites in 1.28, all `len 8, idx 0` or `len 8, idx 32`). On a CPU without SSE4a
the first one ends the game (0xC000001D). Each site becomes a jump to a trampoline in a new
executable segment: psllq xmm, 64-idx-len; psrlq xmm, 64-len; jmp back, which gives EXTRQ's
low qword (its upper qword is undefined by AMD). Found and verified by jony113; the macOS
backend's PatchExtrq does the same at run time.

Runs on CPUs without SSE4a only, so AMD images stay byte-identical; DC_EXTRQ_PATCH=0/1
overrides the CPU check.
"""
import bisect
import os
import re
import struct

from cpu_compat import _cpuid_windows

# 66 [REX.B] 0F 78 /0 ib ib, register form (ModRM C0..C7).
PATTERN = re.compile(rb'\x66([\x40-\x4f]?)\x0f\x78([\xc0-\xc7])(.)(.)', re.DOTALL)
MAX_IMAGE_SIZE = 512 * 1024 * 1024


def sse4a_missing(cpuid=None):
    """True when the CPU lacks SSE4a (CPUID 0x80000001 ECX bit 6), None when unknown."""
    cleanup = None
    if cpuid is None:
        if os.name != 'nt' or struct.calcsize('P') != 8:
            return None
        try:
            found = _cpuid_windows()
        except (OSError, AttributeError, ValueError):
            return None
        if not found:
            return None
        cpuid, cleanup = found
    try:
        if cpuid(0x80000000)[0] < 0x80000001:
            return True
        return not cpuid(0x80000001)[2] >> 6 & 1
    finally:
        if cleanup:
            cleanup()


def enabled():
    override = os.environ.get('DC_EXTRQ_PATCH')
    if override in ('0', '1'):
        return override == '1'
    # Unknown CPU: translate (the trampolines run on every x86-64 CPU).
    missing = sse4a_missing()
    return missing is None or missing


def trampoline(rex, modrm, length, index):
    """psllq xmm, 64-index-length; psrlq xmm, 64-length (both skipped when the shift is 0)."""
    rm = modrm & 7
    prefix = b'\x66' + (bytes([rex & 0x41]) if rex & 1 else b'')
    body = b''
    if 64 - index - length:
        body += prefix + b'\x0f\x73' + bytes([0xf0 | rm, 64 - index - length])
    if 64 - length:
        body += prefix + b'\x0f\x73' + bytes([0xd0 | rm, 64 - length])
    return body


def patch(image, segments, main_size, relocations, *, active=None):
    """A copy of the linked image with every EXTRQ of the eboot translated, the segment list
    with the trampoline segment added, and a report. Unchanged when inactive or none found."""
    is_active = enabled() if active is None else active
    report = dict(enabled=bool(is_active), sites=0, by_form={}, trampoline_bytes=0)
    result, new_segments = bytearray(image), list(segments)
    if not is_active:
        return result, new_segments, report
    code = [(start, size) for start, size, flags in segments
            if flags & 1 and start < main_size]
    targets = sorted(r[0] for r in relocations)
    sites = []
    for start, size in code:
        end = min(start + size, main_size)
        for m in PATTERN.finditer(image, start, end):
            rex, modrm = m.group(1), m.group(2)[0]
            length, index = m.group(3)[0], m.group(4)[0]
            length = length & 63 or 64
            index &= 63
            if index + length > 64:
                raise ValueError(f'EXTRQ at {m.start():#x}: index {index} + length {length} > 64')
            if rex and rex[0] & 0x0e:  # only REX.B may be set for a register-form EXTRQ
                raise ValueError(f'EXTRQ at {m.start():#x}: unexpected REX {rex[0]:#x}')
            i = bisect.bisect_left(targets, m.start() - 7)
            if i < len(targets) and targets[i] < m.end():
                raise ValueError(f'EXTRQ at {m.start():#x} overlaps a relocation')
            sites.append((m.start(), m.end(), rex[0] if rex else 0, modrm, length, index))
    if not sites:
        return result, new_segments, report
    arena = (len(image) + 65535) & ~65535
    cursor = arena
    forms = {}
    for site_start, site_end, rex, modrm, length, index in sites:
        body = trampoline(rex, modrm, length, index)
        back = site_end - (cursor + len(body) + 5)
        go = site_start + 5
        body += b'\xe9' + struct.pack('<i', back)
        jump = b'\xe9' + struct.pack('<i', cursor - go) + b'\x90' * (site_end - site_start - 5)
        if len(result) < cursor + len(body):
            result.extend(bytes(cursor + len(body) - len(result)))
        result[site_start:site_end] = jump
        result[cursor:cursor + len(body)] = body
        cursor = (cursor + len(body) + 15) & ~15
        key = f'len {length} idx {index}'
        forms[key] = forms.get(key, 0) + 1
    padded = (cursor + 4095) & ~4095
    if padded > MAX_IMAGE_SIZE:
        raise ValueError('EXTRQ trampolines exceed the image size limit')
    result.extend(bytes(padded - len(result)))
    new_segments.append((arena, padded - arena, 5))
    report.update(sites=len(sites), by_form=forms, trampoline_bytes=cursor - arena)
    return result, new_segments, report
