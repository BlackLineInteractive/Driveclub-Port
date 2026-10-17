"""Compile Driveclub community patches (shadPS4 ps4_cheats PATCHES/Driveclub.xml) into the
BBPATCH2 file bb-probe applies to the eboot at start.

Usage: dc_patches.py <game dir> <Driveclub.xml> <out patches.bin> [patch name ...]
Patches exist for game version 01.28 only; other versions get an empty patch file.
"""
import struct
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from prepare import sfo  # noqa: E402

EBOOT_BASE = 0x400000
APP_VERSION = '01.28'


def encode(line):
    kind, value = line.get('Type'), line.get('Value')
    if kind == 'bytes':
        return bytes.fromhex(value.replace(' ', ''))
    if kind in ('bytes16', 'bytes32', 'bytes64'):
        return int(value, 0).to_bytes(int(kind[5:]) // 8, 'little')
    if kind == 'float32':
        return struct.pack('<f', float(value))
    if kind == 'float64':
        return struct.pack('<d', float(value))
    raise ValueError(f'unsupported patch type {kind!r}')


def compile_patches(game, xml, out, names):
    version = sfo((Path(game) / 'sce_sys/param.sfo').read_bytes()).get('APP_VER')
    writes = []
    if version != APP_VERSION:
        print(f'Patches: game version {version}, the community patches need {APP_VERSION}; none applied')
        names = []
    found = {}
    for meta in ET.parse(xml).getroot().iter('Metadata'):
        if meta.get('Name') in names and meta.get('AppVer') == APP_VERSION:
            found[meta.get('Name')] = meta
    missing = [n for n in names if n not in found]
    if missing:
        raise SystemExit(f'patches not found: {missing}')
    for name in names:
        for line in found[name].iter('Line'):
            writes.append((int(line.get('Address'), 0) - EBOOT_BASE, encode(line)))
        print(f'Patch: {name}')
    blob = struct.pack('<8sQQ', b'BBPATCH2', EBOOT_BASE, len(writes))
    for offset, data in writes:
        blob += struct.pack('<QQ', offset, len(data)) + data
    Path(out).write_bytes(blob)


if __name__ == '__main__':
    if len(sys.argv) < 4:
        raise SystemExit(__doc__)
    compile_patches(sys.argv[1], sys.argv[2], sys.argv[3], [n for n in sys.argv[4:] if n])
