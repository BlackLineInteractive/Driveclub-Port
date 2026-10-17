# SPDX-License-Identifier: GPL-2.0-or-later
"""GARAGE page: save profiles and backups, shader cache, desktop shortcut and Steam."""

import os
import re
import tkinter as tk
from datetime import datetime
from pathlib import Path

from PIL import ImageTk

from dcport import garage as tools
from dcport import profiles
from dcport.ui import dialogs, theme

STARTS = (("launcher", "Launcher"), ("direct", "Game directly"))
GLYPHS = {"ok": "✓", "warn": "⚠", "bad": "✕", "muted": ""}
ICONS = {"profiles": "", "backups": "", "cache": "", "shortcut": "",
         "steam": ""}
KINDS = {"saves": "Backup", "before-restore": "Before restore", "deleted": "Deleted profile"}
LIST_ROWS = 4  # backups the list shows before it scrolls
_STAMPED = re.compile(r"^(?P<label>.+?)-\d{4}-\d{2}-\d{2}_\d{2}-\d{2}(?:-\d+)?\.zip$",
                      re.IGNORECASE)


def cache_bytes(user: Path) -> int:
    """Size of the shader cache (the folders clear_shader_cache empties)."""
    total = 0
    for sub in ("cache", "shader"):
        base = user / sub
        if not base.is_dir():
            continue
        for p in base.rglob("*"):
            try:
                if p.is_file():
                    total += p.stat().st_size
            except OSError:
                pass
    return total


def size_text(n: int) -> str:
    mb = n / (1024 * 1024)
    if mb >= 1024:
        return f"{mb / 1024:.1f} GB"
    if mb >= 10:
        return f"{mb:.0f} MB"
    if mb >= 0.1:
        return f"{mb:.1f} MB"
    return f"{max(1, round(n / 1024))} KB"


def backup_kind(name: str) -> str:
    """What made a backup zip, from its name (saves-…, before-restore-…, deleted-…)."""
    m = _STAMPED.match(name)
    return KINDS.get(m.group("label").lower() if m else "saves", "Backup")


def when_text(stamp: float) -> str:
    return f"{datetime.fromtimestamp(stamp):%d %b %Y, %H:%M}"


def _reason(error: BaseException) -> str:
    return getattr(error, "strerror", None) or str(error) or type(error).__name__


