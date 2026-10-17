# SPDX-License-Identifier: GPL-2.0-or-later
"""Launcher window: top bar with tabs and theme switch, the pages, and the PLAY flow."""

import dataclasses
import os
import queue
import re
import sys
import threading
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

from PIL import Image, ImageTk

from dcport import patches, profiles, sysinfo
from dcport.game import art_path, game_info, has_eboot, ready_checks
from dcport.ini import read_ini, update_ini
from dcport.paths import AppPaths
from dcport.prepare_state import is_prepared, prepare_command
from dcport.process import RUNNER
from dcport.runtime import game_command, game_env
from dcport.settings import load_settings, save_settings
from dcport.ui import theme
from dcport.ui.controls import ControlsPage
from dcport.ui.garage import GaragePage
from dcport.ui.log import LogPage
from dcport.ui.patches import PatchesPage
from dcport.ui.race import RacePage
from dcport.ui.setup import SetupPage

TABS = ("race", "setup", "patches", "controls", "garage", "log")
PAGES = {"race": RacePage, "setup": SetupPage, "patches": PatchesPage, "controls": ControlsPage,
         "garage": GaragePage, "log": LogPage}

_GPU_CACHE: dict[str, "str | None"] = {}
_WINDOW_POS = re.compile(r"[+-]-?\d+[+-]-?\d+")


def _lookup_gpu(results: "queue.Queue") -> None:
    """Worker thread: never touches Tk, only the queue."""
    if "name" not in _GPU_CACHE:
        _GPU_CACHE["name"] = sysinfo.gpu_name()
    results.put(("gpu", _GPU_CACHE["name"]))


def _wait_for(runner, kind: str, results: "queue.Queue") -> None:
    """Worker thread: waits for the child process and reports its exit code."""
    results.put(("ended", kind, runner.wait()))


def _reason(error: BaseException) -> str:
    return getattr(error, "strerror", None) or str(error) or type(error).__name__


def _code(code: "int | None") -> str:
    if code is None:
        return "unknown"
    if code < 0 or code >= 0x80000000:
        return f"0x{code & 0xFFFFFFFF:08X}"
    return str(code)


