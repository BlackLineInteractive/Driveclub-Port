# SPDX-License-Identifier: GPL-2.0-or-later
"""LOG page: the end of the last run's log, live while it runs, and a bug-report copy."""

import os
import platform
import tkinter as tk
from pathlib import Path

from dcport import __version__
from dcport.ui import theme

TAIL_LINES = 6000
REPORT_LINES = 400  # what a bug report needs; GitHub issues stop at 65536 characters
REPORT_CHARS = 50000
NO_LOG = "No log yet — press PLAY first."
ERROR_MARKS = ("<Error>", "<Critical>")
_BLOCK = 64 * 1024
_MAX_BYTES = 16 * 1024 * 1024


def read_tail(path: Path, lines: int = TAIL_LINES, max_bytes: int = _MAX_BYTES) -> str:
    """The last `lines` lines of a text file, read backwards from the end in blocks."""
    with open(path, "rb") as f:
        f.seek(0, os.SEEK_END)
        end = pos = f.tell()
        chunks: list[bytes] = []
        newlines = 0
        while pos > 0 and newlines <= lines and end - pos < max_bytes:
            step = min(_BLOCK, pos)
            pos -= step
            f.seek(pos)
            chunk = f.read(step)
            chunks.append(chunk)
            newlines += chunk.count(b"\n")
    parts = b"".join(reversed(chunks)).decode("utf-8", errors="replace").split("\n")
    if parts and parts[-1] == "":
        parts.pop()
    if pos > 0 and len(parts) <= lines:
        parts = parts[1:]  # the first line was cut by the size limit
    return "\n".join(p[:-1] if p.endswith("\r") else p for p in parts[-lines:])


