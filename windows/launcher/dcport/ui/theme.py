# SPDX-License-Identifier: GPL-2.0-or-later
"""Colours, fonts and Pillow drawing helpers for the launcher window."""

import ctypes
import math
import sys
import tkinter as tk
import tkinter.font as tkfont
from ctypes import wintypes
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageTk

ACCENT = "#d81f2a"
WHITE = "#ffffff"

PALETTES = {
    "dark": {
        "bg": "#101318",
        "panel": "#181d24",
        "text": "#e9edf2",
        "muted": "#92969b",
        "accent": ACCENT,
        "accent_hover": "#e8333e",
        "accent_down": "#b3141e",
        "ok": "#1e9e57",
        "warn": "#e3a238",
        "bad": "#ff5c63",
        "line": "#2a313b",
        "hero": ("#1e2c3d", "#0f1720", "#3c1418"),
        "shadow": (0, 0, 0, 128),
        "shadow_blur": 18,
        "shadow_y": 6,
    },
    "light": {
        "bg": "#f4f6f9",
        "panel": "#ffffff",
        "text": "#1b1f24",
        "muted": "#72757a",
        "accent": ACCENT,
        "accent_hover": "#e8333e",
        "accent_down": "#b3141e",
        "ok": "#1e9e57",
        "warn": "#b06a00",
        "bad": "#c8232c",
        "line": "#d9dee5",
        "hero": ("#cfd9e6", "#eef2f7", "#f3d3d5"),
        "shadow": (0, 0, 0, 31),
        "shadow_blur": 14,
        "shadow_y": 3,
    },
}


# ---------------------------------------------------------------- colours

def hex_rgb(color: str) -> tuple[int, int, int]:
    color = color.lstrip("#")
    return int(color[0:2], 16), int(color[2:4], 16), int(color[4:6], 16)


def mix(a: str, b: str, t: float) -> str:
    """`a` laid over `b` at opacity `t` (CSS opacity on a solid background)."""
    ra, ga, ba = hex_rgb(a)
    rb, gb, bb = hex_rgb(b)
    return "#%02x%02x%02x" % (round(ra * t + rb * (1 - t)), round(ga * t + gb * (1 - t)),
                              round(ba * t + bb * (1 - t)))


# ---------------------------------------------------------------- window setup

def enable_dpi_awareness() -> None:
    """Crisp text on scaled displays; must run before the first Tk window exists."""
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except (AttributeError, OSError):
        pass


def ui_scale(root: tk.Misc) -> float:
    """Pixel sizes are written for 96 DPI; fonts are in points and scale by themselves."""
    return max(1.0, root.winfo_fpixels("1i") / 96.0)


def resource_dir() -> Path:
    """Files bundled with the launcher: the PyInstaller folder when frozen, the repo in dev."""
    base = getattr(sys, "_MEIPASS", None)
    if base:
        return Path(base)
    return Path(__file__).resolve().parent.parent.parent


def assets_dir() -> Path:
    return resource_dir() / "assets"


def default_controls() -> Path:
    """The controls defaults, bundled like the icon (build: --add-data "input-default.ini;.")."""
    return resource_dir() / "input-default.ini"


def style_title_bar(window: tk.Misc, pal: dict, dark: bool) -> None:
    """Windows 11 title bar in the theme colour (older Windows ignores it)."""
    try:
        user32, dwm = ctypes.windll.user32, ctypes.windll.dwmapi
    except (AttributeError, OSError):
        return
    user32.GetParent.restype = wintypes.HWND
    user32.GetParent.argtypes = [wintypes.HWND]
    dwm.DwmSetWindowAttribute.argtypes = [wintypes.HWND, wintypes.DWORD, ctypes.c_void_p,
                                          wintypes.DWORD]
    hwnd = user32.GetParent(window.winfo_id())
    if not hwnd:
        return

    def colorref(color: str) -> int:
        r, g, b = hex_rgb(color)
        return r | (g << 8) | (b << 16)

    for attr, value in ((20, int(dark)), (35, colorref(pal["bg"])), (36, colorref(pal["text"]))):
        v = ctypes.c_uint(value)
        dwm.DwmSetWindowAttribute(hwnd, attr, ctypes.byref(v), ctypes.sizeof(v))
    # SWP_NOSIZE | SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED
    user32.SetWindowPos(hwnd, None, 0, 0, 0, 0, 0x0001 | 0x0002 | 0x0004 | 0x0010 | 0x0020)


# ---------------------------------------------------------------- fonts

ICON_FONT = "Segoe MDL2 Assets"  # Windows 10/11


def chevron(fonts: dict) -> str:
    """Down chevron for the Session values: MDL2 ChevronDown, or ▾ without that font."""
    return "\ue70d" if fonts["chevron"].actual("family") == ICON_FONT else "▾"


def icon(fonts: dict, code: str) -> str:
    """An MDL2 icon glyph, or nothing on a system without the icon font."""
    return code if fonts["icon"].actual("family") == ICON_FONT else ""


