# SPDX-License-Identifier: GPL-2.0-or-later
"""Small dialogs in the launcher's colours: type a name, or confirm a step."""

import tkinter as tk

from dcport.ui import theme


class _Dialog(tk.Toplevel):
    """A modal panel over the launcher: a heading, the content, then Cancel and OK."""

    def __init__(self, app, title: str):
        super().__init__(app)
        self.withdraw()
        self.app = app
        self.result = None
        pal = app.palette
        self.title(title)
        self.transient(app)
        self.resizable(False, False)
        self.configure(bg=pal["panel"])
        self.body = tk.Frame(self, bg=pal["panel"])
        self.body.pack(fill="both", expand=True, padx=app.px(26),
                       pady=(app.px(22), app.px(20)))
        theme.text_label(self.body, app, title, font="card_title").pack(fill="x")
        self.protocol("WM_DELETE_WINDOW", self.cancel)
        self.bind("<Escape>", lambda e: self.cancel())

    def _text(self, text: str, *, font: str = "body", fg: str = "text",
              width: "int | None" = None) -> tk.Label:
        label = theme.text_label(self.body, self.app, text, font=font, fg=fg,
                                 wraplength=width or self.app.px(380))
        label.pack(fill="x", pady=(self.app.px(10), 0))
        return label

    def _buttons(self, ok_text: str) -> None:
        app = self.app
        row = tk.Frame(self.body, bg=app.palette["panel"])
        row.pack(fill="x", pady=(app.px(22), 0))
        self.ok_button = theme.PillButton(row, app, ok_text, self.ok, font=app.fonts["button"],
                                          height=app.px(36), padx=app.px(24), bg_role="panel")
        self.ok_button.pack(side="right")
        theme.PillButton(row, app, "Cancel", self.cancel, kind="secondary",
                         font=app.fonts["button"], height=app.px(36), padx=app.px(20),
                         bg_role="panel").pack(side="right", padx=(0, app.px(10)))

    def _first_focus(self) -> None:
        self.focus_set()

    def show(self):
        """Show it over the launcher and wait for an answer."""
        app = self.app
        self.update_idletasks()
        w, h = self.winfo_reqwidth(), self.winfo_reqheight()
        x = app.winfo_rootx() + (app.winfo_width() - w) // 2
        y = app.winfo_rooty() + max(0, (app.winfo_height() - h) // 3)
        self.geometry(f"+{x}+{y}")  # also left of the main monitor (negative x)
        self.deiconify()
        try:
            theme.style_title_bar(self, app.palette, app.settings.theme == "dark")
        except (OSError, tk.TclError):
            pass
        try:
            self.grab_set()
        except tk.TclError:
            pass
        self._first_focus()
        self.wait_window(self)
        return self.result

    def ok(self) -> None:
        self.result = True
        self.destroy()

    def cancel(self) -> None:
        self.destroy()


class NameDialog(_Dialog):
    """Type a name; check(name) returns why it can't be used, or None."""

    def __init__(self, app, title: str, prompt: str, initial: str = "", ok_text: str = "OK",
                 check=None):
        super().__init__(app, title)
        self._check = check
        self._text(prompt, fg="muted")
        self.field = theme.Field(self.body, app, width=app.px(380))
        self.field.pack(fill="x", pady=(app.px(14), 0))
        self.entry = self.field.entry
        self.entry.insert(0, initial)
        self.entry.select_range(0, "end")
        self.entry.bind("<Return>", lambda e: self.ok())
        self.entry.bind("<Key>", lambda e: self._say(""), add="+")  # not for Return
        self._error = self._text("", font="small", fg="bad")
        self._error.pack_configure(pady=(app.px(6), 0))
        self._buttons(ok_text)

    def _first_focus(self) -> None:
        self.entry.focus_force()

    def _say(self, text: str) -> None:
        self._error.configure(text=text)

    def error_text(self) -> str:
        return self._error.cget("text")

    def ok(self) -> None:
        name = self.entry.get().strip()
        problem = self._check(name) if self._check is not None else (
            None if name else "Type a name")
        if problem:
            self._say(problem)
            return
        self.result = name
        self.destroy()


class ConfirmDialog(_Dialog):
    """A yes/no question; only the button answers yes (Enter does not)."""

    def __init__(self, app, title: str, text: str, ok_text: str):
        super().__init__(app, title)
        self.result = False
        self._text(text)
        self._buttons(ok_text)


def ask_name(app, title: str, prompt: str, initial: str = "", ok_text: str = "OK",
             check=None) -> "str | None":
    return NameDialog(app, title, prompt, initial, ok_text, check).show()


def confirm(app, title: str, text: str, ok_text: str) -> bool:
    return bool(ConfirmDialog(app, title, text, ok_text).show())
