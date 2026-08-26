// Hebrew rendering for the news page.
//
// Kept here rather than in epaper_lib on purpose: it needs only Paint_SetPixel,
// so it costs the shared driver nothing. Promote it if a second page wants it.
//
// Blob layout, produced by gen_hebrew_fon.py (New Peninim MT, 40pt):
//   [0 .. N*HE_GLYPH)      glyph bitmaps, 24x41 cell, 3 bytes/row, bit 0 = ink
//   [N*HE_GLYPH .. +N)     one advance-width byte per glyph
//
// Glyphs are left-normalised (ink starts at x=0) and advance by their real ink
// width, not a fixed cell. Fixed-width gave vav and yod the same 24px slot as
// shin, which is what made the first version read as stretched out.

extern const uint8_t font24HE_start[] asm("_binary_font24HE_FON_start");
extern const uint8_t font24HE_end[]   asm("_binary_font24HE_FON_end");

#define HE_W        24                  // cell width (max glyph width)
#define HE_H        41                  // cell height
#define HE_ROWBYTES 3
#define HE_GLYPH    (HE_ROWBYTES * HE_H)
#define HE_BASE     0x05D0
#define HE_LAST     0x05EA
#define HE_NGLYPH   (HE_LAST - HE_BASE + 1)
#define HE_GAP      3                   // space between letters
#define HE_SPACE    9                   // width of a word space
#define LAT_W       16                  // Font16 advance, for embedded Latin

static inline bool is_hebrew(uint32_t cp) { return cp >= 0x0590 && cp <= 0x05FF; }

// Decodes one UTF-8 sequence. Returns bytes consumed, 0 at end of string.
static int utf8_next(const char *s, uint32_t *cp)
{
    unsigned char c = (unsigned char)s[0];
    if (!c) return 0;
    if (c < 0x80)          { *cp = c;                       return 1; }
    if ((c & 0xE0) == 0xC0 && s[1]) {
        *cp = ((uint32_t)(c & 0x1F) << 6) | (s[1] & 0x3F);  return 2;
    }
    if ((c & 0xF0) == 0xE0 && s[1] && s[2]) {
        *cp = ((uint32_t)(c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        return 3;
    }
    if ((c & 0xF8) == 0xF0 && s[1] && s[2] && s[3]) { *cp = 0xFFFD; return 4; }
    *cp = 0xFFFD;                                           return 1;
}

// Per-glyph ink width from the table appended after the bitmaps.
static int he_glyph_width(uint32_t cp)
{
    if (cp < HE_BASE || cp > HE_LAST) return 0;
    size_t avail = (size_t)(font24HE_end - font24HE_start);
    size_t woff  = (size_t)HE_NGLYPH * HE_GLYPH + (cp - HE_BASE);
    if (woff >= avail) return HE_W;               // table missing: fall back
    int w = font24HE_start[woff];
    return (w > 0 && w <= HE_W) ? w : HE_W;
}

// How far the pen moves after drawing this codepoint.
static int he_advance(uint32_t cp)
{
    if (cp == ' ')                    return HE_SPACE;
    if (is_hebrew(cp))                return he_glyph_width(cp) + HE_GAP;
    if (cp >= 0x21 && cp < 0x7F)      return LAT_W;
    return 0;                                     // not drawable, not counted
}

static void he_draw_glyph(int x, int y, uint32_t cp)
{
    if (cp < HE_BASE || cp > HE_LAST) return;
    size_t avail = (size_t)(font24HE_end - font24HE_start);
    size_t off   = (size_t)(cp - HE_BASE) * HE_GLYPH;
    if (off + HE_GLYPH > avail) return;

    const uint8_t *g = font24HE_start + off;
    for (int row = 0; row < HE_H; row++) {
        for (int b = 0; b < HE_ROWBYTES; b++) {
            uint8_t byte = g[row * HE_ROWBYTES + b];
            if (byte == 0xFF) continue;                   // all background
            for (int bit = 0; bit < 8; bit++) {
                if (!((byte >> (7 - bit)) & 1))           // 0 = ink
                    Paint_SetPixel(x + b * 8 + bit, y + row, BLACK);
            }
        }
    }
}

static int he_measure(const char *s)
{
    uint32_t cp; int n, w = 0;
    while ((n = utf8_next(s, &cp)) > 0) { s += n; w += he_advance(cp); }
    return w;
}

// Draws a mixed Hebrew/Latin line right-to-left from `right_x`.
//
// Deliberately simplified bidi, not the Unicode algorithm: the line runs RTL and
// Latin/digit runs keep their internal LTR order. Correct for the shape real
// headlines take ("אפל חושפת Mac mini"); nested or bidirectional punctuation
// will not be perfect.
static void he_draw_line_rtl(int right_x, int y, const char *s)
{
    int x = right_x;
    const char *p = s;
    uint32_t cp;
    int n;

    while ((n = utf8_next(p, &cp)) > 0) {
        if (is_hebrew(cp)) {
            int w = he_glyph_width(cp);
            x -= w + HE_GAP;
            if (x < 0) return;
            he_draw_glyph(x, y, cp);
            p += n;
        } else if (cp == ' ') {
            x -= HE_SPACE;
            p += n;
        } else if (cp > 0x20 && cp < 0x7F) {
            // Reserve the whole Latin run, then draw it LTR inside that slot so
            // "NVIDIA" does not come out reversed.
            const char *run = p;
            int runlen = 0, runbytes = 0;
            const char *q = p;
            uint32_t c2; int m;
            while ((m = utf8_next(q, &c2)) > 0 && c2 > 0x20 && c2 < 0x7F) {
                runlen++; runbytes += m; q += m;
            }
            x -= runlen * LAT_W;
            if (x < 0) return;
            char buf[96];
            int cpy = runbytes < (int)sizeof(buf) - 1 ? runbytes : (int)sizeof(buf) - 1;
            memcpy(buf, run, cpy);
            buf[cpy] = '\0';
            // Latin sits lower in the taller Hebrew cell so baselines align.
            Paint_DrawString_EN(x, y + (HE_H - 28) / 2 + 4, buf, &Font16, WHITE, BLACK);
            p += runbytes;
        } else {
            p += n;                                       // undrawable: skip
        }
    }
}

// Word-wraps an RTL string to `width` pixels, drawing each line right-aligned.
// Returns the y below the block, or -1 if it did not fit.
static int he_draw_wrapped(int y, int left_x, int right_x, const char *text, int max_lines)
{
    int width = right_x - left_x;
    const char *p = text;

    for (int line = 0; *p && line < max_lines; line++) {
        const char *q = p, *last_space = NULL;
        int w = 0;
        uint32_t cp; int n;
        while ((n = utf8_next(q, &cp)) > 0) {
            int adv = he_advance(cp);
            if (w + adv > width) break;
            if (cp == ' ') last_space = q;
            w += adv;
            q += n;
        }
        if (*q && last_space) q = last_space;             // break on the space
        if (q == p) break;                                // no progress: bail

        char buf[256];
        int len = (int)(q - p);
        if (len > (int)sizeof(buf) - 1) len = (int)sizeof(buf) - 1;
        memcpy(buf, p, len);
        buf[len] = '\0';

        if (y + HE_H > BODY_BOTTOM) return -1;
        he_draw_line_rtl(right_x, y, buf);
        y += HE_H - 6;                                    // cell has built-in slack

        p = q;
        while (*p == ' ') p++;
    }
    return y;
}
