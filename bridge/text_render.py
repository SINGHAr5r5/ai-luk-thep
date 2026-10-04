"""Render text to an 8-bit alpha bitmap for the board's screen.

The ESP32 has no complex-text engine, so Thai (stacked vowels and tone marks) is shaped here with
Pillow + libraqm and sent to the board as pixels. The board only has to draw an A8 image.
"""
import logging
import unicodedata
from pathlib import Path

log = logging.getLogger("voice-bridge")
HERE = Path(__file__).resolve().parent

LEADING_VOWELS = set("เแโใไ")   # written before the consonant they belong to; never end a line on one
TRAILING_MARKS = set("ะาำๆฯ")    # never start a line (they complete the previous syllable)


def _clusters(text: str) -> list[str]:
    """Split into unbreakable units: base char + its combining marks, leading vowels glued to the next unit."""
    units: list[str] = []
    pending = ""
    for ch in text:
        if units and (unicodedata.category(ch) in ("Mn", "Mc", "Me") or ch in TRAILING_MARKS):
            units[-1] += ch
            continue
        if ch in LEADING_VOWELS:
            pending += ch
            continue
        units.append(pending + ch)
        pending = ""
    if pending:
        units.append(pending)
    return units


class TextRenderer:
    def __init__(self, width: int = 230, size: int = 20, line_height: int = 28,
                 font_path: str | Path | None = None):
        from PIL import ImageFont, features

        self.width, self.line_height = width, line_height
        self.margin = 4                      # ink never touches the image edge (glyphs overhang their advance)
        path = Path(font_path) if font_path else HERE / "fonts" / "Sarabun-Regular.ttf"
        engine = ImageFont.Layout.RAQM if features.check("raqm") else ImageFont.Layout.BASIC
        if engine == ImageFont.Layout.BASIC:
            log.warning("libraqm not available: Thai tone marks and vowels will be misplaced on screen")
        self.font = ImageFont.truetype(str(path), size, layout_engine=engine)

    def _wrap(self, text: str) -> list[str]:
        lines: list[str] = []
        for para in text.replace("\r", "").split("\n"):
            cur = ""
            for unit in _clusters(para):
                if self.font.getlength(cur + unit) <= self.width - 2 * self.margin or not cur:
                    cur += unit
                    continue
                # prefer breaking at the last space if it is not too far back
                cut = cur.rfind(" ")
                if cut > len(cur) * 0.5:
                    lines.append(cur[:cut].rstrip())
                    cur = cur[cut:].lstrip() + unit
                else:
                    lines.append(cur)
                    cur = unit.lstrip() if unit.isspace() else unit
            lines.append(cur)
        return [ln for ln in lines if ln.strip()] or [""]

    def render(self, text: str, max_lines: int, keep: str = "head") -> tuple[int, int, bytes]:
        """Return (width, height, A8 bytes). If the text is longer than max_lines keep its head or tail."""
        from PIL import Image, ImageDraw

        lines = self._wrap(text.strip())
        if len(lines) > max_lines:
            if keep == "tail":
                lines = ["…" + lines[-max_lines].lstrip()] + lines[-max_lines + 1:]
            else:
                lines = lines[:max_lines - 1] + [lines[max_lines - 1].rstrip() + "…"]
        h = len(lines) * self.line_height + 4
        img = Image.new("L", (self.width, h), 0)
        d = ImageDraw.Draw(img)
        for i, ln in enumerate(lines):
            d.text((self.margin, 2 + i * self.line_height), ln, font=self.font, fill=255)
        return self.width, h, img.tobytes()
