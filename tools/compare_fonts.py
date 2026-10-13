"""Side-by-side preview of candidate fonts at the size make_words.py would pick."""
import re, sys
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw
from shape import Shaper

root = Path(__file__).resolve().parent.parent
src = (root / 'HebrewClock' / 'HebrewClock.ino').read_text(encoding='utf-8')
lit = lambda n: re.findall(r'"([^"]*)"', re.search(r'\b' + n + r'\[\]\s*=\s*\{(.*?)\};', src, re.S).group(1))
units, teens, tens, hours = (lit(n) for n in ('unitsV', 'teensV', 'tensV', 'hours'))
def mw(m):
    if m < 10: return units[m]
    if m < 20: return teens[m - 10]
    return tens[m // 10] + (' ' + units[m % 10] if m % 10 else '')
lines = [mw(m) for m in range(1, 60)] + hours
FONTS = [('gisha.ttf', 'Gisha'), ('gishabd.ttf', 'Gisha Bold'), ('david.ttf', 'David'), ('davidbd.ttf', 'David Bold')]
L1, L2 = hours[11], mw(50)

def render_line(sh, line):
    parts = [sh.render(w) for w in line.split()]
    sp = round(sh.px * 0.28)
    total = sum(p[1] for p in parts) + sp * (len(parts) - 1)
    return parts, sp, total

tiles = []
for f, name in FONTS:
    for px in range(140, 40, -2):
        sh = Shaper('C:/Windows/Fonts/' + f, px)
        if max(render_line(sh, l)[2] for l in lines) <= 900: break
    img = Image.new('L', (960, 540), 255)
    h = sh.render(L1)[0].shape[0]
    top1 = (540 - 2 * h) // 2 + 10
    for line, top in ((L1, top1), (L2, top1 + h)):
        parts, sp, total = render_line(sh, line)
        right = (960 + total) // 2
        for canvas, adv, _ in parts:
            right -= adv
            bits = canvas >= 128
            img.paste(0, (right - 4, top), Image.fromarray((bits * 255).astype('uint8')))
            right -= sp
    d = ImageDraw.Draw(img); d.rectangle((0, 0, 959, 539), outline=0, width=3)
    d.text((14, 10), f'{name} ({px}px)', fill=0, font_size=28)
    tiles.append(img)
sheet = Image.new('L', (1940, 1100), 160)
for i, t in enumerate(tiles): sheet.paste(t, ((i % 2) * 980, (i // 2) * 560))
sheet.save(root / 'docs' / 'fonts_compare.png'); print('ok')
