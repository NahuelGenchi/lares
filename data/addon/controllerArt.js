// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later

// Draws the controller picture with the live input state on top, from the
// window's own pixel map, controller.txt (data/pad/controller.txt, installed
// beside this file): one character per pixel, with the legend in its header.
// The rules are those of src/pad_map.c and src/pad_picture.c, so the menu and
// the Controller section show the same picture. It imports nothing, so the
// tests can run it under Node.

// The helper's button bits, in the order of PadButton in src/pad/pads.h
export const Button = {
    A: 0, B: 1, X: 2, Y: 3, LB: 4, RB: 5, LT: 6, RT: 7, VIEW: 8, MENU: 9, XBOX: 10,
    SHARE: 11, LS: 12, RS: 13, UP: 14, DOWN: 15, LEFT: 16, RIGHT: 17,
};
export const bit = button => 1 << button;

// What a map pixel is
export const Cell = {
    EMPTY: 0,
    LINE: 1,
    FAINT: 2,     // a line drawn faint: the D-pad's middle
    GAUGE: 3,     // a trigger gauge's outline
    AREA: 4,      // lights when its button is pressed
    GLYPH: 5,     // a button's letter, icon or logo
    CAP_LINE: 6,  // a stick's cap, which moves with the stick
    CAP_AREA: 7,  // the inside of a cap, lit when the stick is clicked
};

// How a map pixel shows
export const Paint = {
    NONE: 0,      // the background shows
    LINE: 1,      // the text color
    FAINT: 2,     // the text color, faint
    LIT: 3,       // the accent color: held now
    SEEN: 4,      // the accent color, soft: the checklist has seen it
};

// How far a stick's cap travels at full tilt, in map pixels. The map leaves
// this much room between each cap and its rim.
export const STICK_TRAVEL = 3;
const FAINT_ALPHA = 0.4;
const SEEN_ALPHA = 0.36;
const PLACEHOLDER_ALPHA = 0.3;

// The legend, as in the map's header
const LEGEND = new Map([
    ['.', [Cell.EMPTY, 0]], ['#', [Cell.LINE, 0]], ['+', [Cell.FAINT, 0]], ['=', [Cell.GAUGE, 0]],
    ['a', [Cell.AREA, Button.A]], ['b', [Cell.AREA, Button.B]], ['x', [Cell.AREA, Button.X]],
    ['y', [Cell.AREA, Button.Y]], ['v', [Cell.AREA, Button.VIEW]], ['m', [Cell.AREA, Button.MENU]],
    ['s', [Cell.AREA, Button.SHARE]],
    ['A', [Cell.GLYPH, Button.A]], ['B', [Cell.GLYPH, Button.B]], ['X', [Cell.GLYPH, Button.X]],
    ['Y', [Cell.GLYPH, Button.Y]], ['V', [Cell.GLYPH, Button.VIEW]], ['M', [Cell.GLYPH, Button.MENU]],
    ['S', [Cell.GLYPH, Button.SHARE]], ['H', [Cell.GLYPH, Button.XBOX]],
    ['[', [Cell.AREA, Button.LB]], [']', [Cell.AREA, Button.RB]],
    ['(', [Cell.AREA, Button.LT]], [')', [Cell.AREA, Button.RT]],
    ['u', [Cell.AREA, Button.UP]], ['d', [Cell.AREA, Button.DOWN]],
    ['l', [Cell.AREA, Button.LEFT]], ['r', [Cell.AREA, Button.RIGHT]],
    ['<', [Cell.CAP_LINE, Button.LS]], ['>', [Cell.CAP_LINE, Button.RS]],
    ['{', [Cell.CAP_AREA, Button.LS]], ['}', [Cell.CAP_AREA, Button.RS]],
]);

// A cell packs its kind with the button it belongs to.
export const kindOf = cell => cell >> 5;
export const buttonOf = cell => cell & 31;

