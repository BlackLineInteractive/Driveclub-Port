"""Red-zone sites of the linked image, found on the player's PC (no table ships).

On Windows an exception's frame is written right below rsp, over the System V red zone where
leaf functions keep live locals (PS4 and Linux leave those 128 bytes alone). A guest access
that faults on a page the GPU tracker protected is resumed with those locals overwritten
(DRIVECLUB 01.28: the leaf at 0x64d980 reads GPU memory with [rsp-0x38] live, then loads
NULL from it; found by jony113 on Intel 12th gen and later). A site is a run of a red-zone
leaf's instructions, at least 5 bytes for a jmp, holding its accesses through non-stack
pointers; link_modules.protect_red_zones runs those accesses with rsp 128 bytes lower.
Unlike Bloodborne's table (stores only), loads count too: DRIVECLUB's GPU pages fault on
reads. Nothing inside a site may be a branch target, rip-relative, segment-prefixed or change
rsp; functions with jump tables, calls or community patches are left out.

Adapted from bbport's tools/redzone_sites.py (Supermedo's Windows runtime). Needs capstone
(bundled with dc-prepare).
"""
import re
import struct

import capstone
from capstone import x86

RSP, RBP, RIP = x86.X86_REG_RSP, x86.X86_REG_RBP, x86.X86_REG_RIP
STACK = {RSP, x86.X86_REG_ESP, x86.X86_REG_SP, x86.X86_REG_SPL}
FLOW = {capstone.CS_GRP_JUMP, capstone.CS_GRP_CALL, capstone.CS_GRP_RET, capstone.CS_GRP_INT,
        capstone.CS_GRP_IRET}
NO_ACCESS = ('lea', 'nop', 'prefetch')
EH_FRAME_HDR = 0x6474e550
RED_ZONE_RSP = re.compile(rb'[\x44\x4c\x54\x5c\x64\x6c\x74\x7c]\x24[\x80-\xff]', re.S)  # [rsp-disp8]
FRAME_POINTER = bytes.fromhex('4889e5')  # mov rbp, rsp


def function_ranges(image, ph, base):
    """Function starts of one module (from its .eh_frame_hdr) within its text, plus the end."""
    loads = [p for p in ph if p['type'] == 1]
    text = next((p for p in loads if p['flags'] & 1), None)
    hdr = next((p for p in ph if p['type'] == EH_FRAME_HDR), None)
    if text is None or hdr is None:
        return []
    at = base + hdr['vaddr']
    if image[at + 2] != 0x03 or image[at + 3] != 0x3b:  # udata4 count, sdata4 datarel table
        return []
    count, = struct.unpack_from('<I', image, at + 8)
    start, end = base + text['vaddr'], base + text['vaddr'] + text['filesz']
    starts = sorted({at + struct.unpack_from('<i', image, at + 12 + 8 * i)[0]
                     for i in range(count)})
    return [s for s in starts if start <= s < end] + [end]


def _stack_layout(insns):
    """rsp after the prologue and rbp as a frame pointer, both relative to the entry rsp."""
    rsp, rbp = 0, None
    for i in insns:
        ops = i.operands
        if i.mnemonic == 'push':
            rsp -= 8
        elif (i.mnemonic == 'sub' and ops[0].type == x86.X86_OP_REG and ops[0].reg == RSP
              and ops[1].type == x86.X86_OP_IMM):
            rsp -= ops[1].imm
        elif (i.mnemonic == 'mov' and ops[0].type == x86.X86_OP_REG and ops[0].reg == RBP
              and ops[1].type == x86.X86_OP_REG and ops[1].reg == RSP):
            rbp = rsp
        else:
            break
    return rsp, rbp


def _movable(i):
    _, written = i.regs_access()
    return (not FLOW & set(i.groups) and not STACK & set(written)
            and i.prefix[1] not in (0x64, 0x65)
            and not any(op.type == x86.X86_OP_MEM and op.mem.base == RIP for op in i.operands))


def _analyse(md, image, start, end):
    """(sites, uncovered accesses) of one function; None when it keeps nothing below rsp."""
    insns = []
    for i in md.disasm(bytes(image[start:end]), start):
        if i.group(capstone.CS_GRP_CALL):
            return None  # not a leaf: no red zone
        insns.append(i)
    if not insns:
        return None
    body_rsp, rbp = _stack_layout(insns)
    targets, red_zone, accesses = set(), False, []
    for i in insns:
        if i.group(capstone.CS_GRP_JUMP):
            op = i.operands[0]
            if op.type == x86.X86_OP_IMM:
                targets.add(op.imm)
            elif not (op.type == x86.X86_OP_MEM and op.mem.base == RIP):
                return [], ['jump table']  # targets unknown: left alone
        for op in i.operands:
            if op.type != x86.X86_OP_MEM:
                continue
            if op.mem.base == RSP and op.mem.disp < 0:
                red_zone = True
            if rbp is not None and op.mem.base == RBP and rbp + op.mem.disp < body_rsp:
                red_zone = True
            if (not i.mnemonic.startswith(NO_ACCESS) and op.mem.base not in (RSP, RIP)
                    and op.mem.index not in STACK
                    and not (rbp is not None and op.mem.base == RBP)):
                accesses.append(i)
                break
    if not red_zone or not accesses:
        return None
    # Runs of movable instructions; a branch target starts a new run.
    runs, run = [], []
    for i in insns:
        if not _movable(i) or (i.address in targets and run):
            if run:
                runs.append(run)
            run = [i] if _movable(i) else []
            continue
        run.append(i)
    if run:
        runs.append(run)
    marked_at = {i.address for i in accesses}
    sites = []
    for run in runs:
        marked = [k for k, i in enumerate(run) if i.address in marked_at]
        if not marked:
            continue
        first, last = marked[0], marked[-1]
        while sum(i.size for i in run[first:last + 1]) < 5 and (last + 1 < len(run) or first > 0):
            if last + 1 < len(run):
                last += 1
            else:
                first -= 1
        part = run[first:last + 1]
        if sum(i.size for i in part) >= 5:
            sites.append([part[0].address, b''.join(i.bytes for i in part).hex(),
                          [-i.size if i.address in marked_at else i.size for i in part]])
    covered = {a for s in sites for a in range(s[0], s[0] + len(s[1]) // 2)}
    return sites, [f'{i.address:#x}' for i in accesses if i.address not in covered]


def find_sites(image, units, patched=()):
    """Sites of every unit ((base, program headers) of the eboot and each linked module) in
    the untranslated image; functions holding a community patch address are left out."""
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    patched = sorted(patched)
    sites, functions, skipped, uncovered = [], 0, 0, 0
    for base, ph in units:
        bounds = function_ranges(image, ph, base)
        for start, end in zip(bounds, bounds[1:]):
            code = image[start:end]
            # Cheap filters: a [rsp-disp8] operand or a frame pointer, else no red zone.
            if b'\x24' not in code:
                continue
            result = _analyse(md, image, start, end)
            if result is None:
                continue
            found, missing = result
            if missing == ['jump table'] or any(start <= q < end for q in patched):
                skipped += 1
                continue
            functions += 1
            sites += found
            uncovered += len(missing)
    return sites, dict(functions=functions, skipped=skipped, uncovered=uncovered)
