# SPDX-License-Identifier: GPL-2.0-or-later
"""Model for input.ini (shadPS4 syntax: output = input, one input per line)."""
import shutil
from pathlib import Path

ACTIONS: list[tuple[str, str]] = [
    ("cross", "Cross"),
    ("circle", "Circle"),
    ("square", "Square"),
    ("triangle", "Triangle"),
    ("l1", "L1"),
    ("r1", "R1"),
    ("l2", "Brake / reverse (L2)"),
    ("r2", "Throttle (R2)"),
    ("l3", "L3"),
    ("r3", "R3"),
    ("options", "Pause menu (OPTIONS)"),
    ("pad_up", "D-pad up"),
    ("pad_down", "D-pad down"),
    ("pad_left", "D-pad left"),
    ("pad_right", "D-pad right"),
    ("touchpad_left", "Touchpad"),
    ("axis_left_x_minus", "Steer left"),
    ("axis_left_x_plus", "Steer right"),
    ("axis_left_y_minus", "Left stick up"),
    ("axis_left_y_plus", "Left stick down"),
    ("axis_right_x_minus", "Right stick left"),
    ("axis_right_x_plus", "Right stick right"),
    ("axis_right_y_minus", "Right stick up"),
    ("axis_right_y_plus", "Right stick down"),
]

_OUTPUTS = frozenset(o for o, _ in ACTIONS)

CONTROLLER_INPUTS = frozenset({
    "cross", "circle", "square", "triangle", "l1", "r1", "l2", "r2", "l3", "r3",
    "options", "back", "pad_up", "pad_down", "pad_left", "pad_right",
    "axis_left_x", "axis_left_y", "axis_right_x", "axis_right_y",
})

MOUSE_INPUTS = frozenset({
    "leftbutton", "rightbutton", "middlebutton", "sidebuttonback", "sidebuttonforward",
})

_KB, _CTRL, _MOUSE = "keyboard", "controller", "mouse"


def _parse(line: str):
    """(output, input) for a binding of an ACTIONS output, else None."""
    s = line.strip()
    if not s or s.startswith("#") or "=" not in s:
        return None
    out, inp = (p.strip() for p in s.split("=", 1))
    if out not in _OUTPUTS or not inp:
        return None
    return out, inp


def _kind(inp: str) -> str:
    if inp in MOUSE_INPUTS:
        return _MOUSE
    if inp in CONTROLLER_INPUTS:
        return _CTRL
    return _KB


class InputConfig:
    def __init__(self, lines: list[str]):
        self.lines = lines

    @classmethod
    def load(cls, path: Path) -> "InputConfig":
        try:
            text = Path(path).read_text(encoding="utf-8-sig")
        except FileNotFoundError:
            text = ""
        return cls(text.splitlines())

    def _values(self, output: str, kind: str) -> list[str]:
        res = []
        for line in self.lines:
            p = _parse(line)
            if p and p[0] == output and _kind(p[1]) == kind:
                res.append(p[1])
        return res

    def keyboard(self, output: str) -> list[str]:
        return self._values(output, _KB)

    def controller(self, output: str) -> list[str]:
        return self._values(output, _CTRL)

    def _set(self, output: str, value: str, kind: str) -> None:
        new = f"{output} = {value}"
        mine = []
        last_of_kind = -1
        for i, line in enumerate(self.lines):
            p = _parse(line)
            if p and _kind(p[1]) == kind:
                last_of_kind = i
                if p[0] == output:
                    mine.append(i)
        if mine:
            self.lines[mine[0]] = new
            for i in reversed(mine[1:]):
                del self.lines[i]
        elif last_of_kind >= 0:
            self.lines.insert(last_of_kind + 1, new)
        else:
            self.lines.append(new)

    def set_keyboard(self, output: str, key: str) -> None:
        self._set(output, key, _KB)

    def set_controller(self, output: str, button: str) -> None:
        self._set(output, button, _CTRL)

    def save(self, path: Path) -> None:
        path = Path(path)
        path.parent.mkdir(parents=True, exist_ok=True)
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(self.lines) + "\n")

    @staticmethod
    def reset(path: Path, default: Path) -> None:
        path = Path(path)
        path.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(default, path)


_TK_SPECIAL = {
    "space": "space", "Return": "enter", "KP_Enter": "enter", "BackSpace": "backspace",
    "Tab": "tab", "Escape": "escape", "Up": "up", "Down": "down", "Left": "left",
    "Right": "right", "Shift_L": "lshift", "Shift_R": "rshift", "Control_L": "lctrl",
    "Control_R": "rctrl", "Alt_L": "lalt", "Alt_R": "ralt", "Home": "home", "End": "end",
    "Prior": "pageup", "Next": "pagedown", "Delete": "delete", "Insert": "insert",
    "Caps_Lock": "capslock",
}


def tk_key_name(keysym: str) -> str | None:
    """Tk keysym -> input.ini key name, None when unsupported."""
    if keysym in _TK_SPECIAL:
        return _TK_SPECIAL[keysym]
    if len(keysym) == 1 and keysym.isascii() and keysym.isalnum():
        return keysym.lower()
    if keysym.startswith("F") and keysym[1:].isdigit() and 1 <= int(keysym[1:]) <= 12:
        return keysym.lower()
    return None
