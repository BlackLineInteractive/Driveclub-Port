# SPDX-License-Identifier: GPL-2.0-or-later
"""CONTROLS page: keyboard and controller bindings, read from and written to input.ini."""

import tkinter as tk
from tkinter import messagebox

from dcport.controls import ACTIONS, CONTROLLER_INPUTS, InputConfig, tk_key_name
from dcport.ui import theme

LABELS = dict(ACTIONS)
GROUPS = (
    ("Buttons", ("cross", "circle", "square", "triangle", "l1", "r1", "l2", "r2", "l3", "r3",
                 "options", "touchpad_left")),
    ("D-pad and sticks", ("pad_up", "pad_down", "pad_left", "pad_right",
                          "axis_left_x_minus", "axis_left_x_plus", "axis_left_y_minus",
                          "axis_left_y_plus", "axis_right_x_minus", "axis_right_x_plus",
                          "axis_right_y_minus", "axis_right_y_plus")),
)
RESERVED = {"f7", "f8"}  # the runtime's hotkeys
RESERVED_NOTE = "F7 and F8 are reserved (mouse toggle / reload controls)"
HINT = "Click a binding to change it. Changes save at once; in game, F8 reloads them."

KEY_NAMES = {
    "space": "Space", "enter": "Enter", "backspace": "Backspace", "tab": "Tab",
    "escape": "Esc", "up": "↑", "down": "↓", "left": "←", "right": "→",
    "lshift": "Left Shift", "rshift": "Right Shift", "lctrl": "Left Ctrl",
    "rctrl": "Right Ctrl", "lalt": "Left Alt", "ralt": "Right Alt", "home": "Home",
    "end": "End", "pageup": "Page Up", "pagedown": "Page Down", "delete": "Delete",
    "insert": "Insert", "capslock": "Caps Lock",
}
BUTTONS = (
    ("cross", "Cross"), ("circle", "Circle"), ("square", "Square"), ("triangle", "Triangle"),
    ("l1", "L1"), ("r1", "R1"), ("l2", "L2"), ("r2", "R2"), ("l3", "L3"), ("r3", "R3"),
    ("options", "OPTIONS"), ("back", "Back / Share"), ("pad_up", "D-pad up"),
    ("pad_down", "D-pad down"), ("pad_left", "D-pad left"), ("pad_right", "D-pad right"),
)
BUTTON_NAMES = dict(BUTTONS)
STICKS = {"axis_left_x": "Left stick", "axis_left_y": "Left stick",
          "axis_right_x": "Right stick", "axis_right_y": "Right stick"}
ARROWS = {("x", "minus"): "←", ("x", "plus"): "→", ("y", "minus"): "↑", ("y", "plus"): "↓"}


def key_label(name: str) -> str:
    return KEY_NAMES.get(name, name.upper())


def _is_axis(output: str) -> bool:
    return output.startswith("axis_")