class App(tk.Tk):
    def __init__(self, paths: AppPaths, fsr: bool):
        theme.enable_dpi_awareness()
        super().__init__(className="DCport")
        self.paths = paths
        self.fsr = fsr
        self.settings = load_settings(paths)
        if self.settings.theme not in theme.PALETTES:
            self.settings.theme = "dark"
        self._default_fps_counter()
        self.runner = RUNNER
        self.last_log: Path = paths.log
        self.gpu: "str | None" = None
        self.gpu_done = False
        self.phase = "idle"  # idle | preparing | running
        self._then_play = False  # start the game when the running prepare succeeds
        self.saves_busy = False  # a profile or backup task is moving save files
        self.recovering = True  # startup recovery of interrupted save moves is pending
        self._close_pending = False  # the window closes when that task is done
        self.icon_images: list = []
        self.pages: dict[str, tk.Misc] = {}
        self.current: "str | None" = None
        self._after_ids: set[str] = set()
        self._results: "queue.Queue" = queue.Queue()
        self._painted: list[tuple[tk.Misc, dict[str, str]]] = []
        self._theme_hooks: list = []

        self.scale = theme.ui_scale(self)
        self.fonts = theme.make_fonts(self)
        self.title("DCport")
        self._set_icon()
        self._place_window()
        self.style = ttk.Style(self)
        self.style.theme_use("clam")
        theme.style_ttk(self.style, self.palette, self.fonts)
        self.configure(bg=self.palette["bg"])

        self._build_top()
        self.body = tk.Frame(self)
        self.paint(self.body, bg="bg")
        self.body.pack(fill="both", expand=True)
        self.body.rowconfigure(0, weight=1)
        self.body.columnconfigure(0, weight=1)
        # the window sets the body's size; without this, grid never places a page that
        # asks for a height of 1 (a card board before its first layout)
        self.body.grid_propagate(False)
        for name in TABS:
            self.pages[name] = PAGES[name](self.body, self)
        self.show("race")
        self.apply_theme()
        self.bind("<Map>", self._on_map, add="+")
        self.protocol("WM_DELETE_WINDOW", self._on_close)

        self.refresh_game()
        self._start_recovery()
        threading.Thread(target=_lookup_gpu, args=(self._results,), daemon=True).start()
        self.after(120, self._poll)

    # ------------------------------------------------------------ helpers

    @property
    def palette(self) -> dict:
        return theme.PALETTES[self.settings.theme]

    def px(self, n: float) -> int:
        return round(n * self.scale)

    def paint(self, widget: tk.Misc, **roles: str) -> None:
        """Colour a widget from the palette now and on every theme change."""
        self._painted.append((widget, roles))
        widget.configure(**{opt: self.palette[role] for opt, role in roles.items()})

    def on_theme(self, hook) -> None:
        """Call hook(palette) now and on every theme change."""
        self._theme_hooks.append(hook)
        hook(self.palette)

    def after(self, ms, func=None, *args):
        """Like Tk.after, but remembered so destroy() can cancel it."""
        if func is None:
            return super().after(ms)
        holder: list[str] = []

        def run():
            self._after_ids.discard(holder[0])
            func(*args)

        aid = super().after(ms, run)
        holder.append(aid)
        self._after_ids.add(aid)
        return aid

    def after_cancel(self, aid) -> None:
        self._after_ids.discard(aid)
        super().after_cancel(aid)

    def destroy(self) -> None:
        for aid in list(self._after_ids):
            try:
                super().after_cancel(aid)
            except tk.TclError:
                pass
        self._after_ids.clear()
        super().destroy()

    def save_settings(self) -> bool:
        ok = True
        try:
            save_settings(self.paths, self.settings)
        except OSError as e:
            ok = False
            self.pages["race"].show_message(
                f"Could not save settings: {e.strerror or e}", "bad")
        for page in self.pages.values():
            refresh = getattr(page, "refresh_session", None)
            if refresh is not None:
                refresh()
        return ok

    def game_ini(self) -> dict[str, str]:
        """dcport.ini as a dict ({} when it is missing or unreadable)."""
        try:
            return read_ini(self.paths.ini)
        except (OSError, ValueError):
            return {}

    def update_game_ini(self, changes: dict[str, str]) -> "str | None":
        """Write keys to dcport.ini, keeping every other line; returns an error or None."""
        try:
            update_ini(self.paths.ini, changes)
        except OSError as e:
            return f"Could not save dcport.ini: {e.strerror or e}"
        return None

    def fps_counter_on(self) -> bool:
        """dcport.ini show_fps; a missing key counts as on."""
        return self.game_ini().get("show_fps", "1").strip() != "0"

    def _default_fps_counter(self) -> None:
        """The launcher's default is On but the runtime's is off: without the key the
        switch would not show what the game does, so write show_fps=1 once (R20).
        An unreadable dcport.ini is left alone."""
        try:
            if "show_fps" not in read_ini(self.paths.ini):
                update_ini(self.paths.ini, {"show_fps": "1"})
        except (OSError, ValueError):
            pass

    def run_task(self, fn, on_done) -> None:
        """Run fn() on a worker thread; on_done(result, error) then runs on the UI thread."""
        def work():
            try:
                result, error = fn(), None
            except Exception as e:  # handed to on_done, which shows it
                result, error = None, e
            self._results.put(("task", on_done, result, error))

        threading.Thread(target=work, daemon=True).start()

    # ------------------------------------------------------------ profiles and backups

    def saves_locked(self) -> bool:
        """Save folders must not move while a process runs or another saves task does."""
        return (self.phase != "idle" or self.runner.running() or self.saves_busy
                or self.recovering)

    def _start_recovery(self) -> None:
        paths, active = self.paths, self.settings.profile

        def done(messages, error) -> None:
            self.recovering = False
            if error is not None:
                text, role = f"Could not check for interrupted saves: {_reason(error)}", "bad"
            elif messages:
                text, role = " ".join(messages), "warn"
            else:
                text, role = "", ""
            try:
                garage = self.pages.get("garage")
                if text and garage is not None:
                    garage._show("profiles", text, role)
            finally:
                self._profiles_changed()

        self._profiles_changed()
        self.run_task(lambda: profiles.recover(paths, active), done)

    def run_saves_task(self, work, done) -> bool:
        """Like run_task, for work that moves save files: one at a time, never while the
        game or the prepare tool runs, and PLAY waits for it. False when refused."""
        if self.saves_locked():
            return False
        self.saves_busy = True
        self._profiles_changed()

        def finished(result, error):
            self.saves_busy = False
            try:
                done(result, error)
            finally:
                self._profiles_changed()
                if self._close_pending and not self.saves_busy:
                    self.after(0, self.destroy)

        self.run_task(work, finished)
        return True

    def _profiles_changed(self) -> None:
        for page in self.pages.values():
            refresh = getattr(page, "refresh_profiles", None)
            if refresh is not None:
                refresh()

    def switch_profile(self, name: str, report) -> bool:
        """Make another profile the one that plays (its saves move into user/savedata).
        report(text, role) shows the outcome where the player asked for it."""
        current = self.settings.profile
        if profiles.same(name, current) or self.saves_locked():
            return False
        paths = self.paths

        def switched(result, error) -> None:
            if error is not None:
                report(f"Could not switch to {name}: {_reason(error)}", "bad")
                return
            self.settings.profile = name
            if self.save_settings():
                report(f"Now playing as {name}", "ok")
            else:  # the launcher could not remember it: put the folders back as they were
                self.settings.profile = current
                self.run_saves_task(lambda: profiles.switch_profile(paths, name, current),
                                    lambda r, e: switched_back(e))
            self.refresh_checks()

        def switched_back(error) -> None:
            if error is None:
                report(f"Could not save the profile choice (dcport-launcher.json), so "
                       f"{current} stays in use", "bad")
            else:
                self.settings.profile = name
                report(f"Playing as {name}, but the choice could not be saved "
                       f"(dcport-launcher.json)", "bad")
            self.refresh_checks()

        return self.run_saves_task(lambda: profiles.switch_profile(paths, current, name),
                                   switched)

    def _on_close(self) -> None:
        if self.saves_busy:  # never stop halfway through moving saves
            self._close_pending = True
            self.pages["race"].show_message("Finishing with the saves, then DCport closes…",
                                            "warn")
            return
        self.destroy()

    def _set_icon(self) -> None:
        """DCport's icon on the title bar, the taskbar and Alt-Tab (also when run from source)."""
        assets = theme.assets_dir()
        try:
            self.iconbitmap(default=str(assets / "dcport.ico"))
        except tk.TclError:
            pass
        try:
            with Image.open(assets / "dcport.png") as im:
                im = im.convert("RGBA")
                self.icon_images = [ImageTk.PhotoImage(im.resize((n, n), Image.LANCZOS),
                                                       master=self)
                                    for n in (256, 64, 48, 32, 16)]
            self.iconphoto(True, *self.icon_images)
        except (OSError, ValueError, tk.TclError):
            self.icon_images = []

    def _place_window(self) -> None:
        """Size and centre the window before it first shows. DCPORT_WINDOW_POS (a Tk
        position such as "+-1500+60") puts it elsewhere, e.g. tests on a second monitor."""
        sw, sh = self.winfo_screenwidth(), self.winfo_screenheight()
        w, h = min(self.px(1100), int(sw * 0.94)), min(self.px(700), int(sh * 0.88))
        pos = f"+{max(0, (sw - w) // 2)}+{max(0, (sh - h) // 2 - self.px(20))}"
        wanted = os.environ.get("DCPORT_WINDOW_POS", "").strip()
        if _WINDOW_POS.fullmatch(wanted):
            pos = wanted
        self.geometry(f"{w}x{h}{pos}")
        self.minsize(min(self.px(900), w), min(self.px(600), h))

    # ------------------------------------------------------------ top bar

    def _build_top(self) -> None:
        top = tk.Frame(self)
        self.paint(top, bg="bg")
        top.pack(fill="x", padx=self.px(24), pady=(self.px(12), self.px(8)))

        self._logo = tk.Canvas(top, highlightthickness=0, bd=0)
        self.paint(self._logo, bg="bg")
        self._logo.pack(side="left", anchor="n", padx=(self.px(4), self.px(40)))
        self.on_theme(self._draw_logo)

        # tab text sits on the logo's baseline; the red underline hangs below it
        drop = self.fonts["logo"].metrics("ascent") - self.fonts["tab"].metrics("ascent")
        self._tabs: dict[str, tuple[tk.Label, tk.Frame]] = {}
        for name in TABS:
            cell = tk.Frame(top)
            self.paint(cell, bg="bg")
            cell.pack(side="left", anchor="n", padx=(0, self.px(26)), pady=(max(0, drop), 0))
            label = tk.Label(cell, text=name.upper(), font=self.fonts["tab"], cursor="hand2",
                             padx=self.px(2), pady=0, bd=0)
            self.paint(label, bg="bg")
            label.pack(side="top")
            bar = tk.Frame(cell, height=self.px(3))
            bar.pack(side="top", fill="x", pady=(self.px(2), 0))
            for w in (label, bar):
                w.bind("<Button-1>", lambda e, n=name: self.show(n))
            label.bind("<Enter>", lambda e, n=name: self._hover_tab(n, True))
            label.bind("<Leave>", lambda e, n=name: self._hover_tab(n, False))
            self._tabs[name] = (label, bar)
        self.on_theme(lambda pal: self._paint_tabs())

        switch = tk.Label(top, text="◐", font=self.fonts["symbol"], cursor="hand2",
                          padx=self.px(4))
        self.paint(switch, bg="bg", fg="text")
        switch.pack(side="right")
        switch.bind("<Button-1>", lambda e: self.toggle_theme())
        switch.bind("<Enter>", lambda e: switch.configure(fg=self.palette["accent"]))
        switch.bind("<Leave>", lambda e: switch.configure(fg=self.palette["text"]))

    def _draw_logo(self, pal: dict) -> None:
        c, font = self._logo, self.fonts["logo"]
        c.delete("all")
        c.create_text(0, 0, text="DC", anchor="nw", font=font, fill=pal["text"])
        c.create_text(font.measure("DC"), 0, text="PORT", anchor="nw", font=font,
                      fill=pal["accent"])
        c.configure(width=font.measure("DCPORT") + self.px(6),
                    height=font.metrics("linespace"))

    def _hover_tab(self, name: str, on: bool) -> None:
        if name != self.current:
            pal = self.palette
            self._tabs[name][0].configure(fg=theme.mix(pal["text"], pal["bg"], 0.85 if on else 0.6))

    def _paint_tabs(self) -> None:
        pal = self.palette
        for name, (label, bar) in self._tabs.items():
            active = name == self.current
            label.configure(fg=pal["text"] if active else theme.mix(pal["text"], pal["bg"], 0.6))
            bar.configure(bg=pal["accent"] if active else pal["bg"])

    # ------------------------------------------------------------ pages and theme

    def show(self, tab: str) -> None:
        if tab not in self.pages:
            raise ValueError(f"unknown tab: {tab}")
        if self.current and self.current != tab:
            old = self.pages[self.current]
            old.grid_remove()
            on_hide = getattr(old, "on_hide", None)
            if on_hide is not None:
                on_hide()
        self.pages[tab].grid(row=0, column=0, sticky="nsew")
        self.current = tab
        self._paint_tabs()
        on_show = getattr(self.pages[tab], "on_show", None)
        if on_show is not None:
            on_show()

    def toggle_theme(self) -> None:
        self.settings.theme = "light" if self.settings.theme == "dark" else "dark"
        self.save_settings()
        self.apply_theme()

    def apply_theme(self) -> None:
        pal = self.palette
        self.configure(bg=pal["bg"])
        theme.style_ttk(self.style, pal, self.fonts)
        alive = []
        for widget, roles in self._painted:
            try:
                widget.configure(**{opt: pal[role] for opt, role in roles.items()})
                alive.append((widget, roles))
            except tk.TclError:
                pass  # widget was destroyed (a page was replaced)
        self._painted = alive
        hooks = []
        for hook in self._theme_hooks:
            try:
                hook(pal)
                hooks.append(hook)
            except tk.TclError:
                pass
        self._theme_hooks = hooks
        self._style_title_bar()

    def _style_title_bar(self) -> None:
        try:
            theme.style_title_bar(self, self.palette, self.settings.theme == "dark")
        except (OSError, tk.TclError):
            pass

    def _on_map(self, event) -> None:
        if event.widget is self:
            self._style_title_bar()

    # ------------------------------------------------------------ game folder and checks

    def game_dir(self) -> "Path | None":
        d = self.settings.game_dir
        return Path(d) if d and has_eboot(Path(d)) else None

    def ask_game_folder(self) -> bool:
        current = self.settings.game_dir
        initial = current if current and Path(current).is_dir() else str(self.paths.root)
        chosen = filedialog.askdirectory(
            parent=self, initialdir=initial, mustexist=True,
            title="Choose your DRIVECLUB folder (the one with eboot.bin)")
        if not chosen:
            return False
        folder = Path(chosen)
        if not has_eboot(folder):
            messagebox.showwarning(
                "DCport", f"There is no eboot.bin in\n{folder}\n\n"
                "Choose the DRIVECLUB folder (CUSA00003) that contains eboot.bin.", parent=self)
            return False
        self.settings.game_dir = str(folder)
        self.save_settings()
        self.refresh_game()
        return True

    def refresh_game(self) -> None:
        """Re-read title data and art from the game folder, then the checks."""
        gd = self.game_dir()
        art = None
        if gd is not None:
            path = art_path(gd)
            if path is not None:
                try:
                    with Image.open(path) as im:
                        art = im.convert("RGB")
                except (OSError, ValueError):
                    art = None
        self.pages["race"].set_game(game_info(gd) if gd else None, art)
        self.refresh_checks()

    def refresh_checks(self) -> None:
        gd = self.game_dir()
        prepared = False
        if gd is not None:
            try:
                prepared = is_prepared(self.paths, gd)
            except (OSError, ValueError):
                prepared = False
        folder = Path(self.settings.game_dir) if self.settings.game_dir else \
            self.paths.root / "(no game folder)"
        checks = ready_checks(folder, self.paths, self.gpu, prepared)
        if self.gpu is None:
            checks = [self._gpu_placeholder(c) for c in checks]
        self.pages["race"].show_checks(checks)

    def _gpu_placeholder(self, check):
        if not check.label.startswith("Graphics card"):
            return check
        if self.gpu_done:
            return dataclasses.replace(check, label="Graphics card not detected")
        return dataclasses.replace(check, state="pending")

    # ------------------------------------------------------------ PLAY

    def _ready_game_dir(self) -> "Path | None":
        """The game folder when nothing runs (asks for one if needed), else None."""
        race = self.pages["race"]
        if self.phase != "idle":
            return None
        if self.runner.running():
            race.show_message("DRIVECLUB is already running.", "warn")
            return None
        if self.recovering or self.saves_busy:
            race.show_message("One moment: the saves are being moved. Press PLAY again then.",
                              "warn")
            return None
        gd = self.game_dir()
        if gd is None:
            if not self.ask_game_folder():
                return None
            gd = self.game_dir()
        race.clear_message()
        return gd

    def play(self) -> None:
        gd = self._ready_game_dir()
        if gd is None:
            return
        try:
            prepared = is_prepared(self.paths, gd)
        except (OSError, ValueError):
            prepared = False
        if prepared:
            self._start_game(gd)
        else:
            self.prepare(start_game=True)

    def prepare(self, start_game: bool = False) -> bool:
        """Run the prepare tool; its progress shows on RACE.

        data/source.txt (the stamp of the last prepare) goes first, so the files count as
        prepared only once the tool has finished. With start_game the game starts after a
        successful prepare; without it the game never starts from here.
        """
        gd = self._ready_game_dir()
        if gd is None:
            return False
        race = self.pages["race"]
        try:
            self.paths.data.mkdir(parents=True, exist_ok=True)
            (self.paths.data / "source.txt").unlink(missing_ok=True)
        except OSError as e:
            race.show_message(f"Could not reset the data folder: {e.strerror or e}", "bad")
            return False
        self.show("race")
        self._then_play = start_game
        self._set_phase("preparing")
        return self._start("prepare", prepare_command(self.paths, gd), dict(os.environ),
                           self.paths.prepare_log)

    def _start(self, kind: str, cmd: list[str], env: dict, log: Path) -> bool:
        race = self.pages["race"]
        what = "the prepare tool" if kind == "prepare" else "DRIVECLUB"
        try:
            started = self.runner.start(cmd, env=env, log=log, cwd=self.paths.root)
        except OSError as e:
            self._set_phase("idle")
            race.show_message(f"Could not start {what}: {e.strerror or e}", "bad")
            return False
        if not started:
            self._set_phase("idle")
            race.show_message("DRIVECLUB is already running.", "warn")
            return False
        self.last_log = log
        threading.Thread(target=_wait_for, args=(self.runner, kind, self._results),
                         daemon=True).start()
        return True

    def _start_game(self, gd: Path) -> None:
        s = self.settings
        error = patches.build_for_session(self.paths, gd, s)
        if error:
            self.pages["race"].show_message(error)
        cmd = game_command(self.paths, gd, s.fps)
        env = game_env(self.paths, s, self.fsr, dict(os.environ))
        self._set_phase("running")
        self._start("game", cmd, env, self.paths.log)

    def stop_game(self) -> None:
        if self.phase != "running":
            return
        self.pages["race"].stopping()
        threading.Thread(target=self.runner.stop, daemon=True).start()

    def open_log(self) -> None:
        self.show("log")

    def _set_phase(self, phase: str) -> None:
        self.phase = phase
        for page in self.pages.values():
            set_phase = getattr(page, "set_phase", None)
            if set_phase is not None:
                set_phase(phase)

    def _ended(self, kind: str, code: "int | None") -> None:
        race = self.pages["race"]
        stopped = self.runner.was_stopped()
        if kind == "prepare" and code == 0 and not stopped:
            gd = self.game_dir()
            self.refresh_checks()
            if not self._then_play:
                self._set_phase("idle")
                race.show_message("Game files prepared", "ok")
                return
            if gd is not None:
                self._start_game(gd)
                return
        self._set_phase("idle")
        self.refresh_checks()
        if stopped or code == 0:
            return
        if kind == "prepare":
            race.show_message(f"Preparing game files failed (code {_code(code)})", "bad",
                              log_button=True)
        else:
            race.show_message(f"DRIVECLUB stopped (code {_code(code)})", "bad", log_button=True)

    def _poll(self) -> None:
        try:
            while True:
                try:
                    msg = self._results.get_nowait()
                except queue.Empty:
                    break
                try:
                    if msg[0] == "gpu":
                        self.gpu, self.gpu_done = msg[1], True
                        self.refresh_checks()
                    elif msg[0] == "ended":
                        self._ended(msg[1], msg[2])
                    elif msg[0] == "task":
                        msg[1](msg[2], msg[3])
                except Exception:  # keep polling; Tk reports it like any callback error
                    self.report_callback_exception(*sys.exc_info())
        finally:
            self.after(150, self._poll)