/**
 * Reads the map. Lines starting with ';' are comments; every other line is a
 * row of the picture, and all rows have the same length.
 *
 * @param {string} text - the map file's contents
 * @returns {object} {width, height, cells: Uint8Array, gauges: [[x0, x1], [x0, x1]]}
 * @throws {Error} when a row is ragged or has a character the legend doesn't know
 */
export function parseMap(text) {
    const rows = text.split('\n').map(row => row.replace(/\r$/, ''))
        .filter(row => row && !row.startsWith(';'));
    if (!rows.length)
        throw new Error('the map has no rows');
    const width = rows[0].length;
    const height = rows.length;
    const cells = new Uint8Array(width * height);
    rows.forEach((row, y) => {
        if (row.length !== width)
            throw new Error(`row ${y + 1} is ${row.length} pixels long, not ${width}`);
        for (let x = 0; x < width; x++) {
            const entry = LEGEND.get(row[x]);
            if (!entry)
                throw new Error(`row ${y + 1} has '${row[x]}', which the legend doesn't know`);
            cells[y * width + x] = entry[0] << 5 | entry[1];
        }
    });

    // Each trigger gauge's inside, in map pixels; LT's fills from the left,
    // RT's from the right, so the two mirror each other.
    const gauges = [[width, -1], [width, -1]];
    for (let i = 0; i < cells.length; i++) {
        const button = buttonOf(cells[i]);
        if (kindOf(cells[i]) !== Cell.AREA || (button !== Button.LT && button !== Button.RT))
            continue;
        const gauge = gauges[button === Button.RT ? 1 : 0];
        gauge[0] = Math.min(gauge[0], i % width);
        gauge[1] = Math.max(gauge[1], i % width);
    }
    return {width, height, cells, gauges};
}

/**
 * How a pixel shows, given the buttons held and the ones the checklist has
 * seen, as bits. A trigger gauge counts as held as far as it is pulled.
 *
 * @param {number} cell - a map cell
 * @param {number} held - bit(button) of each button held
 * @param {number} seen - bit(button) of each button seen
 * @returns {number} a Paint
 */
export function cellPaint(cell, held, seen) {
    const mask = bit(buttonOf(cell));
    switch (kindOf(cell)) {
    case Cell.LINE:
    case Cell.GAUGE:
    case Cell.CAP_LINE:
        return Paint.LINE;
    case Cell.FAINT:
        return Paint.FAINT;
    case Cell.AREA:
    case Cell.CAP_AREA:
        if (held & mask)
            return Paint.LIT;
        return seen & mask ? Paint.SEEN : Paint.NONE;
    case Cell.GLYPH:
        // The Xbox logo is its own button and lights itself; a letter or icon
        // is cut out of its lit button instead.
        if (buttonOf(cell) === Button.XBOX) {
            if (held & mask)
                return Paint.LIT;
            return seen & mask ? Paint.SEEN : Paint.LINE;
        }
        return held & mask ? Paint.NONE : Paint.LINE;
    default:
        return Paint.NONE;
    }
}

// Halves away from zero, as C's lround(), so both pictures agree.
const round = value => Math.sign(value) * Math.round(Math.abs(value));
const clamp01 = value => Math.min(Math.max(value ?? 0, 0), 1);

// A stick moves in a circle; the raw corners reach past it.
function capOffset(dx = 0, dy = 0) {
    const reach = Math.hypot(dx, dy);
    if (reach > 1) {
        dx /= reach;
        dy /= reach;
    }
    return [round(dx * STICK_TRAVEL), round(dy * STICK_TRAVEL)];
}

/**
 * What a frame shows, worked out from the controller's state: a new state
 * that looks the same needs no repaint (see sameLook).
 *
 * @param {object} map - from parseMap
 * @param {object} [state] - {buttons: bits, lx, ly, rx, ry, lt, rt}, as the helper sends them
 * @param {number} [seen] - bit(button) of each button the checklist has seen
 * @returns {object} the look
 */