def make_fonts(root: tk.Misc) -> dict[str, tkfont.Font]:
    families = set(tkfont.families(root))
    ui = "Segoe UI" if "Segoe UI" in families else "Arial"
    if "Segoe UI Black" in families:
        heavy, heavy_weight = "Segoe UI Black", "normal"
    else:
        heavy, heavy_weight = ui, "bold"
    symbol = "Segoe UI Symbol" if "Segoe UI Symbol" in families else ui
    mono = next((f for f in ("Consolas", "Cascadia Mono") if f in families), "Courier New")
    icons = ICON_FONT if ICON_FONT in families else ui
    spec = {
        "chevron": (icons, 8 if icons == ICON_FONT else 11, "normal", "roman"),
        "icon": (icons, 15, "normal", "roman"),
        "logo": (heavy, 21, heavy_weight, "italic"),
        "tab": (ui, 13, "bold", "italic"),
        "title": (heavy, 48, heavy_weight, "italic"),
        "sub": (ui, 11, "normal", "roman"),
        "card_title": (ui, 13, "bold", "roman"),
        "body": (ui, 11, "normal", "roman"),
        "body_bold": (ui, 11, "bold", "roman"),
        "small": (ui, 9, "normal", "roman"),
        "small_bold": (ui, 9, "bold", "roman"),
        "caps": (ui, 8, "bold", "roman"),
        "mono": (mono, 9, "normal", "roman"),
        "play": (heavy, 18, heavy_weight, "italic"),
        "button": (ui, 10, "bold", "italic"),
        "symbol": (symbol, 15, "normal", "roman"),
    }
    return {name: tkfont.Font(root=root, family=f, size=s, weight=w, slant=sl)
            for name, (f, s, w, sl) in spec.items()}


def style_ttk(style, pal: dict, fonts: dict) -> None:
    style.configure(".", background=pal["bg"], foreground=pal["text"],
                    fieldbackground=pal["panel"], troughcolor=pal["panel"],
                    bordercolor=pal["line"], lightcolor=pal["panel"], darkcolor=pal["panel"],
                    selectbackground=pal["accent"], selectforeground=WHITE,
                    insertcolor=pal["text"], font=fonts["body"])


# ---------------------------------------------------------------- drawing

_SUPERSAMPLE = 4


