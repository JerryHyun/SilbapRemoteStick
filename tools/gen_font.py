#!/usr/bin/env python3
"""Generate ``main/font_data.c`` from a GNU Unifont ``.hex`` bitmap font.

The output is a flat ``uint16_t`` glyph table plus a small range table that says
which run of codepoints the table holds.  Each glyph is 16 rows of one
``uint16_t``; bit 15 of a row is the leftmost pixel, bit 0 the rightmost.

Unifont stores two glyph widths:

* 8x16 half-width glyphs (16 bytes of hex per line) occupy the *left* half of
  the 16x16 cell, so each row becomes ``byte << 8`` and the right half stays
  blank.  Text that walks 8 px per cell is therefore rendered at its native
  width, and wide glyphs still fit.
* 16x16 full-width glyphs (32 bytes of hex per line) are stored as-is.

Codepoints outside the generated ranges fall back to U+003F in the renderer;
the script fails loudly when a *requested* codepoint is absent from the .hex.
"""

import argparse
import gzip
import io
import os
import sys
import tempfile
import urllib.request

UNIFONT_VERSION = "16.0.04"
UNIFONT_URL = (
    "https://unifoundry.com/pub/unifont/unifont-%s/font-builds/unifont-%s.hex.gz"
    % (UNIFONT_VERSION, UNIFONT_VERSION)
)

# Stage 1: ASCII, Latin-1, typographic dashes, ellipsis, middle dot and the
# filled circle used for the status bullet.  Stage 2 adds the Hangul blocks so
# a Korean SSID renders instead of falling back to '?'.  Ranges are inclusive;
# overlapping or adjacent ones are merged below, in ascending order.
RANGES = [
    (0x0020, 0x007E),
    (0x00A0, 0x00FF),
    (0x2010, 0x2015),
    (0x2026, 0x2026),
    (0x00B7, 0x00B7),
    (0x25CF, 0x25CF),
    # Stage 2: Hangul compatibility jamo (U+3131..U+318E) and the precomposed
    # syllables (U+AC00..U+D7A3, 11172 of them).  Unifont draws both full-width.
    (0x3131, 0x318E),
    (0xAC00, 0xD7A3),
]

FALLBACK_CP = 0x003F  # '?' - always present, the renderer's fallback


def merge(ranges):
    """Sort, drop contained ranges and merge overlapping/adjacent ones."""
    out = []
    for first, last in sorted(ranges):
        if out and first <= out[-1][1] + 1:
            out[-1] = (out[-1][0], max(out[-1][1], last))
        else:
            out.append((first, last))
    return out


def download(url, path):
    sys.stderr.write("downloading %s\n" % url)
    with urllib.request.urlopen(url, timeout=120) as resp:
        data = resp.read()
    tmp = path + ".part"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)


def load_hex(path):
    """Parse ``CP:HEX`` lines into {codepoint: hex data}."""
    if path.endswith(".gz"):
        raw = gzip.open(path, "rb")
        text = io.TextIOWrapper(raw, encoding="ascii", errors="replace")
    else:
        text = open(path, "r", encoding="ascii", errors="replace")
    glyphs = {}
    with text:
        for line in text:
            line = line.strip()
            if not line or line.startswith("#") or ":" not in line:
                continue
            cp_hex, data = line.split(":", 1)
            data = data.strip()
            if len(data) % 2:
                continue
            try:
                cp = int(cp_hex, 16)
            except ValueError:
                continue
            glyphs[cp] = data
    return glyphs


