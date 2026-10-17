# SPDX-License-Identifier: GPL-2.0-or-later
"""DCport first-run setup: prepares DRIVECLUB's game image from the player's own dump.

Usage: dc-prepare <game dir> <data dir> <runtime exe> <patches xml>

The steps of run_driveclub.sh: the plaintext eboot.bin is loaded into a memory image, linked
with the game's bundled modules (libc, Fios2, TBB, the Logitech wheel driver, iksemel), its
imports respelled for the runtime's resolver, and the community patches compiled. Nothing here
is copied from the game into the release: every player builds it from their own files.
Writes <data dir>/boot-linked.bin, content.bin, patches.bin and source.txt (the eboot it was
made from); intermediate files are removed.
"""
import os
import sys
import traceback
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import content_profile  # noqa: E402
import dc_patches  # noqa: E402
import link_modules  # noqa: E402
import prepare  # noqa: E402
import respell  # noqa: E402
import stamp  # noqa: E402

MODULES = ['libc.prx', 'libSceFios2.prx', '../libtbb.prx', '../logiWheel.prx', '../iksemelGNMm.prx']
PATCHES = ['60 FPS with deltatime']
KEEP = {'boot-linked.bin', 'content.bin', 'patches.bin', 'source.txt', 'prepare.log'}


def main():
    if len(sys.argv) != 5:
        print(__doc__)
        return 2
    game, data, runtime, xml = (Path(a) for a in sys.argv[1:5])
    data.mkdir(parents=True, exist_ok=True)
    for name in ('eboot.bin', 'sce_sys/param.sfo', 'sce_module/libc.prx', 'sce_module/libSceFios2.prx'):
        if not (game / name).is_file():
            print(f'Missing {name} in {game}: choose the DRIVECLUB folder (CUSA00003)')
            return 1
    prepare.prepare(game, data)
    link_modules.link(game, data, MODULES)
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