class LogPage(tk.Frame):
    def __init__(self, master, app):
        super().__init__(master)
        self.app = app
        self._key = None
        self._live_job = None
        self._status_job = None
        self._message = ""
        self._role = "muted"
        self._empty = True
        app.paint(self, bg="bg")
        self._build_bottom()
        self.board = theme.CardBoard(self, app, columns=1, fill=True)
        self.board.pack(fill="both", expand=True)
        self._build_panel(self.board.add(0, 0))
        app.on_theme(self._paint)

    # ------------------------------------------------------------ building

    def _build_panel(self, body) -> None:
        app = self.app
        head = tk.Frame(body)
        app.paint(head, bg="panel")
        head.pack(fill="x", pady=(0, app.px(12)))
        self.title = theme.text_label(head, app, "last_run.log", font="card_title")
        self.title.pack(side="left")
        self.where = theme.text_label(head, app, font="small", fg="muted")
        self.where.pack(side="left", padx=(app.px(12), 0), pady=(app.px(3), 0))
        self.live_badge = theme.text_label(head, app, "●  L I V E",
                                           font="caps", fg="accent")
        box = tk.Frame(body)
        app.paint(box, bg="panel")
        box.pack(fill="both", expand=True)
        self.text = tk.Text(box, wrap="word", font=app.fonts["mono"], relief="flat", bd=0,
                            highlightthickness=0, padx=0, pady=0, spacing1=1, undo=False,
                            cursor="arrow")
        self.scroll = theme.ThinScrollbar(box, app, self.text.yview)
        self.text.configure(yscrollcommand=self.scroll.set, state="disabled")
        self.scroll.pack(side="right", fill="y", padx=(app.px(10), 0))
        self.text.pack(side="left", fill="both", expand=True)

    def _build_bottom(self) -> None:
        app = self.app
        bar = tk.Frame(self)
        app.paint(bar, bg="bg")
        bar.pack(side="bottom", fill="x", padx=app.px(24), pady=(app.px(16), app.px(20)))
        for text, command in (("Copy for bug report", self.copy_report),
                              ("Open folder", self.open_folder)):
            theme.PillButton(bar, app, text, command, kind="secondary",
                             font=app.fonts["button"], height=app.px(40),
                             padx=app.px(22)).pack(side="right", padx=(app.px(12), 0))
        self.status = theme.text_label(bar, app, font="body_bold", bg="bg")
        self.status.pack(side="left")

    def _paint(self, pal: dict) -> None:
        self.text.configure(bg=pal["panel"], fg=pal["text"], insertbackground=pal["panel"],
                            selectbackground=pal["accent"], selectforeground=theme.WHITE,
                            inactiveselectbackground=theme.mix(pal["accent"], pal["panel"], 0.55))
        self.text.tag_configure("error", foreground=pal["bad"])
        self.text.tag_configure("empty", foreground=pal["muted"], font=self.app.fonts["body"])
        self._paint_status()

    # ------------------------------------------------------------ showing

    def on_show(self) -> None:
        self.refresh()
        if self.app.phase != "idle":
            self._start_live()

    def on_hide(self) -> None:
        self._stop_live()

    def set_phase(self, phase: str) -> None:
        if phase == "idle":
            self._stop_live()
            if self.app.current == "log":
                self.refresh()  # the last lines the process wrote
        elif self.app.current == "log":
            self._start_live()

    def live(self) -> bool:
        return self._live_job is not None

    def _start_live(self) -> None:
        if self._live_job is None:
            self._live_job = self.app.after(1000, self._tick)
        if not self.live_badge.winfo_manager():
            self.live_badge.pack(side="right", before=self.where)  # a long path gives way

    def _stop_live(self) -> None:
        if self._live_job is not None:
            self.app.after_cancel(self._live_job)
            self._live_job = None
        self.live_badge.pack_forget()

    def _tick(self) -> None:
        self._live_job = None
        self.refresh()
        if self.app.phase != "idle" and self.app.current == "log":
            self._start_live()
        else:
            self._stop_live()

    def refresh(self) -> None:
        """Show the end of App.last_log; does nothing when the file has not changed."""
        path = Path(self.app.last_log)
        self.title.configure(text=path.name)
        self.where.configure(text=str(path.parent))
        try:
            st = path.stat()
            key = (str(path), st.st_size, st.st_mtime_ns)
        except OSError:
            st, key = None, (str(path), None)
        if key == self._key:
            return
        self._key = key
        if st is None:
            self._fill(NO_LOG, empty=True)
            return
        try:
            text = read_tail(path)
        except OSError as e:
            self._fill(f"Could not read {path.name}: {e.strerror or e}", empty=True)
            return
        self._fill(text)

    def _fill(self, text: str, empty: bool = False) -> None:
        t = self.text
        follow = self._empty or t.yview()[1] >= 0.999  # at the bottom: keep following
        top = t.index("@0,0")
        t.configure(state="normal")
        t.delete("1.0", "end")
        if empty:
            t.insert("1.0", text, ("empty",))
        else:
            t.insert("1.0", *self._runs(text))
        t.configure(state="disabled")
        if follow:
            t.yview_moveto(1.0)
        else:
            t.yview(top)
        self._empty = empty

    @staticmethod
    def _runs(text: str) -> list:
        """Text.insert arguments: runs of lines, error lines tagged."""
        args: list = []
        run: list[str] = []
        tag = ()
        for line in text.split("\n"):
            line_tag = ("error",) if any(m in line for m in ERROR_MARKS) else ()
            if line_tag != tag and run:
                args += ["\n".join(run) + "\n", tag]
                run = []
            tag = line_tag
            run.append(line)
        if run:
            args += ["\n".join(run), tag]
        return args or [""]

    def text_content(self) -> str:
        return self.text.get("1.0", "end-1c")

    # ------------------------------------------------------------ buttons

    def report_text(self) -> str:
        app, s = self.app, self.app.settings
        path = Path(app.last_log)
        lines = [
            f"DCport {__version__}",
            f"Edition: {'FSR' if app.fsr else 'no FSR'}",
            f"GPU: {app.gpu or 'unknown'}",
            f"Frame rate: {s.fps} FPS",
            f"Window: {'Fullscreen' if s.fullscreen else 'Windowed'}",
        ]
        if app.fsr:
            ini = app.game_ini()
            lines.append(f"Upscaler: {ini.get('upscaler', 'fsr3')}, "
                         f"sharpness {ini.get('sharpness', '0.30')}")
        lines.append(f"Windows: {platform.version()}")
        try:
            tail = read_tail(path, REPORT_LINES)
        except OSError:
            tail = ""
        if len(tail) > REPORT_CHARS:
            tail = tail[-REPORT_CHARS:]
            tail = tail[tail.find("\n") + 1:]
        if not tail:
            return "\n".join(lines + [f"Log: {path.name} (none yet)"]) + "\n"
        count = tail.count("\n") + 1
        lines.append(f"Log: {path.name}, last {count} line{'s' if count != 1 else ''}")
        return "\n".join(lines) + "\n\n```\n" + tail + "\n```\n"

    def copy_report(self) -> None:
        text = self.report_text()
        self.app.clipboard_clear()
        self.app.clipboard_append(text)
        self._flash("Copied", "ok")

    def open_folder(self) -> None:
        folder = Path(self.app.last_log).parent
        if not folder.is_dir():
            self._flash("The log folder does not exist yet", "warn")
            return
        try:
            os.startfile(str(folder))  # returns at once; Explorer opens on its own
        except OSError as e:
            self._flash(f"Could not open the folder: {e.strerror or e}", "bad")

    def status_text(self) -> str:
        return self._message

    def _flash(self, text: str, role: str) -> None:
        self._message, self._role = text, role
        self._paint_status()
        if self._status_job is not None:
            self.app.after_cancel(self._status_job)
        self._status_job = self.app.after(4000, self._clear_status)

    def _clear_status(self) -> None:
        self._status_job = None
        self._message = ""
        self._paint_status()

    def _paint_status(self) -> None:
        glyph = {"ok": "✓", "warn": "⚠", "bad": "✕"}.get(self._role, "")
        text = f"{glyph}  {self._message}" if glyph and self._message else self._message
        self.status.configure(text=text, fg=self.app.palette[self._role])
