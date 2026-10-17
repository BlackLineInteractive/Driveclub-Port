# SPDX-License-Identifier: GPL-2.0-or-later
"""RACE page: game art, Session card, ready checks and the PLAY button."""

import tkinter as tk

from PIL import ImageTk

from dcport import profiles
from dcport.game import Check, GameInfo
from dcport.runtime import FPS_PRESETS, fps_note
from dcport.ui import theme

GLYPHS = {"ok": "✓", "warn": "⚠", "bad": "✕", "pending": "…"}
ROWS = (("profile", "Profile"), ("fps", "Frame rate"), ("window", "Window"),
        ("counter", "FPS counter"))


def _short_version(app_ver: str) -> str:
    major, _, minor = app_ver.partition(".")
    return f"{int(major)}.{minor}" if major.isdigit() and minor else app_ver


class _Sweep(tk.Canvas):
    """Slim progress bar with a red segment sweeping across (length of work unknown)."""

    def __init__(self, master, app, width: int, height: int):
        super().__init__(master, width=width, height=height, highlightthickness=0, bd=0)
        self.app = app
        self._len, self._thick = width, height
        self._step = 0
        self._job = None
        self._track = self.create_rectangle(0, 0, width, height, width=0)
        self._bar = self.create_rectangle(0, 0, 0, height, width=0)
        app.on_theme(self._colour)

    def _colour(self, pal: dict) -> None:
        self.configure(bg=pal["bg"])
        self.itemconfigure(self._track, fill=theme.mix(pal["text"], pal["bg"], 0.12))
        self.itemconfigure(self._bar, fill=pal["accent"])

    def start(self) -> None:
        if self._job is None:
            self._tick()

    def stop(self) -> None:
        if self._job is not None:
            self.app.after_cancel(self._job)
            self._job = None

    def _tick(self) -> None:
        frames = 90  # one sweep in about 1.5 s
        t = (self._step % frames) / frames
        t = t * t * (3 - 2 * t)  # ease in and out
        seg = self._len * 0.3
        x = -seg + t * (self._len + seg)
        self.coords(self._bar, max(0, x), 0, min(self._len, x + seg), self._thick)
        self._step += 1
        self._job = self.app.after(16, self._tick)