class ControlsPage(tk.Frame):
    def __init__(self, master, app):
        super().__init__(master)
        self.app = app
        self.cfg = InputConfig([])
        self._pending: "str | None" = None
        self._note_role = "muted"
        self._menu: "tk.Menu | None" = None
        self._menu_var = None
        self._keys: dict[str, theme.Chip] = {}
        self._pads: dict[str, theme.Chip] = {}
        app.paint(self, bg="bg")
        self._build_bottom()
        # the cards reach down to the bottom bar; their rows share the height
        self.board = theme.CardBoard(self, app, columns=2, fill=True, flat=False)
        self.board.pack(fill="both", expand=True)
        for col, (title, outputs) in enumerate(GROUPS):
            self._build_group(self.board.add(0, col), title, outputs)
        self.bind("<KeyPress>", self._on_key)
        app.on_theme(lambda pal: self._note.configure(fg=pal[self._note_role]))

    # ------------------------------------------------------------ building

    def _build_group(self, card, title: str, outputs) -> None:
        app = self.app
        grid = tk.Frame(card)
        app.paint(grid, bg="panel")
        grid.pack(fill="both", expand=True)
        grid.columnconfigure(0, weight=1)
        theme.text_label(grid, app, title, font="card_title").grid(
            row=0, column=0, sticky="sw", pady=(0, app.px(8)))
        for col, text in ((1, "KEYBOARD"), (2, "CONTROLLER")):
            theme.text_label(grid, app, " ".join(text), font="caps", fg="muted").grid(
                row=0, column=col, sticky="s", pady=(0, app.px(10)))
        for i, output in enumerate(outputs, start=1):
            grid.rowconfigure(i, weight=1)
            theme.text_label(grid, app, LABELS[output]).grid(row=i, column=0, sticky="w")
            key = theme.Chip(grid, app, lambda o=output: self._chip_clicked(o),
                             width=app.px(92), height=app.px(24))
            key.grid(row=i, column=1, padx=(app.px(10), 0), pady=app.px(3))
            if _is_axis(output):
                pad = theme.Chip(grid, app, None, width=app.px(108), height=app.px(24))
            else:
                pad = theme.Chip(grid, app, lambda o=output: self._pick_button(o),
                                 width=app.px(108), height=app.px(24))
            pad.grid(row=i, column=2, padx=(app.px(8), 0), pady=app.px(3))
            self._keys[output], self._pads[output] = key, pad

    def _build_bottom(self) -> None:
        app = self.app
        bar = tk.Frame(self)
        app.paint(bar, bg="bg")
        bar.pack(side="bottom", fill="x", padx=app.px(24), pady=(app.px(16), app.px(20)))
        self.reset_button = theme.PillButton(bar, app, "Reset to defaults", self.reset_defaults,
                                             kind="secondary", font=app.fonts["button"],
                                             height=app.px(40), padx=app.px(22))
        self.reset_button.pack(side="right")
        self._note = theme.text_label(bar, app, HINT, fg="muted", bg="bg")
        self._note.pack(side="left", fill="x", expand=True)

    # ------------------------------------------------------------ showing

    def on_show(self) -> None:
        self._ensure_file()
        self.reload()

    def on_hide(self) -> None:
        self.cancel_rebind()

    def _ensure_file(self) -> None:
        """A missing input.ini is created from the defaults bundled with the launcher."""
        path = self.app.paths.input_ini
        if path.exists():
            return
        default = theme.default_controls()
        if not default.is_file():
            self._say("input-default.ini is missing from DCport; reinstall it", "bad")
            return
        try:
            InputConfig.reset(path, default)
        except OSError as e:
            self._say(f"Could not create input.ini: {e.strerror or e}", "bad")

    def reload(self) -> None:
        try:
            self.cfg = InputConfig.load(self.app.paths.input_ini)
        except (OSError, ValueError) as e:
            self.cfg = InputConfig([])
            self._say(f"Could not read input.ini: {getattr(e, 'strerror', None) or e}", "bad")
        for _, outputs in GROUPS:
            for output in outputs:
                self._show_binding(output)

    def _show_binding(self, output: str) -> None:
        keys = self.cfg.keyboard(output)
        waiting = output == self._pending
        self._keys[output].set(
            "Press a key" if waiting else (" / ".join(key_label(k) for k in keys) or "—"),
            state="waiting" if waiting else "normal")
        if _is_axis(output) and not self.cfg.controller(output):
            self._pads[output].set(self._stick_text(output), state="static")
        else:
            buttons = self.cfg.controller(output)
            self._pads[output].set(" / ".join(BUTTON_NAMES.get(b, b) for b in buttons) or "—",
                                   state="normal")

    def _stick_text(self, output: str) -> str:
        """Which stick drives a direction, from the full-axis line (axis_left_x = ...)."""
        axis, _, direction = output.rpartition("_")
        source = None
        for line in self.cfg.lines:
            text = line.strip()
            if text.startswith("#") or "=" not in text:
                continue
            out, inp = (part.strip() for part in text.split("=", 1))
            if out == axis and inp in CONTROLLER_INPUTS:
                source = inp
        if source is None:
            return "—"
        if source not in STICKS:
            return BUTTON_NAMES.get(source, source)
        return f"{STICKS[source]} {ARROWS[(source[-1], direction)]}"

    def binding_text(self, output: str, kind: str) -> str:
        return (self._keys if kind == "keyboard" else self._pads)[output].text

    def note_text(self) -> str:
        return self._note.cget("text")

    def _say(self, text: str, role: str = "muted") -> None:
        self._note_role = role
        self._note.configure(text=text, fg=self.app.palette[role])

    def _save(self) -> bool:
        try:
            self.cfg.save(self.app.paths.input_ini)
        except OSError as e:
            self._say(f"Could not save input.ini: {e.strerror or e}", "bad")
            return False
        return True

    # ------------------------------------------------------------ keyboard

    def pending(self) -> "str | None":
        return self._pending

    def _chip_clicked(self, output: str) -> None:
        if self._pending == output:
            self.cancel_rebind()
        else:
            self.start_rebind(output)

    def start_rebind(self, output: str) -> None:
        previous, self._pending = self._pending, output
        if previous is not None:
            self._show_binding(previous)
        self._show_binding(output)
        self._say(f"Press a key for {LABELS[output]}  (Esc cancels)", "text")
        self.focus_set()

    def cancel_rebind(self) -> None:
        output, self._pending = self._pending, None
        if output is not None:
            self._show_binding(output)
            self._say(HINT)

    def _on_key(self, event) -> "str | None":
        if self._pending is None:
            return None
        self.press_key(event.keysym)
        return "break"  # the key was for the binding: Tab, Alt and F10 do nothing else

    def press_key(self, keysym: str) -> bool:
        """The key pressed while a binding waits; True when it was saved."""
        output = self._pending
        if output is None:
            return False
        if keysym == "Escape":
            self.cancel_rebind()
            return False
        name = tk_key_name(keysym)
        if name in RESERVED:
            self._say(RESERVED_NOTE, "bad")
            return False
        if name is None:
            self._say(f"{keysym} can't be used; press another key (Esc cancels)", "warn")
            return False
        self.cfg.set_keyboard(output, name)
        self._pending = None
        saved = self._save()
        self.reload()
        if saved:
            others = [LABELS[o] for o, _ in ACTIONS if o != output and name in self.cfg.keyboard(o)]
            if others:
                self._say(f"{LABELS[output]}: {key_label(name)} (also used for "
                          f"{', '.join(others)})", "warn")
            else:
                self._say(f"{LABELS[output]}: {key_label(name)}", "ok")
        return saved

    # ------------------------------------------------------------ controller

    def _pick_button(self, output: str) -> None:
        self.cancel_rebind()
        app, pal = self.app, self.app.palette
        if self._menu is not None:
            self._menu.destroy()
        menu = tk.Menu(self, tearoff=0, bg=pal["panel"], fg=pal["text"],
                       activebackground=pal["accent"], activeforeground=theme.WHITE,
                       selectcolor=pal["text"], font=app.fonts["body"], bd=0,
                       activeborderwidth=app.px(4), relief="flat")
        self._menu = menu
        current = self.cfg.controller(output)
        self._menu_var = tk.StringVar(menu, value=current[0] if current else "")
        for value, label in BUTTONS:
            menu.add_radiobutton(label=label, variable=self._menu_var, value=value,
                                 command=lambda v=value: self.set_controller(output, v))
        chip = self._pads[output]
        menu.update_idletasks()
        try:
            menu.tk_popup(chip.winfo_rootx() + chip.winfo_width() - menu.winfo_reqwidth(),
                          chip.winfo_rooty() + chip.winfo_height() + app.px(4))
        finally:
            menu.grab_release()

    def set_controller(self, output: str, button: str) -> bool:
        self.cfg.set_controller(output, button)
        saved = self._save()
        self.reload()
        if saved:
            self._say(f"{LABELS[output]}: {BUTTON_NAMES.get(button, button)}", "ok")
        return saved

    # ------------------------------------------------------------ defaults

    def reset_defaults(self) -> bool:
        self.cancel_rebind()
        if not messagebox.askyesno(
                "DCport", "Reset all controls to the defaults?\n\n"
                "Your changes in input.ini will be replaced.", parent=self.app, icon="warning"):
            return False
        try:
            InputConfig.reset(self.app.paths.input_ini, theme.default_controls())
        except OSError as e:
            self._say(f"Could not reset the controls: {e.strerror or e}", "bad")
            return False
        self.reload()
        self._say("Controls reset to defaults", "ok")
        return True