def rows_for(data):
    """Return 16 uint16 rows, bit 15 = leftmost pixel."""
    if len(data) == 32:            # 8x16, left-aligned in the 16x16 cell
        return [int(data[2 * r:2 * r + 2], 16) << 8 for r in range(16)]
    if len(data) == 64:            # 16x16
        return [int(data[4 * r:4 * r + 4], 16) for r in range(16)]
    raise ValueError("unsupported glyph data length %d (%d px wide)"
                     % (len(data) // 2, len(data) * 4 // 16))


def c_int(value):
    return "0x%04X" % value


def emit(path, ranges, glyphs):
    total = sum(last - first + 1 for first, last in ranges)
    lines = []
    lines.append("/*")
    lines.append(" * font_data.c - GENERATED 16x16 LCD glyph table. Do not edit by hand.")
    lines.append(" *")
    lines.append(" * Source:  GNU Unifont %s (unifont-%s.hex.gz)" % (UNIFONT_VERSION, UNIFONT_VERSION))
    lines.append(" *          %s" % UNIFONT_URL)
    lines.append(" * Licence: SIL Open Font License 1.1 (dual-licensed with GPLv2 + font")
    lines.append(" *          embedding exception); see THIRD_PARTY_NOTICES.md.")
    lines.append(" *")
    lines.append(" * Regenerate with:")
    lines.append(" *     python3 tools/gen_font.py --hex /path/to/unifont-%s.hex.gz" % UNIFONT_VERSION)
    lines.append(" *")
    lines.append(" * Ranges (inclusive, in table order):")
    for first, last in ranges:
        lines.append(" *     U+%04X..U+%04X  (%d glyph%s)" %
                     (first, last, last - first + 1, "" if last == first else "s"))
    lines.append(" *")
    lines.append(" * Format: flat uint16_t font_glyphs[], 16 rows per glyph, one uint16_t per")
    lines.append(" * row, bit 15 = leftmost pixel. Rows are stored top to bottom. The table is")
    lines.append(" * NOT one contiguous run of codepoints: font_ranges[] says where each run")
    lines.append(" * starts, so a lookup finds the run first (see font_range_t in lcd.h).")
    lines.append(" * U+00B7 sits inside U+00A0..U+00FF, so it needs no separate run.")
    lines.append(" * %d glyphs, %d bytes." % (total, total * 32))
    lines.append(" *")
    lines.append(" * Unifont half-width (8x16) glyphs are left-aligned in the cell: the low byte")
    lines.append(" * of every row is zero and a caller that advances 8 px per glyph gets the")
    lines.append(" * native width. Full-width (16x16) glyphs use all 16 columns.")
    lines.append(" */")
    lines.append("")
    lines.append('#include <stdint.h>')
    lines.append("")
    lines.append('#include "lcd.h"')
    lines.append("")
    lines.append("const uint32_t font_first_cp = 0x%04X;" % ranges[0][0])
    lines.append("const uint32_t font_last_cp = 0x%04X;" % ranges[-1][1])
    lines.append("")
    lines.append("const font_range_t font_ranges[] = {")
    offset = 0
    for first, last in ranges:
        lines.append("    { 0x%04X, 0x%04X, %5d },  /* %d glyph%s */"
                     % (first, last, offset, last - first + 1, "" if last == first else "s"))
        offset += last - first + 1
    lines.append("};")
    lines.append("const unsigned font_range_count = sizeof(font_ranges) / sizeof(font_ranges[0]);")
    lines.append("")
    lines.append("const uint16_t font_glyphs[] = {")
    offset = 0
    for first, last in ranges:
        for cp in range(first, last + 1):
            rows = rows_for(glyphs[cp])
            name = cp_name(cp)
            lines.append("    /* U+%04X%s */ %s," %
                         (cp, " " + name if name else "", ", ".join(c_int(r) for r in rows)))
            offset += 1
    lines.append("};")
    lines.append("")
    with open(path, "w", encoding="ascii") as f:
        f.write("\n".join(lines))
    return total


def cp_name(cp):
    """A short printable name for the comment column, never raw control bytes."""
    if cp == 0x20:
        return "space"
    if 0x21 <= cp <= 0x7E:
        return "'%s'" % chr(cp)
    return ""


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--hex", help="path to unifont-<version>.hex[.gz]; downloaded when omitted")
    ap.add_argument("--out", default=os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                  "..", "main", "font_data.c"),
                    help="output C file (default: main/font_data.c)")
    ap.add_argument("--cache", default=os.path.join(tempfile.gettempdir(),
                                                    "unifont-%s.hex.gz" % UNIFONT_VERSION),
                    help="where to keep the downloaded .hex.gz")
    args = ap.parse_args(argv)

    src = args.hex
    if not src:
        src = args.cache
        if not os.path.exists(src):
            try:
                download(UNIFONT_URL, src)
            except Exception as exc:                       # noqa: BLE001 - report and stop
                sys.stderr.write("gen_font: cannot download %s: %s\n" % (UNIFONT_URL, exc))
                return 2
    if not os.path.exists(src):
        sys.stderr.write("gen_font: no such file: %s\n" % src)
        return 2

    ranges = merge(RANGES)
    wanted = [cp for first, last in ranges for cp in range(first, last + 1)]
    glyphs = load_hex(src)

    missing = [cp for cp in wanted if cp not in glyphs]
    if FALLBACK_CP not in glyphs:
        missing.append(FALLBACK_CP)
    if missing:
        sys.stderr.write("gen_font: %d codepoint(s) missing from %s: %s\n"
                         % (len(missing), src,
                            " ".join("U+%04X" % cp for cp in missing[:32])))
        return 1

    bad = []
    for cp in wanted:
        try:
            rows_for(glyphs[cp])
        except ValueError as exc:
            bad.append("U+%04X: %s" % (cp, exc))
    if bad:
        sys.stderr.write("gen_font: unusable glyph data:\n  %s\n" % "\n  ".join(bad))
        return 1

    total = emit(os.path.abspath(args.out), ranges, glyphs)
    sys.stderr.write("gen_font: wrote %s (%d glyphs, %d bytes of glyph data)\n"
                     % (os.path.abspath(args.out), total, total * 32))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
