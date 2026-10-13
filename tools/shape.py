"""Render shaped (nikud-positioned) Hebrew words with HarfBuzz + FreeType."""
import uharfbuzz as hb
import freetype
import numpy as np


class Shaper:
    def __init__(self, path, px):
        self.px = px
        blob = hb.Blob.from_file_path(path)
        self.hbfont = hb.Font(hb.Face(blob))
        self.upem = self.hbfont.face.upem
        self.ft = freetype.Face(path)
        self.ft.set_pixel_sizes(0, px)
        self.ascent = self.ft.size.ascender / 64
        self.descent = -self.ft.size.descender / 64

    def render(self, text):
        """Return (L-mode numpy array, ink 0..255 where 255 = black) with baseline at ascent."""
        buf = hb.Buffer()
        buf.add_str(text)
        buf.guess_segment_properties()
        hb.shape(self.hbfont, buf, {})
        scale = self.px / self.upem
        pen = 0.0
        placed = []
        for info, pos in zip(buf.glyph_infos, buf.glyph_positions):
            placed.append((info.codepoint, pen + pos.x_offset * scale, pos.y_offset * scale))
            pen += pos.x_advance * scale
        width = int(np.ceil(pen)) + 8
        height = int(np.ceil(self.ascent + self.descent)) + 8
        canvas = np.zeros((height, width), dtype=np.float32)
        base = int(round(self.ascent)) + 4
        for gid, x, y in placed:
            self.ft.load_glyph(gid, freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_NORMAL)
            g = self.ft.glyph
            bm = g.bitmap
            if bm.width == 0:
                continue
            arr = np.array(bm.buffer, dtype=np.float32).reshape(bm.rows, bm.pitch)[:, :bm.width]
            x0 = int(round(4 + x + g.bitmap_left))
            y0 = int(round(base - y - g.bitmap_top))
            ys, xs = max(0, -y0), max(0, -x0)
            y1, x1 = min(height, y0 + bm.rows), min(width, x0 + bm.width)
            canvas[y0 + ys:y1, x0 + xs:x1] = np.maximum(canvas[y0 + ys:y1, x0 + xs:x1], arr[ys:y1 - y0, xs:x1 - x0])
        return canvas, int(round(pen)), base
