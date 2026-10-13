#!/bin/bash
set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PKG_TOOL="$DIR/tools/pkg_pfs_tool"
PASSCODE="00000000000000000000000000000000"

PKG_FILE="${1:-"$DIR/rom/EP9000-CUSA00003_00-XXXXXXXDRIVECLUB-A0100-V0105.pkg"}"
OUT_DIR="${2:-"$DIR/extracted"}"

if [ ! -f "$PKG_FILE" ]; then
    echo "Помилка: PKG файл не знайдено: $PKG_FILE"
    exit 1
fi

mkdir -p "$OUT_DIR"

echo "=== Розпакування PS4 PKG ==="
echo "Файл: $PKG_FILE"
echo "Папка виводу: $OUT_DIR"
echo ""

# Розпаковуємо inner PFS (усі файли гри: eboot.bin, game*.dat, PRX) та SC entries (param.sfo, icon0.png тощо)
"$PKG_TOOL" --passcode "$PASSCODE" --unpack-sc-entries -u "$PKG_FILE" "$OUT_DIR"

echo ""
echo "=== Розпакування успішно завершено! ==="
