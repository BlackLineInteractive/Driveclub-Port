"""Respell DriveClub's import names into the spelling bb-probe's host resolver knows.

Import names are NID#library-key#module-key, and the short keys are assigned per
executable. bb-probe matches the spelling of the game it was first built for, so a NID the runtime implements
is renamed to the one spelling bb-probe.exe contains. NIDs with no spelled string get
that game's keys for the same library (learned from the first pass), so the shadPS4
HLE table can still find them. Rewrites boot-linked.bin in place.
"""
import collections
import json
import re
import struct
import sys
from pathlib import Path

NAME = re.compile(rb'[A-Za-z0-9+\-]{11}#[A-Za-z0-9+\-]{1,3}#[A-Za-z0-9+\-]{1,3}')


def names_offset(data):
    magic, size, entry, ns, nr, ni, flags = struct.unpack_from('<8s6Q', data, 0)
    if magic != b'BBPROBE5':
        raise ValueError(f'expected BBPROBE5, got {magic!r}')
    pos = 56 + 8 + 32
    nt, = struct.unpack_from('<Q', data, pos)
    pos += 8 + nt * 56
    nb, = struct.unpack_from('<Q', data, pos)
    pos += 8 + nb * 24 + ns * 24
    return pos, ni


def read_name(data, at):
    return bytes(data[at:at + 128]).split(b'\0', 1)[0].decode()


def write_name(data, at, name):
    data[at:at + 128] = name.encode().ljust(128, b'\0')


def respell(boot, exe):
    data = bytearray(Path(boot).read_bytes())
    known = collections.defaultdict(set)
    for m in NAME.findall(Path(exe).read_bytes()):
        m = m.decode()
        known[m.split('#')[0]].add(m)
    pos, ni = names_offset(data)

    renamed, ambiguous = [], []
    for i in range(ni):
        at = pos + i * 128
        name = read_name(data, at)
        spellings = known.get(name.split('#')[0], set())
        if name in spellings or not spellings:
            continue
        if len(spellings) > 1:
            ambiguous.append(name)
            continue
        new = next(iter(spellings))
        write_name(data, at, new)
        renamed.append((name, new))

    votes = collections.defaultdict(collections.Counter)
    for old, new in renamed:
        votes[old.split('#', 1)[1]][new.split('#', 1)[1]] += 1
    keymap = {k: c.most_common(1)[0][0] for k, c in votes.items()}
    rekeyed = []
    for i in range(ni):
        at = pos + i * 128
        name = read_name(data, at)
        parts = name.split('#')
        if len(parts) == 3 and parts[0] not in known and name.split('#', 1)[1] in keymap:
            new = parts[0] + '#' + keymap[name.split('#', 1)[1]]
            if new != name:
                write_name(data, at, new)
                rekeyed.append((name, new))

    Path(boot).write_bytes(data)
    print(f'Respelled {len(renamed)} imports, re-keyed {len(rekeyed)} by library, {len(ambiguous)} ambiguous')
    return dict(renamed=renamed, rekeyed=rekeyed, ambiguous=ambiguous, keymap=keymap)


if __name__ == '__main__':
    report = respell(sys.argv[1], sys.argv[2])
    Path(sys.argv[1]).with_name('respell.json').write_text(json.dumps(report, indent=1))
