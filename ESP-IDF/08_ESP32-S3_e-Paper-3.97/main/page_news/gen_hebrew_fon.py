#!/usr/bin/env python3
"""Generate the Hebrew glyph table for the e-paper news page.

Shipped: New Peninim MT at 40pt in a 24x41 cell.

Face chosen by rendering a real Ynet headline through every Hebrew-capable font
on the machine at the true 24x41 bitmap and comparing actual pixels. New Peninim
and Raanana fit 40pt where Arial Unicode manages 34 and SF Hebrew only 32 - their
letterforms are more compact, so more point size fits before anything clips.
SF Hebrew also has the thinnest strokes of the set, which is the wrong trade on
1-bit e-ink where there is no antialiasing to carry a thin stem.

File layout (one blob, read by hebrew.inc):
    [0 .. N*GLYPH_BYTES)          glyph bitmaps, 3 bytes/row * 41 rows
    [N*GLYPH_BYTES .. +N)         one advance-width byte per glyph

Glyphs are LEFT-NORMALISED: each is shifted so its ink starts at x=0, and its
real ink width is recorded. That is what lets the renderer advance
proportionally instead of giving narrow letters like vav and yod the same 24px
slot as shin - which is what made the first version read as stretched out.

Bit 0 = ink, matching the vendor's own .FON files (verified by decoding their
'A' and 'H' before generating anything).

Regenerate:
    HE_W=24 HE_H=41 HE_FONT=/System/Library/Fonts/Supplemental/NewPeninimMT.ttc \
        python3 gen_hebrew_fon.py font24HE.FON 40 32
"""
import os
import sys
from PIL import Image, ImageDraw, ImageFont

WIDTH = int(os.environ.get("HE_W", 24))
HEIGHT = int(os.environ.get("HE_H", 41))
BASE, LAST = 0x05D0, 0x05EA
NGLYPH = LAST - BASE + 1
ROWBYTES = WIDTH // 8
GLYPH_BYTES = ROWBYTES * HEIGHT

FONT_PATH = os.environ.get("HE_FONT", "/System/Library/Fonts/Supplemental/NewPeninimMT.ttc")
FONT_INDEX = int(os.environ.get("HE_FONT_IDX", 0))
OUT = sys.argv[1] if len(sys.argv) > 1 else "font24HE.FON"
PT = int(sys.argv[2]) if len(sys.argv) > 2 else 40
BASELINE = int(sys.argv[3]) if len(sys.argv) > 3 else 32


def raster(cp, font):
    """Draw one codepoint into a WIDTH x HEIGHT 1-bit image. 0 = ink."""
    img = Image.new("1", (WIDTH, HEIGHT), 1)
    d = ImageDraw.Draw(img)
    try:
        d.text((1, BASELINE), chr(cp), font=font, fill=0, anchor="ls")
    except Exception:
        d.text((1, BASELINE - PT), chr(cp), font=font, fill=0)
    return img


def normalise(img):
    """Shift ink to x=0 and report its width. Returns (bytes, width)."""
    px = img.load()
    cols = [x for x in range(WIDTH) for y in range(HEIGHT) if not (px[x, y] & 1)]
    if not cols:
        return bytes([0xFF] * GLYPH_BYTES), 0
    minx, maxx = min(cols), max(cols)

    shifted = Image.new("1", (WIDTH, HEIGHT), 1)
    sp = shifted.load()
    for y in range(HEIGHT):
        for x in range(minx, maxx + 1):
            if not (px[x, y] & 1):
                sp[x - minx, y] = 0

    out = bytearray()
    for y in range(HEIGHT):
        for b in range(ROWBYTES):
            byte = 0
            for bit in range(8):
                byte = (byte << 1) | (sp[b * 8 + bit, y] & 1)
            out.append(byte)
    return bytes(out), (maxx - minx + 1)


def main():
    font = ImageFont.truetype(FONT_PATH, PT, index=FONT_INDEX)
    glyphs, widths, blank, clipped = bytearray(), bytearray(), [], []

    for cp in range(BASE, LAST + 1):
        img = raster(cp, font)
        px = img.load()
        ink = [(y, x) for y in range(HEIGHT) for x in range(WIDTH) if not (px[x, y] & 1)]
        if not ink:
            blank.append(chr(cp))
        elif (min(r for r, _ in ink) == 0 or max(r for r, _ in ink) == HEIGHT - 1
              or max(c for _, c in ink) == WIDTH - 1):
            clipped.append(chr(cp))

        g, w = normalise(img)
        glyphs += g
        widths.append(min(w, WIDTH))

    if blank:
        print(f"  ERROR: blank glyphs: {''.join(blank)}")
        return 1
    if clipped:
        print(f"  ERROR: clipped glyphs at {PT}pt: {''.join(clipped)} - lower the point size")
        return 1

    data = bytes(glyphs) + bytes(widths)
    open(OUT, "wb").write(data)
    print(f"wrote {OUT}: {len(data)}B = {NGLYPH} glyphs x {GLYPH_BYTES}B + {NGLYPH} width bytes")
    print(f"  face={os.path.basename(FONT_PATH)} idx={FONT_INDEX} pt={PT}")
    print(f"  advance widths: min={min(widths)} max={max(widths)} "
          f"(fixed-cell would be {WIDTH} for every letter)")

    for cp in (0x05D0, 0x05D5):     # alef (wide), vav (narrow)
        i = cp - BASE
        g = data[i * GLYPH_BYTES:(i + 1) * GLYPH_BYTES]
        print(f"--- U+{cp:04X} {chr(cp)}  advance={widths[i]}px ---")
        for y in range(HEIGHT):
            row = "".join(f"{g[y*ROWBYTES+b]:08b}" for b in range(ROWBYTES))
            row = row.replace("0", "#").replace("1", ".")
            if "#" in row:
                print("   " + row)
    return 0


sys.exit(main())
