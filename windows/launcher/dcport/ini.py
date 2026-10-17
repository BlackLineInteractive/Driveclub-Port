# SPDX-License-Identifier: GPL-2.0-or-later
from pathlib import Path

HEADER = "# DCport settings (in-game menu: Insert / L3+R3)"


def _key(line: str):
    if "=" not in line or line.strip().startswith("#"):
        return None
    return line.split("=", 1)[0].strip()


def _lines(path: Path) -> list[str]:
    try:
        return path.read_text(encoding="utf-8").splitlines()
    except FileNotFoundError:
        return []


def read_ini(path: Path) -> dict[str, str]:
    out: dict[str, str] = {}
    for line in _lines(path):
        k = _key(line)
        if k is not None:
            out[k] = line.split("=", 1)[1].strip()
    return out


def update_ini(path: Path, changes: dict[str, str]) -> None:
    lines = _lines(path)
    if not path.exists():
        lines = [HEADER]
    done: set[str] = set()
    out: list[str] = []
    for line in lines:
        k = _key(line)
        if k in changes:
            if k in done:
                continue
            done.add(k)
            out.append(f"{k}={changes[k]}")
        else:
            out.append(line)
    out.extend(f"{k}={v}" for k, v in changes.items() if k not in done)
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out) + "\n")