class RacePage(tk.Frame):
    def __init__(self, master, app):
        super().__init__(master)
        self.app = app
        self._art = None
        self._photo = None
        self._render_id = None
        self._menu: "tk.Menu | None" = None
        self._menu_var = None
        app.paint(self, bg="bg")

        self.hero = tk.Canvas(self, highlightthickness=0, bd=0)
        app.paint(self.hero, bg="bg")
        self.hero.pack(fill="both", expand=True, padx=app.px(24))
        self._bg_item = self.hero.create_image(0, 0, anchor="nw")
        self._sub_item = self.hero.create_text(0, 0, anchor="sw", font=app.fonts["sub"])
        self._title_item = self.hero.create_text(0, 0, anchor="sw", text="DRIVECLUB",
                                                 font=app.fonts["title"])
        self._build_card()
        self._build_bottom()
        self.hero.bind("<Configure>", lambda e: self._schedule_render())
        app.on_theme(self._on_theme)

    # ------------------------------------------------------------ Session card

    def _build_card(self) -> None:
        app = self.app
        self.card = tk.Frame(self.hero)
        app.paint(self.card, bg="panel")
        self._card_item = self.hero.create_window(0, 0, anchor="ne", window=self.card)
        head = tk.Label(self.card, text="Session", font=app.fonts["card_title"], anchor="w",
                        padx=0, pady=0)
        app.paint(head, bg="panel", fg="text")
        head.grid(row=0, column=0, columnspan=3, sticky="w", pady=(0, app.px(6)))
        self.card.columnconfigure(0, weight=1)
        self._values: dict[str, tk.Label] = {}
        self._chevrons: dict[str, tk.Label] = {}
        self._row_widgets: dict[str, tuple[tk.Label, ...]] = {}
        for i, (key, text) in enumerate(ROWS, start=1):
            name = tk.Label(self.card, text=text, font=app.fonts["body"], anchor="w", padx=0)
            value = tk.Label(self.card, font=app.fonts["body_bold"], anchor="e", padx=0)
            chevron = tk.Label(self.card, text=theme.chevron(app.fonts),
                               font=app.fonts["chevron"], padx=0, bd=0)
            app.paint(name, bg="panel", fg="text")
            app.paint(value, bg="panel", fg="text")
            app.paint(chevron, bg="panel", fg="muted")
            pad = app.px(4)
            name.grid(row=i, column=0, sticky="w", pady=pad)
            value.grid(row=i, column=1, sticky="e", pady=pad)
            chevron.grid(row=i, column=2, sticky="e", padx=(app.px(6), 0), pady=pad)
            for w in (name, value, chevron):
                w.configure(cursor="hand2")
                w.bind("<Button-1>", lambda e, k=key: self._open_menu(k))
                w.bind("<Enter>", lambda e, k=key: self._hover_row(k, True))
                w.bind("<Leave>", lambda e, k=key: self._hover_row(k, False))
            self._values[key] = value
            self._chevrons[key] = chevron
            self._row_widgets[key] = (name, value, chevron)
        self._note = tk.Label(self.card, font=app.fonts["small"], anchor="w", justify="left",
                              padx=0)
        app.paint(self._note, bg="panel", fg="warn")
        self._note.grid(row=len(ROWS) + 1, column=0, columnspan=3, sticky="we")
        self._note.grid_remove()
        self.card.bind("<Configure>", lambda e: self._schedule_render())
        app.on_theme(lambda pal: self._paint_profile_row())
        self.refresh_session()

    def _row_enabled(self, key: str) -> bool:
        """Profiles switch only while nothing runs and no saves are being moved."""
        return key != "profile" or not self.app.saves_locked()

    def _hover_row(self, key: str, on: bool) -> None:
        if not self._row_enabled(key):
            return
        pal = self.app.palette
        self._values[key].configure(fg=pal["accent"] if on else pal["text"])
        self._chevrons[key].configure(fg=pal["accent"] if on else pal["muted"])

    def _paint_profile_row(self) -> None:
        pal, live = self.app.palette, self._row_enabled("profile")
        name, value, chevron = self._row_widgets["profile"]
        dim = theme.mix(pal["text"], pal["panel"], 0.45)
        name.configure(fg=pal["text"] if live else dim)
        value.configure(fg=pal["text"] if live else dim)
        chevron.configure(fg=pal["muted"] if live else dim)
        for w in (name, value, chevron):
            w.configure(cursor="hand2" if live else "arrow")

    def refresh_profiles(self) -> None:
        """The active profile or the saves lock changed."""
        self.refresh_session()
        self._paint_profile_row()

    def set_profile(self, name: str) -> bool:
        """Switch profiles from the Session card; errors show in the message area."""
        def report(text: str, role: str) -> None:
            if role != "ok":
                self.show_message(text, role)

        return self.app.switch_profile(name, report)

    def _profile_value(self, name: str) -> str:
        return theme.shorten_end(name, self.app.fonts["body_bold"], self.app.px(170))

    def _show_fps_counter(self) -> bool:
        return self.app.fps_counter_on()

    def on_show(self) -> None:
        """SETUP may have changed the values: the card always shows the saved ones."""
        self.refresh_session()

    def session_values(self) -> dict[str, str]:
        return {key: label.cget("text") for key, label in self._values.items()}

    def refresh_session(self) -> None:
        s = self.app.settings
        self._values["profile"].configure(text=self._profile_value(s.profile))
        self._values["fps"].configure(text=str(s.fps))
        self._values["window"].configure(text="Fullscreen" if s.fullscreen else "Windowed")
        self._values["counter"].configure(text="On" if self._show_fps_counter() else "Off")
        note = fps_note(s.fps)
        self._note.configure(text=note)
        if note:
            self._note.grid()
        else:
            self._note.grid_remove()

    def fps_note_text(self) -> str:
        return self._note.cget("text")

    def set_fps(self, fps: int) -> None:
        self.app.settings.fps = int(fps)
        self.app.save_settings()
        self.refresh_session()

    def _custom_fps(self) -> None:
        self.app.show("setup")
        self.app.pages["setup"].focus_custom()

    def set_fullscreen(self, on: bool) -> None:
        self.app.settings.fullscreen = bool(on)
        self.app.save_settings()
        self.refresh_session()

    def set_fps_counter(self, on: bool) -> None:
        error = self.app.update_game_ini({"show_fps": "1" if on else "0"})
        if error:
            self.show_message(error, "bad")
        self.refresh_session()

    def _open_menu(self, key: str) -> None:
        app, pal, s = self.app, self.app.palette, self.app.settings
        if not self._row_enabled(key):
            return
        if self._menu is not None:  # the previous popup; its command has run by now
            self._menu.destroy()
        menu = tk.Menu(self, tearoff=0, bg=pal["panel"], fg=pal["text"],
                       activebackground=pal["accent"], activeforeground=theme.WHITE,
                       selectcolor=pal["text"], font=app.fonts["body"], bd=0,
                       activeborderwidth=app.px(4), relief="flat")
        self._menu = menu
        if key == "profile":
            self._menu_var = tk.StringVar(menu, value=s.profile)
            for name in profiles.list_profiles(app.paths, s.profile):
                menu.add_radiobutton(label=name, variable=self._menu_var, value=name,
                                     command=lambda v=name: self.set_profile(v))
            menu.add_separator()
            menu.add_command(label="Manage profiles…", command=lambda: app.show("garage"))
        elif key == "fps":
            self._menu_var = tk.IntVar(menu, value=s.fps)
            choices = sorted(set(FPS_PRESETS) | {s.fps})
            for fps in choices:
                label = f"{fps} FPS"
                if fps == 30:
                    label += "   original timing"
                elif fps not in FPS_PRESETS:
                    label += "   custom"
                menu.add_radiobutton(label=label, variable=self._menu_var, value=fps,
                                     command=lambda v=fps: self.set_fps(v))
            menu.add_separator()
            menu.add_command(label="Custom…", command=self._custom_fps)
        elif key == "window":
            self._menu_var = tk.IntVar(menu, value=int(s.fullscreen))
            for text, value in (("Windowed", 0), ("Fullscreen", 1)):
                menu.add_radiobutton(label=text, variable=self._menu_var, value=value,
                                     command=lambda v=value: self.set_fullscreen(bool(v)))
        else:
            self._menu_var = tk.IntVar(menu, value=int(self._show_fps_counter()))
            for text, value in (("On", 1), ("Off", 0)):
                menu.add_radiobutton(label=text, variable=self._menu_var, value=value,
                                     command=lambda v=value: self.set_fps_counter(bool(v)))
        # right-aligned under the value, so it stays over the card
        value, chevron = self._values[key], self._chevrons[key]
        menu.update_idletasks()
        right = chevron.winfo_rootx() + chevron.winfo_width()
        try:
            menu.tk_popup(right - menu.winfo_reqwidth(),
                          value.winfo_rooty() + value.winfo_height() + app.px(4))
        finally:
            menu.grab_release()

    # ------------------------------------------------------------ bottom bar

    def _build_bottom(self) -> None:
        app = self.app
        bar = tk.Frame(self)
        app.paint(bar, bg="bg")
        bar.pack(fill="x", padx=app.px(24), pady=(app.px(16), app.px(20)))
        self.play_button = theme.PillButton(bar, app, "PLAY", app.play, icon="play",
                                            font=app.fonts["play"], height=app.px(56),
                                            padx=app.px(44))
        self.play_button.pack(side="right")
        self.stop_button = theme.PillButton(bar, app, "■  STOP", app.stop_game,
                                            kind="secondary", font=app.fonts["button"],
                                            height=app.px(40), padx=app.px(22))
        self.status = tk.Frame(bar)
        app.paint(self.status, bg="bg")
        self.status.pack(side="left", fill="x", expand=True, padx=(0, app.px(20)))

        self.checks = theme.Flow(self.status, gap_x=app.px(22), gap_y=app.px(4))
        app.paint(self.checks, bg="bg")

        self.preparing = tk.Frame(self.status)
        app.paint(self.preparing, bg="bg")
        prep_label = tk.Label(self.preparing, font=app.fonts["body"], anchor="w", padx=0,
                              text="Preparing game files… this happens once")
        app.paint(prep_label, bg="bg", fg="text")
        prep_label.pack(anchor="w")
        self.progress = _Sweep(self.preparing, app, app.px(380), app.px(5))
        self.progress.pack(anchor="w", pady=(app.px(10), 0))

        self.message = tk.Frame(self.status)
        app.paint(self.message, bg="bg")
        self._message_label = tk.Label(self.message, font=app.fonts["body_bold"], anchor="w",
                                       padx=0)
        app.paint(self._message_label, bg="bg")
        self._message_label.pack(side="left")
        self._log_button = theme.PillButton(self.message, app, "Open log", app.open_log,
                                            kind="secondary", font=app.fonts["button"],
                                            height=app.px(32), padx=app.px(16))
        self._dismiss = tk.Label(self.message, text="✕", font=app.fonts["body"],
                                 cursor="hand2", padx=app.px(6))
        app.paint(self._dismiss, bg="bg", fg="muted")
        self._dismiss.bind("<Button-1>", lambda e: self.clear_message())
        self._message_state = "bad"
        self._checks: list[Check] = []
        self._showing = None
        self._show_status("checks")

    def _show_status(self, which: str) -> None:
        if which == self._showing:
            return
        for name, frame in (("checks", self.checks), ("preparing", self.preparing),
                            ("message", self.message)):
            if name == which:
                frame.pack(fill="x", anchor="w")
            else:
                frame.pack_forget()
        if which == "preparing":
            self.progress.start()
        else:
            self.progress.stop()
        self._showing = which

    def show_checks(self, checks: "list[Check]") -> None:
        self._checks = list(checks)
        for child in self.checks.winfo_children():
            child.destroy()
        pal = self.app.palette
        for check in self._checks:
            label = tk.Label(self.checks, text=f"{GLYPHS.get(check.state, '')} {check.label}",
                             font=self.app.fonts["body"], padx=0, bg=pal["bg"],
                             fg=self._check_color(check.state, pal))
            label.check_state = check.state
            if check.state == "bad":
                label.configure(cursor="hand2")
                label.bind("<Button-1>", lambda e: self.app.ask_game_folder())
        self.checks.relayout()

    @staticmethod
    def _check_color(state: str, pal: dict) -> str:
        return pal["muted"] if state == "pending" else pal.get(state, pal["text"])

    def show_message(self, text: str, state: str = "bad", log_button: bool = False) -> None:
        self._message_state = state
        self._message_label.configure(text=text, fg=self.app.palette[state])
        self._log_button.pack_forget()
        self._dismiss.pack_forget()
        if log_button:
            self._log_button.pack(side="left", padx=(self.app.px(16), 0))
        self._dismiss.pack(side="left", padx=(self.app.px(10), 0))
        self._show_status("message")

    def clear_message(self) -> None:
        self._message_label.configure(text="")
        if self.app.phase == "preparing":
            self._show_status("preparing")
        else:
            self._show_status("checks")

    def status_text(self) -> str:
        return self._message_label.cget("text") if self._showing == "message" else ""

    def set_phase(self, phase: str) -> None:
        if phase == "preparing":
            self._message_label.configure(text="")
            self._show_status("preparing")
            self.play_button.set("PREPARING…", enabled=False)
            self.stop_button.pack_forget()
        elif phase == "running":
            if self._showing == "preparing":
                self._show_status("checks")
            self.play_button.set("RUNNING…", enabled=False)
            self.stop_button.set("■  STOP", enabled=True)
            self.stop_button.pack(side="right", padx=(0, self.app.px(14)))
        else:
            if self._showing == "preparing":
                self._show_status("checks")
            self.play_button.set("PLAY", enabled=True, icon="play")
            self.stop_button.pack_forget()
        self._paint_profile_row()

    def stopping(self) -> None:
        self.stop_button.set("STOPPING…", enabled=False)

    # ------------------------------------------------------------ hero

    def set_game(self, info: "GameInfo | None", art) -> None:
        self._art = art
        sub = ""
        if info is not None and info.title_id:
            sub = f"{info.title_id} · v{_short_version(info.app_ver)}"
        self.hero.itemconfigure(self._sub_item, text=self._spaced(sub))
        self._schedule_render(0)

    @staticmethod
    def _spaced(text: str) -> str:
        """Letter-spacing for the small line above the title (hair spaces)."""
        return "\u200a".join(text)

    def _schedule_render(self, delay: int = 60) -> None:
        if self._render_id is not None:
            self.app.after_cancel(self._render_id)
        self._render_id = self.app.after(delay, self._render)

    def _render(self) -> None:
        self._render_id = None
        app, pal = self.app, self.app.palette
        w, h = self.hero.winfo_width(), self.hero.winfo_height()
        if w < 40 or h < 40:
            return
        inset, pad = app.px(22), app.px(18)
        card_w = int(min(max(w * 0.34, app.px(300)), app.px(370)))
        self.hero.coords(self._card_item, w - inset - pad, inset + pad)
        self.hero.itemconfigure(self._card_item, width=card_w - 2 * pad)
        self._note.configure(wraplength=card_w - 2 * pad)
        card_h = self.card.winfo_reqheight()
        box = (w - inset - card_w, inset, w - inset, inset + card_h + 2 * pad)
        image = theme.render_hero(w, h, pal, self._art, box, app.scale)
        self._photo = ImageTk.PhotoImage(image, master=self.hero)
        self.hero.itemconfigure(self._bg_item, image=self._photo)

        on_art = self._art is not None
        title_fg = theme.WHITE if on_art else pal["text"]
        sub_fg = "#d3d8de" if on_art else theme.mix(pal["text"], pal["hero"][0], 0.8)
        left = app.px(34)
        title_font = app.fonts["title"]
        descent = title_font.metrics("descent")
        base_y = h - app.px(30) + descent
        self.hero.coords(self._title_item, left, base_y)
        self.hero.itemconfigure(self._title_item, fill=title_fg)
        cap_top = base_y - descent - round(title_font.metrics("ascent") * 0.70)
        sub_descent = app.fonts["sub"].metrics("descent")
        self.hero.coords(self._sub_item, left + app.px(4), cap_top - app.px(10) + sub_descent)
        self.hero.itemconfigure(self._sub_item, fill=sub_fg)

    def _on_theme(self, pal: dict) -> None:
        for child in self.checks.winfo_children():
            child.configure(bg=pal["bg"], fg=self._check_color(child.check_state, pal))
        self._message_label.configure(fg=pal[self._message_state])
        self._schedule_render(0)
