# SPDX-License-Identifier: GPL-2.0-or-later
"""DCport first-run setup: prepares DRIVECLUB's game image from the player's own dump.

Usage: dc-prepare <game dir> <data dir> <runtime exe> <patches xml>

The steps of run_driveclub.sh: the plaintext eboot.bin is loaded into a memory image, linked
with the game's bundled modules (libc, Fios2, TBB, the Logitech wheel driver, iksemel, and
dlcldr when the dump has it), its imports respelled for the runtime's resolver, and the
community patches compiled. Nothing here is copied from the game into the release: every
player builds it from their own files.
Writes <data dir>/boot-linked.bin, content.bin, patches.bin and source.txt (the eboot it was
made from); intermediate files are removed.
"""
import os
import sys
import traceback
import xml.etree.ElementTree as ET
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import content_profile  # noqa: E402
import dc_patches  # noqa: E402
import link_modules  # noqa: E402
import prepare  # noqa: E402
import respell  # noqa: E402
import stamp  # noqa: E402

MODULES = ['libc.prx', 'libSceFios2.prx', '../libtbb.prx', '../logiWheel.prx', '../iksemelGNMm.prx']
# Linked when the dump has them. Dumps with the DLC merged into the game folder ship dlcldr.prx:
# their eboot takes its add-on calls (sceAppContent*) from it and it answers from its own add-on
# table. Unlinked, those calls return 0 without filling their out-parameters, and the game builds
# its add-on list from stack garbage (write to 0x13 at 0x6dc47b).
OPTIONAL_MODULES = ['../dlcldr.prx']
PATCHES = ['60 FPS with deltatime']
KEEP = {'boot-linked.bin', 'content.bin', 'patches.bin', 'source.txt', 'prepare.log'}


def modules(game):
    return MODULES + [m for m in OPTIONAL_MODULES if (Path(game) / 'sce_module' / m).is_file()]


def main():
    if len(sys.argv) != 5:
        print(__doc__)
        return 2
    game, data, runtime, xml = (Path(a) for a in sys.argv[1:5])
    data.mkdir(parents=True, exist_ok=True)
    for name in ('eboot.bin', 'sce_sys/param.sfo', 'sce_module/libc.prx', 'sce_module/libSceFios2.prx'):
        if not (game / name).is_file():
            print(f'Missing {name} in {game}: choose the DRIVECLUB folder')
            return 1
    prepare.prepare(game, data)
    patched = []
    if xml.is_file():  # no red-zone site may hold an address a community patch writes
        patched = [int(line.get('Address'), 16) - dc_patches.EBOOT_BASE
                   for meta in ET.parse(xml).iter('Metadata') if meta.get('AppVer') == '01.28'
                   for line in meta.iter('Line') if line.get('Address')]
    link_modules.link(game, data, modules(game), patched)
    content_profile.prepare(game, data, 'full')
    respell.respell(data / 'boot-linked.bin', runtime)
    if xml.is_file():
        dc_patches.compile_patches(game, xml, data / 'patches.bin', PATCHES)
    for f in data.iterdir():
        if f.is_file() and f.name not in KEEP:
            f.unlink()
    (data / 'source.txt').write_text(stamp.source_stamp(game / 'eboot.bin', runtime, xml), encoding='utf-8')
    print('Game files prepared.')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Exception:  # the launcher shows the log
        traceback.print_exc()
        sys.exit(1)
