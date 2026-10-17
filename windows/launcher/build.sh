#!/usr/bin/env bash
# Usage: build.sh <fsr|nofsr>   Builds dist/<edition>/DriveClub.exe with PyInstaller.
set -euo pipefail
cd "$(dirname "$0")"
ed=${1:?usage: build.sh fsr|nofsr}
case "$ed" in fsr) val=True ;; nofsr) val=False ;; *) echo "edition must be fsr or nofsr" >&2; exit 2 ;; esac
write_edition() {
    printf '# SPDX-License-Identifier: GPL-2.0-or-later\n"""Build edition flag; the build script overwrites this value."""\nFSR: bool = %s\n' "$1" > dcport/edition.py
}
trap 'write_edition True' EXIT
write_edition "$val"
python -m PyInstaller --noconfirm --onefile --windowed --name DriveClub --icon assets/dcport.ico \
    --paths prepare --add-data "assets;assets" --add-data "input-default.ini;." \
    --distpath "dist/$ed" --workpath "build/$ed" dcport/__main__.py
ls -l "dist/$ed/DriveClub.exe"
