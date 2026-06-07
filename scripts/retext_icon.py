#!/usr/bin/env python3
"""Take the ORIGINAL CMU icons and replace only the 'CMU' wordmark with
'CTM', preserving the bridge art, colours and layout. Reads the backed-up
originals, writes into the active ctm-bridge-test-webos tree."""
import os
from PIL import Image, ImageDraw, ImageFont

SRC = r"D:\Work\CMU\ctm-bridge-test-webos"          # holds restored originals
DST = r"D:\Work\CMU\ctm-bridge-test-webos"          # active (overwrite in place)
FONTS = [r"C:\Windows\Fonts\arialbd.ttf",
         r"C:\Windows\Fonts\segoeuib.ttf",
         r"C:\Windows\Fonts\arial.ttf"]


def is_gold(px):
    r, g, b, a = px
    return a > 40 and r > 110 and g > 80 and (r - b) > 25 and r >= b


def load_font(px):
    for p in FONTS:
        if os.path.exists(p):
            try:
                return ImageFont.truetype(p, px)
            except Exception:
                pass
    return ImageFont.load_default()


def retext(name):
    im = Image.open(os.path.join(SRC, name)).convert("RGBA")
    W, H = im.size
    px = im.load()

    # Per-row gold counts, then split into vertical runs separated by
    # blank rows. The wordmark is the BOTTOM-most run (above it sit the
    # bridge cable and the deck line as separate runs).
    rowc = [sum(1 for x in range(W) if is_gold(px[x, y])) for y in range(H)]
    runs = []
    y = 0
    while y < H:
        if rowc[y] > 0:
            s = y
            while y < H and rowc[y] > 0:
                y += 1
            runs.append((s, y - 1))
        else:
            y += 1
    if not runs:
        print("no wordmark found in", name); return
    ry0, ry1 = runs[-1]            # bottom-most run = the text
    # x-extent within those rows
    xs, golds = [], []
    for yy in range(ry0, ry1 + 1):
        for x in range(W):
            p = px[x, yy]
            if is_gold(p):
                xs.append(x); golds.append(p)
    x0, x1, y0, y1 = min(xs), max(xs), ry0, ry1
    cx = (x0 + x1) / 2.0
    cap_h = y1 - y0 + 1
    # Sample the gold from the thickest strokes in the whole image (the
    # bridge) — its core pixels are fully saturated, unlike the thin
    # anti-aliased wordmark glyphs. Guarantees the new text matches.
    allgold = [px[x, y] for y in range(H) for x in range(W) if is_gold(px[x, y])]
    gold = max(allgold, key=lambda p: (p[0] - p[2]))
    gold = (gold[0], gold[1], gold[2], 255)
    text_color = (245, 245, 245, 255)   # white wordmark (bridge stays gold)

    # Local background = median of the OPAQUE non-gold pixels within (and
    # just around) the wordmark band — matches whatever sits behind the
    # letters, instead of a corner pixel that may be a different shade or
    # transparent (which produced a visible patch).
    pad = max(2, int(W * 0.04))
    band = []
    for yy in range(max(0, y0 - pad), min(H, y1 + pad + 1)):
        for xx in range(max(0, x0 - pad), min(W, x1 + pad + 1)):
            p = px[xx, yy]
            if p[3] > 200 and not is_gold(p):
                band.append(p)
    if band:
        band.sort(key=lambda p: p[0] + p[1] + p[2])
        bg = band[len(band) // 2]
    else:
        bg = px[3, 3]

    d = ImageDraw.Draw(im)
    # Erase the whole wordmark band with the LOCAL background colour
    # (computed above). The region behind the text is flat, so a solid
    # fill is seamless — and it fully removes anti-aliased glyph residue
    # that a per-pixel gold test would miss.
    d.rectangle([x0 - pad, y0 - pad, x1 + pad, y1 + pad], fill=bg)

    # find a font size whose cap height matches the original
    text = "CTM"
    size = cap_h
    for _ in range(40):
        f = load_font(int(size))
        b = d.textbbox((0, 0), text, font=f)
        h = b[3] - b[1]
        if abs(h - cap_h) <= 1 or size > cap_h * 3:
            break
        size += 1 if h < cap_h else -1
    f = load_font(int(size))
    b = d.textbbox((0, 0), text, font=f)
    tw, th = b[2] - b[0], b[3] - b[1]
    tx = cx - tw / 2.0 - b[0]
    ty = y0 - b[1]
    d.text((tx, ty), text, font=f, fill=text_color)

    out = os.path.join(DST, name)
    im.save(out)
    print("wrote", out, "size", im.size, "gold", gold, "cap_h", cap_h, "font", int(size))


for n in ["icon.png", "icon_large.png"]:
    retext(n)