export function lookOf(map, state = {}, seen = 0) {
    const [lx, ly] = capOffset(state.lx, state.ly);
    const [rx, ry] = capOffset(state.rx, state.ry);
    // The gauges stand for the triggers, lit as far as they are pulled.
    const pulled = (value, [x0, x1]) => round(clamp01(value) * (x1 - x0 + 1));
    return {
        held: (state.buttons ?? 0) & ~(bit(Button.LT) | bit(Button.RT)),
        seen,
        lx, ly, rx, ry,
        lt: pulled(state.lt, map.gauges[0]),
        rt: pulled(state.rt, map.gauges[1]),
    };
}

export const sameLook = (a, b) => Object.keys(a).every(key => a[key] === b[key]);

function gaugeLit(map, look, button, x) {
    if (button === Button.RT)
        return x > map.gauges[1][1] - look.rt;
    return x < map.gauges[0][0] + look.lt;
}

// Every map pixel's Paint: everything that stays put, then the caps where the
// sticks have them, over it.
function paintPixels(map, look, placeholder) {
    const {width, height, cells} = map;
    const pixels = new Uint8Array(width * height);
    for (const caps of [false, true]) {
        for (let y = 0; y < height; y++) {
            for (let x = 0; x < width; x++) {
                const cell = cells[y * width + x];
                const kind = kindOf(cell);
                const button = buttonOf(cell);
                if ((kind === Cell.CAP_LINE || kind === Cell.CAP_AREA) !== caps)
                    continue;
                if (placeholder && kind === Cell.GAUGE)
                    continue;
                let held = look.held;
                if (kind === Cell.AREA && (button === Button.LT || button === Button.RT) &&
                    gaugeLit(map, look, button, x))
                    held |= bit(button);
                const paint = cellPaint(cell, held, look.seen);
                if (paint === Paint.NONE)
                    continue;
                let px = x, py = y;
                if (caps) {
                    const right = button === Button.RS;
                    px += right ? look.rx : look.lx;
                    py += right ? look.ry : look.ly;
                }
                if (px >= 0 && py >= 0 && px < width && py < height)
                    pixels[py * width + px] = paint;
            }
        }
    }
    return pixels;
}

/**
 * Draws the controller, centred, with as many whole screen pixels per map
 * pixel as fit, so every map pixel lands on the screen's grid and stays crisp.
 *
 * @param {object} cr - cairo context
 * @param {number} width - space to draw in
 * @param {number} height
 * @param {object} map - from parseMap
 * @param {object} look - from lookOf
 * @param {object} colors - {line, accent}, each [r, g, b, a] from 0 to 1
 * @param {boolean} [placeholder] - a controller that is not there: dimmed, without gauges
 */
export function drawController(cr, width, height, map, look, colors, placeholder = false) {
    const scale = Math.max(1, Math.floor(Math.min(width / map.width, height / map.height)));
    const left = Math.floor((width - map.width * scale) / 2);
    const top = Math.floor((height - map.height * scale) / 2);
    const pixels = paintPixels(map, look, placeholder);

    // Runs of one Paint along each row, so each color is one fill.
    const runs = [[], [], [], [], []];
    for (let y = 0; y < map.height; y++) {
        const row = y * map.width;
        for (let x = 0; x < map.width;) {
            const paint = pixels[row + x];
            let end = x + 1;
            while (end < map.width && pixels[row + end] === paint)
                end++;
            if (paint !== Paint.NONE)
                runs[paint].push(x, y, end - x);
            x = end;
        }
    }

    const strength = placeholder ? PLACEHOLDER_ALPHA : 1;
    const shades = {
        [Paint.LINE]: [colors.line, 1],
        [Paint.FAINT]: [colors.line, FAINT_ALPHA],
        [Paint.LIT]: [colors.accent, 1],
        [Paint.SEEN]: [colors.accent, SEEN_ALPHA],
    };
    for (const [paint, [[r, g, b, a], alpha]] of Object.entries(shades)) {
        const run = runs[paint];
        if (!run.length)
            continue;
        for (let i = 0; i < run.length; i += 3)
            cr.rectangle(left + run[i] * scale, top + run[i + 1] * scale, run[i + 2] * scale, scale);
        cr.setSourceRGBA(r, g, b, a * alpha * strength);
        cr.fill();
    }
}
