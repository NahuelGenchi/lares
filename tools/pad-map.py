#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Nahuel Genchi
# SPDX-License-Identifier: GPL-2.0-or-later
"""Draws the controller picture's pixel map, data/pad/controller.txt.

    python3 tools/pad-map.py            # from the repository's root

The map is the Controller section's picture: one character per pixel, read by
src/pad_map.c. The controller's outline comes from the artwork's shape
(data/pad/controller.png); every button is a hand-drawn sprite below, placed
where the artwork has it. Positions are in the artwork's own units, 1444 by
1007, the same as the top-bar addon's controllerArt.js. Needs Pillow and NumPy,
only to run this script; building Lares doesn't.
"""
import math
import os
import sys

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ART = os.path.join(ROOT, 'data', 'pad', 'controller.png')
OUT = os.path.join(ROOT, 'data', 'pad', 'controller.txt')

ART_W, ART_H = 1444, 1007
COLS = 210                      # pixels across the artwork
CELL = ART_W / COLS             # artwork units per pixel
ROWS = int(math.ceil(ART_H / CELL))
TOP, SIDE, BOTTOM = 12, 3, 3    # room above for the trigger gauges, and around
W, H = COLS + 2 * SIDE, TOP + ROWS + BOTTOM
CENTER_X = 721.25

LEGEND = """\
; Lares' controller picture, one character per pixel, drawn by tools/pad-map.py
; (change that script and run it again rather than editing this by hand).
;   .  nothing          #  line             +  faint line (the D-pad's middle)
;   =  a trigger gauge's outline            (  )  LT and RT gauges' insides
;   a b x y   A, B, X and Y: what lights    A B X Y   their letters
;   v m s     View, Menu and Share          V M S     their icons
;   H  the Xbox logo    [  ]  LB and RB     u d l r   the D-pad's arms
;   <  >  the left and right sticks' caps, which move with the stick
;   {  }  the inside of each cap, which lights when the stick is clicked
"""


def S(text):
    """A sprite from rows of '#' and '.'."""
    return np.array([[c == '#' for c in row.strip()] for row in text.strip().split('\n')], bool)


# ---------------------------------------------------------------- sprites

LETTERS = {
    'a': S("""
        ...##...
        ..####..
        ..####..
        .##..##.
        .##..##.
        .######.
        ##....##
        ##....##"""),
    'b': S("""
        #####.
        ##..##
        ##..##
        #####.
        ##..##
        ##..##
        ##..##
        #####."""),
    'x': S("""
        ##....##
        .##..##.
        ..####..
        ...##...
        ...##...
        ..####..
        .##..##.
        ##....##"""),
    'y': S("""
        ##....##
        .##..##.
        ..####..
        ...##...
        ...##...
        ...##...
        ...##...
        ...##..."""),
}
VIEW = S("""
    ###...
    #.....
    #.####
    ..#..#
    ..####""")
MENU = S("""
    #####
    .....
    #####
    .....
    #####""")
SHARE = S("""
    ...#...
    ..###..
    #..#..#
    #.....#
    #######""")
# Traced from the Xbox logo at this size, then cleaned up by hand.
XBOX = S("""
    .....######.....
    .....######.....
    ..##...##...##..
    .####......####.
    .#####....#####.
    #####......#####
    #####..##..#####
    ####..####..####
    ###..######..###
    ###.########.###
    ##.##########.##
    .#.##########.#.
    ..############..
    ..############..
    ...##########...
    .....######.....""")

# Where things are, in artwork units, and how big, in pixels
ABXY = {'a': (1086.5, 370), 'b': (1182.5, 277.5), 'x': (988.5, 275.5), 'y': (1082.5, 183)}
ABXY_RING = 14
SMALL = {'v': (618, 277.5), 'm': (824.5, 277.5)}
SMALL_RING = 11
SHARE_AT, SHARE_SIZE = (720, 353), (13, 9)
XBOX_AT = (721.5, 134)
STICKS = {'<': (358, 277.5), '>': (906.5, 490.5)}
STICK_RIM, STICK_CAP, STICK_DISH = 31, 23, 17
DPAD_AT, DPAD_RIM, DPAD_BAR, DPAD_LEN = (534.5, 509.5), 33, 11, 27
# The left bumper's seam with the faceplate; the right one is its mirror.
LEFT_SEAM = [(200, 150), (212, 140), (228, 121), (260, 98), (300, 78), (340, 63),
             (380, 52), (420, 44), (460, 39), (500, 37), (506, 30)]