def rounded_mask(w: int, h: int, r: int) -> Image.Image:
    """Anti-aliased rounded-rectangle alpha mask."""
    mask = Image.new("L", (w, h), 255)
    r = max(0, min(r, w // 2, h // 2))
    if r == 0:
        return mask
    big = r * _SUPERSAMPLE
    tile = Image.new("L", (big, big), 0)
    ImageDraw.Draw(tile).ellipse((0, 0, 2 * big - 1, 2 * big - 1), fill=255)
    tile = tile.resize((r, r), Image.LANCZOS)
    mask.paste(tile, (0, 0))
    mask.paste(tile.transpose(Image.FLIP_LEFT_RIGHT), (w - r, 0))
    mask.paste(tile.transpose(Image.FLIP_TOP_BOTTOM), (0, h - r))
    mask.paste(tile.transpose(Image.ROTATE_180), (w - r, h - r))
    return mask


def rounded_image(w: int, h: int, r: int, fill: str, bg: str, outline: "str | None" = None,
                  outline_width: int = 0) -> Image.Image:
    """A rounded rectangle (corner radius r) on a solid background."""
    w, h = max(1, w), max(1, h)
    out = Image.new("RGB", (w, h), bg)
    if outline and outline_width:
        out.paste(Image.new("RGB", (w, h), outline), (0, 0), rounded_mask(w, h, r))
        iw, ih = max(1, w - 2 * outline_width), max(1, h - 2 * outline_width)
        out.paste(Image.new("RGB", (iw, ih), fill), (outline_width, outline_width),
                  rounded_mask(iw, ih, max(0, r - outline_width)))
    else:
        out.paste(Image.new("RGB", (w, h), fill), (0, 0), rounded_mask(w, h, r))
    return out


def pill_image(w: int, h: int, fill: str, bg: str, outline: "str | None" = None,
               outline_width: int = 0) -> Image.Image:
    """A fully rounded button face on a solid background."""
    return rounded_image(w, h, h // 2, fill, bg, outline, outline_width)


def draw_play_icon(img: Image.Image, x: float, top: float, size: float, color: str) -> None:
    """Solid ▶ leaning like italic type, anti-aliased (x/top/size in pixels)."""
    ss = _SUPERSAMPLE
    lean = size * 0.12
    points = [(x + lean, top), (x + size * 0.92 + lean / 2, top + size / 2), (x, top + size)]
    mask = Image.new("L", (img.width * ss, img.height * ss), 0)
    ImageDraw.Draw(mask).polygon([(px * ss, py * ss) for px, py in points], fill=255)
    mask = mask.resize(img.size, Image.LANCZOS)
    img.paste(Image.new("RGB", img.size, color), (0, 0), mask)


def cover(img: Image.Image, w: int, h: int, focus_y: float = 0.5) -> Image.Image:
    """Scale to fill w×h and crop the overflow (CSS background-size: cover)."""
    sw, sh = img.size
    scale = max(w / sw, h / sh)
    cw, ch = w / scale, h / scale
    left = (sw - cw) / 2
    top = (sh - ch) * focus_y
    return img.resize((w, h), Image.LANCZOS, box=(left, top, left + cw, top + ch),
                      reducing_gap=2.0)


def gradient(w: int, h: int, stops: tuple[str, str, str], angle: float = 120.0) -> Image.Image:
    """CSS linear-gradient(<angle>deg, c0, c1 55%, c2), computed small and scaled up."""
    colors = [hex_rgb(c) for c in stops]
    positions = (0.0, 0.55, 1.0)
    a = math.radians(angle)
    dx, dy = math.sin(a), -math.cos(a)
    length = abs(w * dx) + abs(h * dy)
    sw, sh = max(2, w // 8), max(2, h // 8)
    data = []
    for j in range(sh):
        y = (j + 0.5) * h / sh - h / 2
        for i in range(sw):
            x = (i + 0.5) * w / sw - w / 2
            t = min(1.0, max(0.0, (x * dx + y * dy) / length + 0.5))
            k = 0 if t <= positions[1] else 1
            u = (t - positions[k]) / (positions[k + 1] - positions[k])
            c0, c1 = colors[k], colors[k + 1]
            data.append(tuple(round(c0[n] + (c1[n] - c0[n]) * u) for n in range(3)))
    small = Image.new("RGB", (sw, sh))
    small.putdata(data)
    return small.resize((w, h), Image.BICUBIC)


def _shade_bottom(img: Image.Image, start: float = 0.38, strength: float = 0.82) -> Image.Image:
    """Darken the lower part of the art so the white title stays readable."""
    w, h = img.size
    column = []
    for y in range(h):
        t = (y / max(1, h - 1) - start) / (1 - start)
        column.append(0 if t <= 0 else round(255 * strength * t ** 1.5))
    mask = Image.new("L", (1, h))
    mask.putdata(column)
    mask = mask.resize((w, h), Image.NEAREST)
    return Image.composite(Image.new("RGB", (w, h), (5, 7, 10)), img, mask)


def _card_shadow(img: Image.Image, box: tuple[int, int, int, int], pal: dict,
                 scale: float) -> None:
    x0, y0, x1, y1 = box
    cw, ch = x1 - x0, y1 - y0
    r = round(12 * scale)
    blur = pal["shadow_blur"] * scale
    margin = int(blur * 1.5) + 2
    shadow = Image.new("L", (cw + 2 * margin, ch + 2 * margin), 0)
    shadow.paste(round(pal["shadow"][3]), (margin, margin, margin + cw, margin + ch),
                 rounded_mask(cw, ch, r))
    shadow = shadow.filter(ImageFilter.GaussianBlur(blur / 2))
    img.paste(Image.new("RGB", shadow.size, pal["shadow"][:3]),
              (x0 - margin, y0 - margin + round(pal["shadow_y"] * scale)), shadow)


def _card_panel(img: Image.Image, box: tuple[int, int, int, int], pal: dict, scale: float,
                radius: float = 12) -> None:
    x0, y0, x1, y1 = box
    cw, ch = x1 - x0, y1 - y0
    img.paste(Image.new("RGB", (cw, ch), pal["panel"]), (x0, y0),
              rounded_mask(cw, ch, round(radius * scale)))


def _card(img: Image.Image, box: tuple[int, int, int, int], pal: dict, scale: float) -> None:
    """The floating Session-style card: soft shadow, then the rounded panel."""
    _card_shadow(img, box, pal, scale)
    _card_panel(img, box, pal, scale)


def render_hero(w: int, h: int, pal: dict, art: "Image.Image | None",
                card_box: "tuple[int, int, int, int] | None", scale: float) -> Image.Image:
    """The rounded hero panel: art (or the theme gradient), bottom shade, Session card."""
    if art is not None:
        body = _shade_bottom(cover(art, w, h))
    else:
        body = gradient(w, h, pal["hero"])
    if card_box is not None:
        _card(body, card_box, pal, scale)
    out = Image.new("RGB", (w, h), pal["bg"])
    out.paste(body, (0, 0), rounded_mask(w, h, round(16 * scale)))
    return out


# ---------------------------------------------------------------- widgets

class PillButton(tk.Canvas):
    """Rounded button drawn with Pillow (Tk has no rounded widgets).

    kind "primary" is the red PLAY pill; "secondary" is a quiet outlined pill.
    """

    def __init__(self, master, app, text: str, command, *, kind: str = "primary",
                 font: "tkfont.Font", height: int, padx: int, bg_role: str = "bg",
                 icon: "str | None" = None):
        super().__init__(master, highlightthickness=0, bd=0, cursor="hand2")
        self.app = app
        self._text = text
        self._icon = icon
        self._command = command
        self._kind = kind
        self._font = font
        self._height = height
        self._padx = padx
        self._bg_role = bg_role
        self._enabled = True
        self._hover = False
        self._down = False
        self._photo = None
        self.bind("<Enter>", lambda e: self._set(hover=True))
        self.bind("<Leave>", lambda e: self._set(hover=False, down=False))
        self.bind("<ButtonPress-1>", lambda e: self._set(down=True))
        self.bind("<ButtonRelease-1>", self._release)
        app.on_theme(self.redraw)

    @property
    def text(self) -> str:
        return self._text

    @property
    def enabled(self) -> bool:
        return self._enabled

    def set(self, text: "str | None" = None, enabled: "bool | None" = None,
            icon: "str | None" = None) -> None:
        """Change the label; `icon` ("play" or None) is replaced on every call."""
        self._icon = icon
        if text is not None:
            self._text = text
        if enabled is not None:
            self._enabled = enabled
            self.configure(cursor="hand2" if enabled else "arrow")
        self.redraw()

    def _set(self, hover=None, down=None) -> None:
        if hover is not None:
            self._hover = hover
        if down is not None:
            self._down = down
        self.redraw()

    def _release(self, event) -> None:
        inside = 0 <= event.x < self.winfo_width() and 0 <= event.y < self.winfo_height()
        was_down = self._down
        self._set(down=False)
        if was_down and inside and self._enabled:
            self._command()

    def redraw(self, pal: "dict | None" = None) -> None:
        pal = pal or self.app.palette
        bg = pal[self._bg_role]
        h = self._height
        text_w = self._font.measure(self._text)
        ascent = self._font.metrics("ascent")
        descent = self._font.metrics("descent")
        size = self._font.actual("size")
        em = -size if size < 0 else size * self.winfo_fpixels("1i") / 72
        icon_size = em * 0.72  # about the cap height
        icon_w = (icon_size + em * 0.4) if self._icon == "play" else 0
        w = round(text_w + icon_w) + 2 * self._padx
        outline, outline_w = None, 0
        if self._kind == "primary":
            if not self._enabled:
                fill, fg = mix(pal["accent"], bg, 0.62), mix(WHITE, bg, 0.88)
            elif self._down:
                fill, fg = pal["accent_down"], WHITE
            elif self._hover:
                fill, fg = pal["accent_hover"], WHITE
            else:
                fill, fg = pal["accent"], WHITE
        else:
            live = self._enabled
            fg = pal["text"] if live else mix(pal["text"], bg, 0.38)
            fill = mix(pal["text"], bg, 0.10 if self._hover and live else 0.0)
            if self._down and live:
                fill = mix(pal["text"], bg, 0.16)
            outline = mix(pal["text"], bg, 0.28 if live else 0.14)
            outline_w = max(1, round(self.app.scale * 1.5))
        img = pill_image(w, h, fill, bg, outline, outline_w)
        left = (w - text_w - icon_w) / 2
        baseline = h / 2 - (ascent + descent) / 2 + ascent
        if self._icon == "play":
            draw_play_icon(img, left + text_w + em * 0.4, baseline - icon_size, icon_size, fg)
        self._photo = ImageTk.PhotoImage(img, master=self)
        self.configure(width=w, height=h, bg=bg)
        self.delete("all")
        self.create_image(0, 0, anchor="nw", image=self._photo)
        self.create_text(left, h / 2, text=self._text, fill=fg, font=self._font, anchor="w")


# ---------------------------------------------------------------- page widgets

def text_label(master, app, text: str = "", *, font: str = "body", fg: str = "text",
               bg: str = "panel", **options) -> tk.Label:
    """A plain label (left-aligned unless options say otherwise) coloured from the palette."""
    options = {"anchor": "w", "justify": "left", "padx": 0, "pady": 0, "bd": 0, **options}
    label = tk.Label(master, text=text, font=app.fonts[font], **options)
    app.paint(label, bg=bg, fg=fg)
    return label


def shorten_middle(text: str, font: "tkfont.Font", width: int) -> str:
    """Cut the middle out of a long text (a folder path) so it fits in width pixels."""
    if width <= 0 or font.measure(text) <= width:
        return text
    lo, hi = 0, len(text)
    while lo < hi:  # the most characters that still fit around the "…"
        k = (lo + hi + 1) // 2
        head = k // 3
        if font.measure(text[:head] + "…" + text[len(text) - (k - head):]) <= width:
            lo = k
        else:
            hi = k - 1
    head = lo // 3
    return text[:head] + "…" + text[len(text) - (lo - head):]


def shorten_end(text: str, font: "tkfont.Font", width: int) -> str:
    """Cut the end off a text (a name) so it fits in width pixels."""
    if width <= 0 or font.measure(text) <= width:
        return text
    while text and font.measure(text + "…") > width:
        text = text[:-1]
    return text.rstrip() + "…"


def styled_menu(master, app) -> tk.Menu:
    """A popup menu in the theme's colours (like the Session card's)."""
    pal = app.palette
    return tk.Menu(master, tearoff=0, bg=pal["panel"], fg=pal["text"],
                   activebackground=pal["accent"], activeforeground=WHITE,
                   selectcolor=pal["text"], font=app.fonts["body"], bd=0,
                   activeborderwidth=app.px(4), relief="flat",
                   disabledforeground=mix(pal["text"], pal["panel"], 0.4))


class Flow(tk.Frame):
    """Lays its children out left to right and wraps them onto a new row when narrow
    (centred on each row's height, so a label lines up with a taller control)."""

    def __init__(self, master, gap_x: int, gap_y: int):
        super().__init__(master, height=1)
        self._gap_x, self._gap_y = gap_x, gap_y
        self.bind("<Configure>", lambda e: self.relayout())

    def relayout(self) -> None:
        width = max(1, self.winfo_width())
        rows: list[list] = [[]]
        x = 0
        for child in self.winfo_children():
            cw = child.winfo_reqwidth()
            if x and x + cw > width:
                rows.append([])
                x = 0
            rows[-1].append(child)
            x += cw + self._gap_x
        y = 0
        for row in rows:
            row_h = max((c.winfo_reqheight() for c in row), default=0)
            x = 0
            for child in row:
                child.place(x=x, y=y + (row_h - child.winfo_reqheight()) // 2)
                x += child.winfo_reqwidth() + self._gap_x
            y += row_h + self._gap_y
        height = max(1, y - self._gap_y)
        if int(self.cget("height")) != height:
            self.configure(height=height)


class Dropdown(tk.Canvas):
    """The current choice in a rounded box with a chevron; clicking calls command()."""

    def __init__(self, master, app, command, *, height: "int | None" = None,
                 bg_role: str = "panel"):
        super().__init__(master, highlightthickness=0, bd=0, height=height or app.px(36),
                         width=app.px(160), cursor="hand2")
        self.app = app
        self._command = command
        self._bg_role = bg_role
        self._text = ""
        self._enabled = True
        self._hover = False
        self._photo = None
        self.bind("<Enter>", lambda e: self._set_hover(True))
        self.bind("<Leave>", lambda e: self._set_hover(False))
        self.bind("<ButtonRelease-1>", self._click)
        self.bind("<Configure>", lambda e: self.redraw())
        app.on_theme(self.redraw)

    @property
    def text(self) -> str:
        return self._text

    @property
    def enabled(self) -> bool:
        return self._enabled

    def set(self, text: "str | None" = None, enabled: "bool | None" = None) -> None:
        if text is not None:
            self._text = text
        if enabled is not None:
            self._enabled = enabled
            self.configure(cursor="hand2" if enabled else "arrow")
        self.redraw()

    def _set_hover(self, on: bool) -> None:
        self._hover = on
        self.redraw()

    def _click(self, event) -> None:
        inside = 0 <= event.x < self.winfo_width() and 0 <= event.y < self.winfo_height()
        if inside and self._enabled:
            self._command()

    def redraw(self, pal: "dict | None" = None) -> None:
        pal = pal or self.app.palette
        bg = pal[self._bg_role]
        w = self.winfo_width() if self.winfo_width() > 1 else self.winfo_reqwidth()
        h = int(self.cget("height"))
        live = self._enabled
        hot = live and self._hover
        fill = mix(pal["text"], bg, 0.06 if live else 0.03)
        outline = pal["accent"] if hot else mix(pal["text"], bg, 0.16 if live else 0.10)
        img = rounded_image(w, h, self.app.px(8), fill, bg, outline,
                            max(1, round(self.app.scale)))
        self._photo = ImageTk.PhotoImage(img, master=self)
        inner = self.app.px(12)
        font = self.app.fonts["body_bold"]
        chevron_font = self.app.fonts["chevron"]
        mark = chevron(self.app.fonts)
        room = w - 3 * inner - chevron_font.measure(mark)
        self.configure(bg=bg)
        self.delete("all")
        self.create_image(0, 0, anchor="nw", image=self._photo)
        self.create_text(inner, h / 2, text=shorten_end(self._text, font, room), anchor="w",
                         font=font, fill=pal["text"] if live else mix(pal["text"], bg, 0.45))
        self.create_text(w - inner, h / 2, text=mark, anchor="e", font=chevron_font,
                         fill=pal["accent"] if hot else mix(pal["text"], bg, 0.55 if live else 0.3))


class CardBoard(tk.Canvas):
    """Floating cards like the Session card (rounded, soft shadow) laid out on the page.

    Cards sit in rows of equal-width columns; cards in one row share its height (with
    stretch=True their frames do too, so content packed at the bottom lines up; call
    schedule() after changing such a card). With fill=True the last row takes the rest of
    the height and (unless flat=False) is drawn as a flat panel, like the art panel on RACE.
    Put a card's content into the frame that add() returns.
    """

    def __init__(self, master, app, columns: int = 2, fill: bool = False,
                 stretch: bool = False, flat: "bool | None" = None):
        super().__init__(master, highlightthickness=0, bd=0, height=app.px(200))
        self.app = app
        self._columns = columns
        self._fill = fill
        self._stretch = stretch
        self._flat = fill if flat is None else flat
        self._side = app.px(24)  # the cards line up with the logo and the RACE art panel
        self._gap = app.px(16)
        self._pad = app.px(18)  # the Session card's inner padding
        self._bottom = 0 if self._flat else app.px(22)  # room for the last row's shadow
        self._cards: list[tuple[int, int, int, tk.Frame, int]] = []
        self._bg = self.create_image(0, 0, anchor="nw")
        self._photo = None
        self._job = None
        app.paint(self, bg="bg")
        self.bind("<Configure>", lambda e: self.schedule())
        app.on_theme(lambda pal: self.schedule(0))

    def add(self, row: int, col: int = 0, span: int = 1, title: "str | None" = None) -> tk.Frame:
        body = tk.Frame(self)
        self.app.paint(body, bg="panel")
        if title:
            text_label(body, self.app, title, font="card_title").pack(
                fill="x", pady=(0, self.app.px(12)))
        item = self.create_window(0, 0, anchor="nw", window=body)
        body.bind("<Configure>", lambda e: self.schedule())
        self._cards.append((row, col, span, body, item))
        self.schedule()
        return body

    def schedule(self, delay: int = 10) -> None:
        if self._job is not None:
            self.app.after_cancel(self._job)
        self._job = self.app.after(delay, self._layout)

    def _layout(self) -> None:
        self._job = None
        app, pal = self.app, self.app.palette
        width, height = self.winfo_width(), self.winfo_height()
        if width < 40 or not self._cards:
            return
        if self._stretch:
            self.update_idletasks()  # wrapped labels report their new height first
        cols = self._columns
        col_w = (width - 2 * self._side - (cols - 1) * self._gap) / cols
        rows = sorted({card[0] for card in self._cards})
        y = 0
        boxes = []
        for i, row in enumerate(rows):
            cards = [c for c in self._cards if c[0] == row]
            last_fill = self._fill and i == len(rows) - 1
            fixed = last_fill or self._stretch
            h = max(c[3].winfo_reqheight() for c in cards) + 2 * self._pad
            if last_fill:
                h = max(h, height - y - self._bottom)
            for _, col, span, body, item in cards:
                x0 = round(self._side + col * (col_w + self._gap))
                x1 = round(self._side + (col + span) * (col_w + self._gap) - self._gap)
                self.coords(item, x0 + self._pad, y + self._pad)
                self.itemconfigure(item, width=max(1, x1 - x0 - 2 * self._pad),
                                   height=max(1, h - 2 * self._pad) if fixed else 0)
                boxes.append((x0, y, x1, y + h))
            y += h + self._gap
        if not self._fill:
            wanted = y - self._gap + self._bottom
            if int(self.cget("height")) != wanted:
                self.configure(height=wanted)
        image = Image.new("RGB", (width, max(1, height)), pal["bg"])
        if self._flat:
            for box in boxes:
                _card_panel(image, box, pal, app.scale, radius=16)
        else:
            for box in boxes:
                _card_shadow(image, box, pal, app.scale)
            for box in boxes:
                _card_panel(image, box, pal, app.scale)
        self._photo = ImageTk.PhotoImage(image, master=self)
        self.itemconfigure(self._bg, image=self._photo)


class Segmented(tk.Canvas):
    """Choices in one rounded track; the chosen one is a red pill (a small PLAY).

    Clicking calls command(value); the page decides and calls set() to show the result.
    """

    def __init__(self, master, app, options, command, *, bg_role: str = "panel",
                 min_label: str = "", height: "int | None" = None, pad: "int | None" = None):
        super().__init__(master, highlightthickness=0, bd=0)
        self.app = app
        self._choices = list(options)
        self._command = command
        self._bg_role = bg_role
        self._font = app.fonts["button"]
        self._height = height or app.px(32)
        self._pad = app.px(14) if pad is None else pad
        self._min_label = min_label
        self._value = None
        self._disabled: set = set()
        self._hover: "int | None" = None
        self._spans: list[tuple[float, float]] = []
        self._photo = None
        self.bind("<Motion>", self._motion)
        self.bind("<Leave>", lambda e: self._set_hover(None))
        self.bind("<ButtonRelease-1>", self._click)
        app.on_theme(self.redraw)

    @property
    def value(self):
        return self._value

    def set(self, value) -> None:
        if value != self._value:
            self._value = value
            self.redraw()

    def set_enabled(self, value, enabled: bool) -> None:
        was = value not in self._disabled
        if enabled:
            self._disabled.discard(value)
        else:
            self._disabled.add(value)
        if was != enabled:
            self.redraw()

    def is_enabled(self, value) -> bool:
        return value not in self._disabled

    def _index_at(self, x: int) -> "int | None":
        for i, (x0, x1) in enumerate(self._spans):
            if x0 <= x < x1:
                return i
        return None

    def _motion(self, event) -> None:
        i = self._index_at(event.x)
        usable = i is not None and self._choices[i][0] not in self._disabled
        self.configure(cursor="hand2" if usable else "arrow")
        self._set_hover(i if usable else None)

    def _set_hover(self, i: "int | None") -> None:
        if i != self._hover:
            self._hover = i
            self.redraw()

    def _click(self, event) -> None:
        i = self._index_at(event.x)
        if i is None or not 0 <= event.y < self.winfo_height():
            return
        value = self._choices[i][0]
        if value not in self._disabled:
            self._command(value)

    def redraw(self, pal: "dict | None" = None) -> None:
        pal = pal or self.app.palette
        bg = pal[self._bg_role]
        h = self._height
        inset = max(2, self.app.px(3))
        pad = self._pad
        least = self._font.measure(self._min_label) if self._min_label else 0
        widths = [max(self._font.measure(label), least) + 2 * pad for _, label in self._choices]
        w = sum(widths) + 2 * inset
        track = mix(pal["text"], bg, 0.08)
        img = rounded_image(w, h, h // 2, track, bg)
        seg_h = h - 2 * inset
        texts = []
        self._spans = []
        x = inset
        for i, ((value, label), sw) in enumerate(zip(self._choices, widths)):
            disabled = value in self._disabled
            if value == self._value:
                fill = mix(pal["accent"], track, 0.5) if disabled else pal["accent"]
                img.paste(rounded_image(sw, seg_h, seg_h // 2, fill, track), (x, inset))
                fg = mix(WHITE, fill, 0.8) if disabled else WHITE
            elif disabled:
                fg = mix(pal["text"], track, 0.3)
            elif i == self._hover:
                fg = pal["text"]
            else:
                fg = mix(pal["text"], track, 0.68)
            texts.append((x + sw / 2, fg, label))
            self._spans.append((x, x + sw))
            x += sw
        self._photo = ImageTk.PhotoImage(img, master=self)
        self.configure(width=w, height=h, bg=bg)
        self.delete("all")
        self.create_image(0, 0, anchor="nw", image=self._photo)
        for cx, fg, label in texts:
            self.create_text(cx, h / 2, text=label, fill=fg, font=self._font, anchor="center")


class Field(tk.Canvas):
    """Rounded text box: a borderless Entry inside, or (editable=False) a read-only text
    that loses its middle when it is too long, like a folder path."""

    def __init__(self, master, app, *, width: int, editable: bool = True,
                 bg_role: str = "panel", justify: str = "left"):
        super().__init__(master, highlightthickness=0, bd=0, width=width, height=app.px(32))
        self.app = app
        self._bg_role = bg_role
        self._focused = False
        self._text = ""
        self._font = app.fonts["body"]
        self._photo = None
        self._bg_item = self.create_image(0, 0, anchor="nw")
        if editable:
            self.entry: "tk.Entry | None" = tk.Entry(
                self, relief="flat", bd=0, highlightthickness=0, font=self._font,
                justify=justify, width=1)
            self._item = self.create_window(0, 0, anchor="w", window=self.entry)
            self.entry.bind("<FocusIn>", lambda e: self._focus(True), add="+")
            self.entry.bind("<FocusOut>", lambda e: self._focus(False), add="+")
            self.bind("<Button-1>", lambda e: self.entry.focus_set())
        else:
            self.entry = None
            self._item = self.create_text(0, 0, anchor="w", font=self._font)
        self.bind("<Configure>", lambda e: self.redraw())
        app.on_theme(self.redraw)

    def get(self) -> str:
        return self.entry.get() if self.entry is not None else self._text

    def set(self, text: str) -> None:
        if self.entry is not None:
            self.entry.delete(0, "end")
            self.entry.insert(0, text)
        else:
            self._text = text
            self.redraw()

    def _focus(self, on: bool) -> None:
        self._focused = on
        self.redraw()

    def redraw(self, pal: "dict | None" = None) -> None:
        pal = pal or self.app.palette
        bg = pal[self._bg_role]
        w = self.winfo_width() if self.winfo_width() > 1 else self.winfo_reqwidth()
        h = int(self.cget("height"))
        fill = mix(pal["text"], bg, 0.06)
        outline = pal["accent"] if self._focused else mix(pal["text"], bg, 0.16)
        img = rounded_image(w, h, self.app.px(8), fill, bg, outline,
                            max(1, round(self.app.scale)))
        self._photo = ImageTk.PhotoImage(img, master=self)
        self.configure(bg=bg)
        self.itemconfigure(self._bg_item, image=self._photo)
        inner = self.app.px(11)
        self.coords(self._item, inner, h / 2)
        if self.entry is not None:
            self.itemconfigure(self._item, width=max(1, w - 2 * inner))
            self.entry.configure(bg=fill, fg=pal["text"], insertbackground=pal["text"],
                                 selectbackground=pal["accent"], selectforeground=WHITE,
                                 readonlybackground=fill, disabledbackground=fill)
        else:
            self.itemconfigure(self._item, fill=pal["text"],
                               text=shorten_middle(self._text, self._font, w - 2 * inner))


class Slider(tk.Canvas):
    """Horizontal slider: on_change(value) while it moves, on_commit(value) when let go."""

    def __init__(self, master, app, *, length: int, from_: float = 0.0, to: float = 1.0,
                 on_change=None, on_commit=None, bg_role: str = "panel"):
        super().__init__(master, highlightthickness=0, bd=0, width=length,
                         height=app.px(24), cursor="hand2")
        self.app = app
        self._from, self._to = from_, to
        self._value = from_
        self._on_change, self._on_commit = on_change, on_commit
        self._bg_role = bg_role
        self._enabled = True
        self._dragging = False
        self._photo = None
        self.bind("<ButtonPress-1>", self._press)
        self.bind("<B1-Motion>", self._move)
        self.bind("<ButtonRelease-1>", self._let_go)
        self.bind("<Configure>", lambda e: self.redraw())
        app.on_theme(self.redraw)

    @property
    def value(self) -> float:
        return self._value

    def set(self, value: float) -> None:
        """Show a value without calling back."""
        self._value = min(self._to, max(self._from, float(value)))
        self.redraw()

    def set_enabled(self, on: bool) -> None:
        if on != self._enabled:
            self._enabled = on
            self.configure(cursor="hand2" if on else "arrow")
            self.redraw()

    def drag_to(self, value: float) -> None:
        self.set(value)
        if self._on_change is not None:
            self._on_change(self._value)

    def release(self) -> None:
        if self._on_commit is not None:
            self._on_commit(self._value)

    def _radius(self) -> int:
        return self.app.px(8)

    def _width(self) -> int:
        return self.winfo_width() if self.winfo_width() > 1 else self.winfo_reqwidth()

    def _value_at(self, x: int) -> float:
        r = self._radius()
        t = (x - r) / max(1, self._width() - 2 * r)
        return self._from + min(1.0, max(0.0, t)) * (self._to - self._from)

    def _press(self, event) -> None:
        if self._enabled:
            self._dragging = True
            self.drag_to(self._value_at(event.x))

    def _move(self, event) -> None:
        if self._dragging:
            self.drag_to(self._value_at(event.x))

    def _let_go(self, event) -> None:
        if self._dragging:
            self._dragging = False
            self.release()

    def redraw(self, pal: "dict | None" = None) -> None:
        pal = pal or self.app.palette
        bg = pal[self._bg_role]
        w, h = self._width(), int(self.cget("height"))
        ss = _SUPERSAMPLE
        r = self._radius()
        t = (self._value - self._from) / ((self._to - self._from) or 1)
        cx, cy = r + t * (w - 2 * r), h / 2
        thick = self.app.px(4)
        track = mix(pal["text"], bg, 0.14)
        done = pal["accent"] if self._enabled else mix(pal["text"], bg, 0.32)
        knob = WHITE if self._enabled else mix(pal["text"], bg, 0.42)
        ring = mix(pal["text"], bg, 0.22)
        big = Image.new("RGB", (w * ss, h * ss), bg)
        d = ImageDraw.Draw(big)
        top, bottom = (cy - thick / 2) * ss, (cy + thick / 2) * ss
        d.rounded_rectangle((r * ss, top, (w - r) * ss, bottom), radius=thick * ss / 2,
                            fill=track)
        d.rounded_rectangle((r * ss, top, max(r + thick, cx) * ss, bottom),
                            radius=thick * ss / 2, fill=done)
        d.ellipse(((cx - r) * ss, (cy - r) * ss, (cx + r) * ss, (cy + r) * ss), fill=ring)
        edge = max(1, round(self.app.scale)) * ss
        d.ellipse(((cx - r) * ss + edge, (cy - r) * ss + edge, (cx + r) * ss - edge,
                   (cy + r) * ss - edge), fill=knob)
        self._photo = ImageTk.PhotoImage(big.resize((w, h), Image.LANCZOS), master=self)
        self.configure(bg=bg)
        self.delete("all")
        self.create_image(0, 0, anchor="nw", image=self._photo)


class Chip(tk.Canvas):
    """A small rounded key cap showing a binding; click it to change the binding.

    state "normal" (clickable), "waiting" (red: press a key now) or "static" (plain text).
    """

    def __init__(self, master, app, command=None, *, width: int, height: "int | None" = None,
                 bg_role: str = "panel"):
        super().__init__(master, highlightthickness=0, bd=0, width=width,
                         height=height or app.px(26))
        self.app = app
        self._command = command
        self._bg_role = bg_role
        self._text = ""
        self._state = "normal"
        self._hover = False
        self._photo = None
        self.bind("<Enter>", lambda e: self._set_hover(True))
        self.bind("<Leave>", lambda e: self._set_hover(False))
        self.bind("<ButtonRelease-1>", self._click)
        app.on_theme(self.redraw)

    @property
    def text(self) -> str:
        return self._text

    def set(self, text: "str | None" = None, state: "str | None" = None) -> None:
        if text is not None:
            self._text = text
        if state is not None:
            self._state = state
        clickable = self._state != "static" and self._command is not None
        self.configure(cursor="hand2" if clickable else "arrow")
        self.redraw()

    def _set_hover(self, on: bool) -> None:
        self._hover = on
        self.redraw()

    def _click(self, event) -> None:
        inside = 0 <= event.x < self.winfo_width() and 0 <= event.y < self.winfo_height()
        if inside and self._state != "static" and self._command is not None:
            self._command()

    def redraw(self, pal: "dict | None" = None) -> None:
        pal = pal or self.app.palette
        bg = pal[self._bg_role]
        w, h = int(self.cget("width")), int(self.cget("height"))
        font = self.app.fonts["body_bold" if len(self._text) == 1 else "small_bold"]
        line = max(1, round(self.app.scale))
        if self._state == "waiting":
            fill, outline, fg = pal["accent"], pal["accent"], WHITE
        elif self._state == "static":
            fill, outline, fg = bg, None, pal["muted"]
            font = self.app.fonts["small"]
        else:
            hover = self._hover and self._command is not None
            fill = mix(pal["text"], bg, 0.10 if hover else 0.06)
            outline = pal["accent"] if hover else mix(pal["text"], bg, 0.17)
            fg = pal["text"]
        img = rounded_image(w, h, self.app.px(6), fill, bg, outline, line if outline else 0)
        self._photo = ImageTk.PhotoImage(img, master=self)
        text = self._text
        room = w - 2 * self.app.px(8)
        if font.measure(text) > room:
            while text and font.measure(text + "…") > room:
                text = text[:-1]
            text += "…"
        self.configure(bg=bg)
        self.delete("all")
        self.create_image(0, 0, anchor="nw", image=self._photo)
        self.create_text(w / 2, h / 2, text=text, fill=fg, font=font, anchor="center")


class ThinScrollbar(tk.Canvas):
    """A slim rounded scrollbar thumb (ttk's is a grey block in the clam theme)."""

    def __init__(self, master, app, command, *, bg_role: str = "panel"):
        super().__init__(master, highlightthickness=0, bd=0, width=app.px(12))
        self.app = app
        self._command = command
        self._bg_role = bg_role
        self._first, self._last = 0.0, 1.0
        self._grab: "float | None" = None
        self._hover = False
        self._photo = None
        self.bind("<Enter>", lambda e: self._set_hover(True))
        self.bind("<Leave>", lambda e: self._set_hover(False))
        self.bind("<ButtonPress-1>", self._press)
        self.bind("<B1-Motion>", self._move)
        self.bind("<ButtonRelease-1>", self._let_go)
        self.bind("<Configure>", lambda e: self.redraw())
        app.on_theme(self.redraw)

    def set(self, first, last) -> None:
        self._first, self._last = float(first), float(last)
        self.redraw()

    def _set_hover(self, on: bool) -> None:
        self._hover = on
        self.redraw()

    def _thumb(self) -> tuple[float, float]:
        h = max(1, self.winfo_height())
        top, bottom = self._first * h, self._last * h
        least = self.app.px(28)
        if bottom - top < least:
            top = min(top, h - least)
            bottom = top + least
        return top, bottom

    def _press(self, event) -> None:
        if self._last - self._first >= 1.0:
            return
        top, bottom = self._thumb()
        if top <= event.y <= bottom:
            self._grab = event.y - top
            self.redraw()
        else:
            self._command("scroll", 1 if event.y > bottom else -1, "pages")

    def _move(self, event) -> None:
        if self._grab is not None:
            self._command("moveto", (event.y - self._grab) / max(1, self.winfo_height()))

    def _let_go(self, event) -> None:
        self._grab = None
        self.redraw()

    def redraw(self, pal: "dict | None" = None) -> None:
        pal = pal or self.app.palette
        bg = pal[self._bg_role]
        self.configure(bg=bg)
        self.delete("all")
        if self._last - self._first >= 1.0:
            return
        w = max(1, self.winfo_width())
        top, bottom = self._thumb()
        active = self._hover or self._grab is not None
        tw = self.app.px(8 if active else 6)
        th = max(tw, round(bottom - top))
        color = mix(pal["text"], bg, 0.42 if active else 0.24)
        self._photo = ImageTk.PhotoImage(rounded_image(tw, th, tw // 2, color, bg), master=self)
        self.create_image((w - tw) / 2, top, anchor="nw", image=self._photo)