class BackupList(tk.Frame):
    """The active profile's backups, newest first: date, kind and size. Click one to pick
    it; the list scrolls (wheel or thin bar) when it is longer than the card."""

    def __init__(self, master, app, on_pick):
        super().__init__(master)
        self.app = app
        self._on_pick = on_pick
        self._items: "list[tuple[Path, str, str, str]]" = []
        self._picked: "Path | None" = None
        self._hover: "int | None" = None
        self._row_h = app.px(36)
        self._photos: list = []
        app.paint(self, bg="panel")
        self.canvas = tk.Canvas(self, highlightthickness=0, bd=0, height=LIST_ROWS * self._row_h,
                                width=app.px(200), yscrollincrement=self._row_h)
        self.scroll = theme.ThinScrollbar(self, app, self.canvas.yview)
        self.canvas.configure(yscrollcommand=self._scrolled)
        self.canvas.pack(side="left", fill="both", expand=True)
        self.canvas.bind("<Configure>", lambda e: self.redraw())
        self.canvas.bind("<Motion>", self._motion)
        self.canvas.bind("<Leave>", lambda e: self._set_hover(None))
        self.canvas.bind("<ButtonRelease-1>", self._click)
        self.canvas.bind("<MouseWheel>", self._wheel)
        app.on_theme(self.redraw)

    # ------------------------------------------------------------ content

    def set_items(self, items: "list[tuple[Path, str, str, str]]") -> None:
        self._items = list(items)
        if self._picked not in [item[0] for item in self._items]:
            self._picked = None
        self._hover = None
        self.redraw()

    def items(self) -> "list[tuple[Path, str, str, str]]":
        return list(self._items)

    def pick(self, index: "int | None") -> None:
        self._picked = self._items[index][0] if index is not None else None
        self.redraw()
        self._on_pick()

    def picked(self) -> "tuple[Path, str, str, str] | None":
        return next((item for item in self._items if item[0] == self._picked), None)

    # ------------------------------------------------------------ mouse

    def _index_at(self, y: int) -> "int | None":
        i = int(self.canvas.canvasy(y) // self._row_h)
        return i if 0 <= i < len(self._items) else None

    def _motion(self, event) -> None:
        i = self._index_at(event.y)
        self.canvas.configure(cursor="hand2" if i is not None else "arrow")
        self._set_hover(i)

    def _set_hover(self, i: "int | None") -> None:
        if i != self._hover:
            self._hover = i
            self.redraw()

    def _click(self, event) -> None:
        if 0 <= event.x < self.canvas.winfo_width():
            i = self._index_at(event.y)
            if i is not None:
                self.pick(i)

    def _wheel(self, event):
        first, last = self.canvas.yview()
        if first <= 0.0 and last >= 1.0:
            return None  # nothing to scroll here: the page scrolls instead
        self.canvas.yview_scroll(-1 if event.delta > 0 else 1, "units")
        return "break"

    def _scrolled(self, first, last) -> None:
        # laid over the list's right edge: showing it never changes the list's size (packing
        # it would, and the list would redraw, show it, hide it… for ever)
        self.scroll.set(first, last)
        if float(first) <= 0.0 and float(last) >= 1.0:
            self.scroll.place_forget()
        else:
            self.scroll.place(relx=1.0, x=0, y=0, relheight=1.0, anchor="ne")
            tk.Misc.lift(self.scroll)  # Canvas.lift raises canvas items instead

    # ------------------------------------------------------------ drawing

    def redraw(self, pal: "dict | None" = None) -> None:
        app = self.app
        pal = pal or app.palette
        c, bg = self.canvas, pal["panel"]
        w, h = max(1, c.winfo_width()), max(1, c.winfo_height())
        rh = self._row_h
        c.configure(bg=bg)
        c.delete("all")
        self._photos = []
        if not self._items:
            c.create_text(w / 2, min(h, LIST_ROWS * rh) / 2, anchor="center", justify="center",
                          text="No backups of this profile yet.\nBack up now keeps a dated zip.",
                          fill=pal["muted"], font=app.fonts["body"], width=max(1, w - app.px(24)))
            c.configure(scrollregion=(0, 0, w, h))
            return
        body, bold, small = app.fonts["body"], app.fonts["body_bold"], app.fonts["small"]
        left = app.px(14)
        # the rows stop short of the scrollbar when there is one (it lies over the edge)
        right = w - (app.px(16) if len(self._items) * rh > h else 0)
        kind_x = left + max(bold.measure(item[1]) for item in self._items) + app.px(26)
        line = theme.mix(pal["text"], bg, 0.08)
        for i, (path, when, kind, size) in enumerate(self._items):
            y0, mid = i * rh, i * rh + rh / 2
            picked = path == self._picked
            if picked or i == self._hover:
                fill = theme.mix(pal["accent"], bg, 0.16) if picked else \
                    theme.mix(pal["text"], bg, 0.05)
                photo = ImageTk.PhotoImage(
                    theme.rounded_image(right, rh - app.px(2), app.px(8), fill, bg), master=c)
                self._photos.append(photo)
                c.create_image(0, y0 + app.px(1), anchor="nw", image=photo)
                if picked:
                    c.create_rectangle(app.px(1), y0 + app.px(9), app.px(4), y0 + rh - app.px(9),
                                       fill=pal["accent"], width=0)
            elif i > 0 and i - 1 != self._hover and self._items[i - 1][0] != self._picked:
                c.create_line(left, y0, right - left, y0, fill=line)
            c.create_text(left, mid, text=when, anchor="w", fill=pal["text"],
                          font=bold if picked else body)
            c.create_text(kind_x, mid, text=kind, anchor="w", font=small,
                          fill=pal["accent"] if kind == "Before restore" and not picked
                          else pal["muted"])
            c.create_text(right - left, mid, text=size, anchor="e", font=small,
                          fill=pal["muted"])
        c.configure(scrollregion=(0, 0, w, max(h, len(self._items) * rh)))


class GaragePage(tk.Frame):
    def __init__(self, master, app):
        super().__init__(master)
        self.app = app
        self._busy = False  # a shader cache, shortcut or Steam task
        self._message = ""
        self._results: dict[str, tuple[tk.Label, str, str]] = {}
        self._names: "list[str]" = [app.settings.profile]
        self._menu: "tk.Menu | None" = None
        self._menu_var = None
        app.paint(self, bg="bg")
        # the cards scroll when the window is too small for them
        self.view = tk.Canvas(self, highlightthickness=0, bd=0, yscrollincrement=app.px(24))
        app.paint(self.view, bg="bg")
        self.view.pack(fill="both", expand=True)
        self.board = theme.CardBoard(self.view, app, columns=3, stretch=True)
        self._board_item = self.view.create_window(0, 0, anchor="nw", window=self.board)
        self.scroll = theme.ThinScrollbar(self, app, self.view.yview, bg_role="bg")
        self.view.configure(yscrollcommand=self._scrolled)
        self.view.bind("<Configure>", lambda e: self._fit_board())
        self.board.bind("<Configure>", lambda e: self._fit_board(), add="+")
        app.bind("<MouseWheel>", self._wheel, add="+")

        self._build_profiles(self._card(0, 0, "profiles", "Profiles"))
        self._build_backups(self._card(0, 1, "backups", "Backups", span=2))
        self._build_cache(self._card(1, 0, "cache", "Shader cache"))
        self._build_shortcut(self._card(1, 1, "shortcut", "Desktop shortcut"))
        self._build_steam(self._card(1, 2, "steam", "Steam"))
        self._saves_buttons = (self.new_button, self.rename_button, self.delete_profile_button,
                               self.backup_button)
        app.on_theme(lambda pal: self._paint_results())
        self.refresh_session()
        self.reload()

    # ------------------------------------------------------------ building

    def _card(self, row: int, col: int, key: str, title: str, span: int = 1) -> tk.Frame:
        app = self.app
        card = self.board.add(row, col, span)
        head = tk.Frame(card)
        app.paint(head, bg="panel")
        head.pack(fill="x", pady=(0, app.px(12)))
        glyph = theme.icon(app.fonts, ICONS[key])
        if glyph:
            theme.text_label(head, app, glyph, font="icon", fg="accent").pack(
                side="left", padx=(0, app.px(12)))
        theme.text_label(head, app, title, font="card_title").pack(side="left")
        card.head = head
        # the result of this card's buttons shows on a line right under them
        result = theme.text_label(card, app, font="body_bold")
        card.bind("<Configure>", lambda e: result.configure(wraplength=max(1, e.width)),
                  add="+")
        result.pack(side="bottom", fill="x", pady=(app.px(10), 0))
        self._results[key] = (result, "", "muted")
        return card

    def _text(self, card, text: str, *, font: str = "body", fg: str = "muted") -> tk.Label:
        label = theme.text_label(card, self.app, text, font=font, fg=fg)
        card.bind("<Configure>", lambda e: label.configure(wraplength=max(1, e.width)),
                  add="+")
        label.pack(fill="x")
        return label

    def _actions(self, card) -> theme.Flow:
        """The card's buttons, at its bottom; they wrap onto a second row when narrow."""
        row = theme.Flow(card, gap_x=self.app.px(10), gap_y=self.app.px(10))
        self.app.paint(row, bg="panel")
        row.pack(side="bottom", fill="x", pady=(self.app.px(16), 0))
        return row

    def _pill(self, master, text: str, command) -> theme.PillButton:
        app = self.app
        button = theme.PillButton(master, app, text, command, kind="secondary",
                                  font=app.fonts["button"], height=app.px(34),
                                  padx=app.px(18), bg_role="panel")
        if not isinstance(master, theme.Flow):
            button.pack(side="left", padx=(0, app.px(10)))
        return button

    def _starts(self, card, command) -> theme.Segmented:
        app = self.app
        row = theme.Flow(card, gap_x=app.px(14), gap_y=app.px(8))
        app.paint(row, bg="panel")
        row.pack(side="bottom", fill="x", pady=(app.px(14), 0))
        theme.text_label(row, app, "Starts")
        choice = theme.Segmented(row, app, STARTS, lambda v: command(v == "direct"),
                                 min_label="Game directly", height=app.px(30))
        return choice

    def _build_profiles(self, card) -> None:
        app = self.app
        actions = self._actions(card)
        self.new_button = self._pill(actions, "New…", self.new_profile)
        self.rename_button = self._pill(actions, "Rename…", self.rename_profile)
        self.delete_profile_button = self._pill(actions, "Delete…", self._pick_profile_to_delete)
        self.open_button = self._pill(actions, "Open saves folder", self.open_saves)
        theme.text_label(card, app, "P L A Y I N G   A S", font="caps", fg="muted").pack(fill="x")
        self.profile_box = theme.Dropdown(card, app, self._open_profile_menu)
        self.profile_box.pack(fill="x", pady=(app.px(6), 0))
        text = self._text(card, "Each profile is its own DRIVECLUB career. Switch any time "
                          "the game is closed.")
        text.pack_configure(pady=(app.px(12), 0))

    def _build_backups(self, card) -> None:
        app = self.app
        self.backups_for = theme.text_label(card.head, app, font="small", fg="muted")
        self.backups_for.pack(side="left", padx=(app.px(10), 0), pady=(app.px(3), 0))
        actions = self._actions(card)
        self.backup_button = self._pill(actions, "Back up now", self.backup_now)
        self.restore_button = self._pill(actions, "Restore…", self.restore_selected)
        self.delete_backup_button = self._pill(actions, "Delete…", self.delete_selected)
        self.backup_list = BackupList(card, app, self._update_buttons)
        self.backup_list.pack(fill="both", expand=True)

    def _build_cache(self, card) -> None:
        actions = self._actions(card)
        self._text(card, "Shaders compiled on this PC. Clear them after a graphics driver "
                   "update or when the picture looks wrong; the next race rebuilds them.")
        self.cache_info = self._text(card, "", font="small")
        self.cache_info.pack_configure(pady=(self.app.px(8), 0))
        self.clear_button = self._pill(actions, "Clear shader cache", self.clear_shader_cache)

    def _build_shortcut(self, card) -> None:
        actions = self._actions(card)
        self.shortcut_choice = self._starts(card, self.set_shortcut_direct)
        self._text(card, "A DRIVECLUB icon on your desktop. It can open this launcher or "
                   "start the game straight away.")
        self.shortcut_button = self._pill(actions, "Create desktop shortcut",
                                          self.create_shortcut)

    def _build_steam(self, card) -> None:
        actions = self._actions(card)
        self.steam_choice = self._starts(card, self.set_steam_direct)
        self._text(card, "DRIVECLUB as a non-Steam game, for Big Picture and Steam Input. "
                   "Close Steam first.")
        self.steam_button = self._pill(actions, "Add to Steam", self.add_to_steam)

    # ------------------------------------------------------------ scrolling

    def _fit_board(self) -> None:
        w = self.view.winfo_width()
        if w > 1:
            self.view.itemconfigure(self._board_item, width=w)
        self.view.configure(scrollregion=(0, 0, max(1, w), self.board.winfo_reqheight()))

    def _scrolled(self, first, last) -> None:
        self.scroll.set(first, last)
        if float(first) <= 0.0 and float(last) >= 1.0:
            self.scroll.place_forget()
        else:
            self.scroll.place(relx=1.0, x=-self.app.px(4), y=self.app.px(4), relheight=1.0,
                              height=-self.app.px(8), anchor="ne")
            tk.Misc.lift(self.scroll)  # Canvas.lift raises canvas items instead

    def _wheel(self, event):
        """The mouse wheel anywhere on this page scrolls it (when it doesn't fit)."""
        if self.app.current != "garage" or not str(event.widget).startswith(str(self)):
            return None
        first, last = self.view.yview()
        if first <= 0.0 and last >= 1.0:
            return None
        self.view.yview_scroll(-3 if event.delta > 0 else 3, "units")
        return None

    # ------------------------------------------------------------ showing

    def on_show(self) -> None:
        self.refresh_session()
        self.reload()
        self._measure_cache()

    def refresh_session(self) -> None:
        s = self.app.settings
        self.shortcut_choice.set("direct" if s.shortcut_direct else "launcher")
        self.steam_choice.set("direct" if s.steam_direct else "launcher")
        self.profile_box.set(s.profile)

    def refresh_profiles(self) -> None:
        """App: the active profile, its backups or the saves lock changed."""
        self.reload()

    def reload(self) -> None:
        """Read the profile names and the active profile's backups (names and sizes only)."""
        app = self.app
        active = app.settings.profile
        self._names = profiles.list_profiles(app.paths, active)
        rows = []
        for path in profiles.list_backups(app.paths, active):
            try:
                st = path.stat()
            except OSError:
                continue
            rows.append((path, when_text(st.st_mtime), backup_kind(path.name),
                         size_text(st.st_size)))
        self.profile_box.set(active)
        count = len(rows)
        self.backups_for.configure(
            text=f"{active}  ·  {count} backup{'' if count == 1 else 's'}" if count else active)
        self.backup_list.set_items(rows)
        self._update_buttons()

    def _measure_cache(self) -> None:
        self.cache_info.configure(text="Cache size: …")
        user = self.app.paths.user
        self.app.run_task(lambda: cache_bytes(user), self._cache_measured)

    def _cache_measured(self, size: "int | None", error) -> None:
        if error is not None or size is None:
            self.cache_info.configure(text="Cache size unknown (the folder could not be read)")
        elif size == 0:
            self.cache_info.configure(text="The cache is empty")
        else:
            self.cache_info.configure(text=f"Cache size: {size_text(size)}")

    def set_phase(self, phase: str) -> None:
        self._update_buttons()

    # ------------------------------------------------------------ results

    def busy(self) -> bool:
        return self._busy

    def status_text(self) -> str:
        """The latest result line (any card)."""
        return self._message

    def _show(self, where: str, text: str, role: str = "ok") -> None:
        label = self._results[where][0]
        self._results[where] = (label, text, role)
        self._message = text
        self._paint_results()

    def _paint_results(self) -> None:
        for label, text, role in self._results.values():
            glyph = GLYPHS.get(role, "")
            label.configure(text=f"{glyph}  {text}" if glyph and text else text,
                            fg=self.app.palette[role])
        self.board.schedule()  # a long result may take a second line

    def profile_controls_enabled(self) -> bool:
        return not self.app.saves_locked()

    def restore_enabled(self) -> bool:
        return self.restore_button.enabled

    def _update_buttons(self) -> None:
        locked = self.app.saves_locked()
        for button in self._saves_buttons:
            usable = not locked
            if button is self.delete_profile_button:
                usable = usable and len(self._names) > 1
            button.set(button.text, enabled=usable)
        picked = self.backup_list.picked() is not None
        for button in (self.restore_button, self.delete_backup_button):
            button.set(button.text, enabled=not locked and picked)
        self.profile_box.set(enabled=not locked)
        for button in (self.clear_button, self.shortcut_button, self.steam_button):
            # the running game keeps its shader cache files open
            usable = not self._busy and not (button is self.clear_button
                                             and self.app.phase == "running")
            button.set(button.text, enabled=usable)

    def _run(self, where: str, doing: str, work, done) -> bool:
        """work() runs on a worker thread; done(result, error) runs here afterwards."""
        if self._busy:
            return False
        self._busy = True
        self._update_buttons()
        self._show(where, doing, "muted")

        def finished(result, error):
            self._busy = False
            self._update_buttons()
            done(result, error)

        self.app.run_task(work, finished)
        return True

    def _run_saves(self, where: str, doing: str, work, done) -> bool:
        """Like _run, for work on the save files (App keeps those one at a time)."""
        if not self.app.run_saves_task(work, done):
            return False
        self._show(where, doing, "muted")
        return True

    def _menu_at(self, widget) -> tk.Menu:
        if self._menu is not None:  # the previous popup; its command has run by now
            self._menu.destroy()
        self._menu = theme.styled_menu(self, self.app)
        return self._menu

    def _popup(self, menu: tk.Menu, widget) -> None:
        menu.update_idletasks()
        try:
            menu.tk_popup(widget.winfo_rootx(),
                          widget.winfo_rooty() + widget.winfo_height() + self.app.px(4))
        finally:
            menu.grab_release()

    # ------------------------------------------------------------ profiles

    def profile_text(self) -> str:
        return self.profile_box.text

    def profile_names(self) -> "list[str]":
        return list(self._names)

    def _name_problem(self, name: str, keep: "str | None" = None) -> "str | None":
        problem = profiles.name_problem(name)
        if problem:
            return problem
        for other in profiles.list_profiles(self.app.paths, self.app.settings.profile):
            if profiles.same(other, name) and not (keep and profiles.same(other, keep)):
                return f"There is already a profile called {other}"
        return None

    def _open_profile_menu(self) -> None:
        if self.app.saves_locked():
            return
        self.reload()
        menu = self._menu_at(self.profile_box)
        active = self.app.settings.profile
        self._menu_var = tk.StringVar(menu, value=active)
        for name in self._names:
            menu.add_radiobutton(label=name, variable=self._menu_var, value=name,
                                 command=lambda v=name: self.switch_profile(v))
        menu.add_separator()
        menu.add_command(label="New profile…", command=self.new_profile)
        self._popup(menu, self.profile_box)

    def switch_profile(self, name: str) -> bool:
        return self.app.switch_profile(name, lambda text, role: self._show("profiles", text, role))

    def new_profile(self) -> bool:
        """Create a profile (a new, empty career) and play with it."""
        app = self.app
        if app.saves_locked():
            return False
        active = app.settings.profile
        name = dialogs.ask_name(
            app, "New profile", f"A new profile starts a new career. The saves of {active} "
            "stay as they are, in their own profile.", ok_text="Create",
            check=self._name_problem)
        if not name:
            return False
        paths = app.paths

        def created(result, error) -> None:
            if error is not None:
                self._show("profiles", f"Could not create {name}: {_reason(error)}", "bad")
                return

            def report(text: str, role: str) -> None:
                self._show("profiles", f"Created {name}. {text}" if role == "ok" else text, role)

            if not app.switch_profile(name, report):
                self._show("profiles", f"Created {name}", "ok")

        return self._run_saves("profiles", f"Creating {name}…",
                               lambda: profiles.create_profile(paths, name, active), created)

    def rename_profile(self) -> bool:
        """Rename the profile in use (its backups folder follows)."""
        app = self.app
        if app.saves_locked():
            return False
        old = app.settings.profile
        new = dialogs.ask_name(app, "Rename profile", f"A new name for {old}. Its saves and "
                               "backups stay with it.", initial=old, ok_text="Rename",
                               check=lambda n: self._name_problem(n, keep=old))
        if not new or new == old:
            return False
        paths = app.paths

        def renamed(result, error) -> None:
            if error is not None:
                self._show("profiles", f"Could not rename {old}: {_reason(error)}", "bad")
                return
            app.settings.profile = new
            if app.save_settings():
                self._show("profiles", f"Renamed {old} to {new}", "ok")
                return
            app.settings.profile = old  # not remembered: give the backups their old name back
            app.run_saves_task(lambda: profiles.rename_profile(paths, new, old, new),
                               lambda r, e: self._show(
                                   "profiles", "Could not save the new name "
                                   "(dcport-launcher.json)", "bad"))

        return self._run_saves("profiles", f"Renaming {old}…",
                               lambda: profiles.rename_profile(paths, old, new, old), renamed)

    def _pick_profile_to_delete(self) -> None:
        if self.app.saves_locked():
            return
        self.reload()
        others = [n for n in self._names if not profiles.same(n, self.app.settings.profile)]
        if not others:
            self._show("profiles", "There is no other profile to delete", "warn")
            return
        menu = self._menu_at(self.delete_profile_button)
        for name in others:
            menu.add_command(label=name, command=lambda v=name: self.delete_profile(v))
        self._popup(menu, self.delete_profile_button)

    def delete_profile(self, name: str) -> bool:
        """Delete a profile that is not in use, after a zip of its saves."""
        app = self.app
        if app.saves_locked():
            return False
        active = app.settings.profile
        if profiles.same(name, active):
            self._show("profiles", "The profile in use can't be deleted; switch to another "
                       "one first", "warn")
            return False
        if not dialogs.confirm(app, "Delete profile",
                               f"Delete the profile {name}?\n\nIts saves are zipped to "
                               f"backups\\{name} first, so they can be restored later.",
                               "Delete"):
            return False
        paths = app.paths

        def deleted(result: "Path | None", error) -> None:
            if error is not None:
                self._show("profiles", f"Could not delete {name}: {_reason(error)}", "bad")
            elif result is None:
                self._show("profiles", f"Deleted {name} (it had no saves)", "ok")
            else:
                self._show("profiles", f"Deleted {name}. Its saves are in backups\\"
                           f"{result.parent.name}\\{result.name}", "ok")

        return self._run_saves("profiles", f"Deleting {name}…",
                               lambda: profiles.delete_profile(paths, name, active,
                                                               datetime.now()), deleted)

    def open_saves(self) -> None:
        folder = self.app.paths.user / "savedata"
        if not folder.is_dir():
            self._show("profiles", "No saves yet", "warn")
            return
        try:
            os.startfile(str(folder))  # returns at once; Explorer opens on its own
        except OSError as e:
            self._show("profiles", f"Could not open the saves folder: {_reason(e)}", "bad")
            return
        self._show("profiles", "", "muted")

    # ------------------------------------------------------------ backups

    def backup_items(self) -> "list[tuple[Path, str, str, str]]":
        """(zip, date, kind, size) as the list shows them, newest first."""
        return self.backup_list.items()

    def select_backup(self, index: int) -> None:
        self.backup_list.pick(index)

    def backup_now(self) -> bool:
        paths, profile = self.app.paths, self.app.settings.profile
        return self._run_saves("backups", "Backing up saves…",
                               lambda: tools.backup_saves(paths, datetime.now(), profile),
                               self._backed_up)

    def _backed_up(self, result: "Path | None", error) -> None:
        if error is None:
            self._show("backups", f"Saves backed up to backups\\{result.parent.name}\\"
                       f"{result.name}", "ok")
        elif isinstance(error, FileNotFoundError) and error.errno is None:
            self._show("backups", str(error), "warn")  # "No saves to back up"
        else:
            self._show("backups", f"Could not back up saves: {_reason(error)}", "bad")

    def restore_selected(self) -> bool:
        app, item = self.app, self.backup_list.picked()
        if item is None or app.saves_locked():
            return False
        path, when, kind, _ = item
        profile = app.settings.profile
        if not dialogs.confirm(app, "Restore backup",
                               f"Replace the saves of {profile} with the backup from {when} "
                               f"({kind.lower()})?\n\nThe saves you have now are zipped "
                               "first and stay in the list (Before restore).", "Restore"):
            return False
        paths = app.paths

        def restored(result: "Path | None", error) -> None:
            if error is not None:
                self._show("backups", f"Could not restore: {_reason(error)}", "bad")
                return
            kept = "; your previous saves are listed as Before restore" if result else ""
            self._show("backups", f"Restored the backup from {when}{kept}", "ok")
            app.refresh_checks()

        return self._run_saves("backups", f"Restoring the backup from {when}…",
                               lambda: profiles.restore_backup(paths, profile, path,
                                                               datetime.now()), restored)

    def delete_selected(self) -> bool:
        app, item = self.app, self.backup_list.picked()
        if item is None or app.saves_locked():
            return False
        path, when, kind, _ = item
        if not dialogs.confirm(app, "Delete backup",
                               f"Delete the backup from {when} ({kind.lower()})?\n\n"
                               "The zip is removed for good.", "Delete"):
            return False

        def deleted(result, error) -> None:
            if error is not None:
                self._show("backups", f"Could not delete the backup: {_reason(error)}", "bad")
            else:
                self._show("backups", f"Deleted the backup from {when}", "ok")

        return self._run_saves("backups", "Deleting the backup…",
                               lambda: profiles.delete_backup(path), deleted)

    # ------------------------------------------------------------ shader cache

    def clear_shader_cache(self) -> bool:
        paths = self.app.paths
        return self._run("cache", "Clearing the shader cache…",
                         lambda: tools.clear_shader_cache(paths), self._cleared)

    def _cleared(self, removed: "int | None", error) -> None:
        if error is not None:
            self._show("cache", f"Could not clear the shader cache: {_reason(error)}", "bad")
        elif removed == 0:
            self._show("cache", "The shader cache is already empty", "ok")
        else:
            self._show("cache", f"Shader cache cleared ({removed} file"
                       f"{'' if removed == 1 else 's'})", "ok")
        self._measure_cache()

    # ------------------------------------------------------------ shortcut and Steam

    def set_shortcut_direct(self, direct: bool) -> None:
        self.app.settings.shortcut_direct = bool(direct)
        self._choice_saved("shortcut", self.app.save_settings())  # refreshes the choices

    def set_steam_direct(self, direct: bool) -> None:
        self.app.settings.steam_direct = bool(direct)
        self._choice_saved("steam", self.app.save_settings())

    def _choice_saved(self, where: str, ok: bool) -> None:
        if not ok:
            self._show(where, "Could not save this choice (dcport-launcher.json)", "bad")

    def create_shortcut(self) -> bool:
        paths, direct = self.app.paths, self.app.settings.shortcut_direct

        def work():  # the real Desktop, also when OneDrive has moved it
            return tools.create_desktop_shortcut(paths, direct, tools.desktop_dir())

        return self._run("shortcut", "Creating the desktop shortcut…", work,
                         lambda result, error: self._shortcut_done(direct, error))

    def _shortcut_done(self, direct: bool, error) -> None:
        if error is None:
            starts = "the game directly" if direct else "the launcher"
            self._show("shortcut", f"Desktop shortcut created (starts {starts})", "ok")
        else:
            self._show("shortcut", f"Could not create the shortcut: {_reason(error)}", "bad")

    def add_to_steam(self) -> bool:
        paths, direct = self.app.paths, self.app.settings.steam_direct

        def work():
            if tools.steam_running():
                return None  # Steam rewrites shortcuts.vdf when it exits
            return tools.add_to_steam(paths, direct, tools.steam_userdata_dirs(None))

        return self._run("steam", "Adding DRIVECLUB to Steam…", work, self._steam_done)

    def _steam_done(self, count: "int | None", error) -> None:
        if error is None and count is None:
            self._show("steam", "Close Steam first, then try again", "warn")
        elif error is None:
            self._show("steam", f"Added to Steam for {count} user(s). Start Steam to see it.",
                       "ok")
        elif isinstance(error, tools.SteamError):
            self._show("steam", str(error), "bad")
        else:
            self._show("steam", f"Could not add to Steam: {_reason(error)}", "bad")