LEFT_TRIGGER_X, TRIGGER_W, GAUGE_ROWS = 300, 170, (2, 9)


def mirror(x):
    return 2 * CENTER_X - x


# ---------------------------------------------------------------- shapes

def ring(n):
    """A one-pixel circle n pixels across, the same in all eight octants."""
    m = np.zeros((n, n), bool)
    c = r = (n - 1) / 2
    for i in range((n + 1) // 2):
        j = int(math.floor(c - math.sqrt(max(0.0, r * r - (i - c) ** 2)) + 0.5))
        for y, x in ((j, i), (i, j)):
            for yy in (y, n - 1 - y):
                for xx in (x, n - 1 - x):
                    m[yy, xx] = True
    return m


def erode(m):
    inner = m.copy()
    inner[1:, :] &= m[:-1, :]
    inner[:-1, :] &= m[1:, :]
    inner[:, 1:] &= m[:, :-1]
    inner[:, :-1] &= m[:, 1:]
    inner[0, :] = inner[-1, :] = inner[:, 0] = inner[:, -1] = False
    return inner


def thin(m):
    """Drops the doubled corner pixels of a one-pixel line."""
    out = m.copy()
    for y in range(1, m.shape[0] - 1):
        for x in range(1, m.shape[1] - 1):
            if not out[y, x]:
                continue
            n, s, w, e = out[y - 1, x], out[y + 1, x], out[y, x - 1], out[y, x + 1]
            if out[y - 1:y + 2, x - 1:x + 2].sum() == 3 and (n or s) and (w or e):
                out[y, x] = False
    return out


def edge(m):
    padded = np.pad(m, 1)
    return thin(padded & ~erode(padded))[1:-1, 1:-1]


def rounded_box(w, h, cut):
    """A filled box with `cut` pixels trimmed off each corner, as stairs."""
    m = np.ones((h, w), bool)
    for k in range(cut):
        n = cut - k
        m[k, :n] = m[k, w - n:] = m[h - 1 - k, :n] = m[h - 1 - k, w - n:] = False
    return m


def inside(outline):
    """The pixels a closed outline encloses, not counting the outline."""
    h, w = outline.shape
    out = np.zeros((h + 2, w + 2), bool)
    wall = np.pad(outline, 1)
    stack = [(0, 0)]
    while stack:
        y, x = stack.pop()
        if out[y, x] or wall[y, x]:
            continue
        out[y, x] = True
        for yy, xx in ((y + 1, x), (y - 1, x), (y, x + 1), (y, x - 1)):
            if 0 <= yy < h + 2 and 0 <= xx < w + 2:
                stack.append((yy, xx))
    return ~out[1:-1, 1:-1] & ~outline


def grid(x, y):
    """The pixel an artwork position falls in."""
    return int(x // CELL) + SIDE, int(y // CELL) + TOP


def blank():
    return np.zeros((H, W), bool)


def placed(sprite, cx, cy):
    """A sprite on the whole map, its middle on the artwork position."""
    h, w = sprite.shape
    gx = int(round(cx / CELL - (w - 1) / 2 - 0.5)) + SIDE
    gy = int(round(cy / CELL - (h - 1) / 2 - 0.5)) + TOP
    m = blank()
    m[gy:gy + h, gx:gx + w] = sprite
    return m


def polyline(points):
    m = blank()
    for (ax, ay), (bx, by) in zip(points, points[1:]):
        x0, y0 = grid(ax, ay)
        x1, y1 = grid(bx, by)
        dx, dy = abs(x1 - x0), -abs(y1 - y0)
        sx, sy = (1 if x0 < x1 else -1), (1 if y0 < y1 else -1)
        err = dx + dy
        while True:
            m[y0, x0] = True
            if (x0, y0) == (x1, y1):
                break
            e2 = 2 * err
            if e2 >= dy:
                err += dy
                x0 += sx
            if e2 <= dx:
                err += dx
                y0 += sy
    return m


# ---------------------------------------------------------------- the map

def draw():
    out = np.full((H, W), '.', dtype='<U1')

    def paint(mask, ch):
        out[mask] = ch

    art = Image.open(ART).convert('RGBA')
    alpha = np.asarray(art.getchannel('A').resize((COLS, ROWS), Image.BOX)) / 255.0
    body = blank()
    body[TOP:TOP + ROWS, SIDE:SIDE + COLS] = alpha > 0.5
    outline = edge(body)

    # Bumpers: the strip above each seam lights up.
    for ch, points in (('[', LEFT_SEAM), (']', [(mirror(x), y) for x, y in LEFT_SEAM])):
        seam = polyline(points) & erode(body)
        above = blank()
        for x in np.nonzero(seam.any(0))[0]:
            above[:np.nonzero(seam[:, x])[0].min(), x] = True
        paint(above & body & ~outline, ch)
        paint(seam, '#')

    # Trigger gauges above the controller, which the front view hides.
    for ch, left in (('(', LEFT_TRIGGER_X), (')', mirror(LEFT_TRIGGER_X + TRIGGER_W))):
        x0, _ = grid(left, 0)
        x1, _ = grid(left + TRIGGER_W, 0)
        y0, y1 = GAUGE_ROWS
        out[y0, x0 + 1:x1] = out[y1, x0 + 1:x1] = '='
        out[y0 + 1:y1, x0] = out[y0 + 1:y1, x1] = '='
        out[y0 + 2:y1 - 1, x0 + 2:x1 - 1] = ch

    # Sticks: the rim stays; the cap and its dish move with the stick.
    for ch, (cx, cy) in STICKS.items():
        paint(placed(ring(STICK_RIM), cx, cy), '#')
        cap, dish = placed(ring(STICK_CAP), cx, cy), placed(ring(STICK_DISH), cx, cy)
        paint(placed(inside(ring(STICK_CAP)), cx, cy) & ~dish, '{' if ch == '<' else '}')
        paint(cap | dish, ch)

    # D-pad: a round rim and two bars whose edges all cross. Where an edge runs
    # across the other bar it is faint, so the cross keeps one strong outline.
    paint(placed(ring(DPAD_RIM), *DPAD_AT), '#')
    upright, across = rounded_box(DPAD_BAR, DPAD_LEN, 2), rounded_box(DPAD_LEN, DPAD_BAR, 2)
    vbar, hbar = placed(upright, *DPAD_AT), placed(across, *DPAD_AT)
    vline, hline = placed(edge(upright), *DPAD_AT), placed(edge(across), *DPAD_AT)
    vin, hin = vbar & ~vline, hbar & ~hline
    rows, cols = np.nonzero(hbar.any(1))[0], np.nonzero(vbar.any(0))[0]
    for ch, area in (('u', vin & (np.arange(H) < rows.min())[:, None]),
                     ('d', vin & (np.arange(H) > rows.max())[:, None]),
                     ('l', hin & (np.arange(W) < cols.min())[None, :]),
                     ('r', hin & (np.arange(W) > cols.max())[None, :])):
        paint(area, ch)
    faint = (vline & hin) | (hline & vin)
    paint((vline | hline) & ~faint, '#')
    paint(faint, '+')

    # Round buttons with a letter or icon, and Share's pill.
    def button(ch, outline_sprite, glyph, cx, cy):
        paint(placed(inside(outline_sprite), cx, cy), ch)
        paint(placed(outline_sprite, cx, cy), '#')
        paint(placed(glyph, cx, cy), ch.upper())

    for ch, (cx, cy) in ABXY.items():
        button(ch, ring(ABXY_RING), LETTERS[ch], cx, cy)
    button('v', ring(SMALL_RING), VIEW, *SMALL['v'])
    button('m', ring(SMALL_RING), MENU, *SMALL['m'])
    button('s', edge(rounded_box(*SHARE_SIZE, 2)), SHARE, *SHARE_AT)

    # The Xbox button is its logo, with no ring.
    paint(placed(XBOX, *XBOX_AT), 'H')

    paint(outline, '#')
    return out


def main():
    out = draw()
    with open(OUT, 'w') as f:
        f.write(LEGEND)
        for row in out:
            f.write(''.join(row) + '\n')
    print(f'{os.path.relpath(OUT, ROOT)}: {W} by {H} pixels')


if __name__ == '__main__':
    sys.exit(main())
