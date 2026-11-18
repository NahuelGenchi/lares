// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later

// The top-bar addon's controller picture, under Node: the map parser and the
// paint rules match tests/unit.c's for the window, and the drawing lands on
// whole pixels with the triggers, sticks and buttons where they belong.
// Run by `make check` from the repository's root.

import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {
    Button, Cell, Paint, STICK_TRAVEL, bit, buttonOf, cellPaint, drawController, kindOf, lookOf,
    parseMap, sameLook,
} from '../data/addon/controllerArt.js';

const tests = [];
const test = (name, run) => tests.push([name, run]);
const cellOf = (kind, button = 0) => kind << 5 | button;

// A cairo context that keeps what is drawn as a grid of colors, one per
// screen pixel, and fails on anything that isn't a whole pixel.
function fakeCairo(width, height) {
    const screen = new Array(width * height).fill(null);
    let path = [];
    let source = null;
    return {
        screen,
        rectangle(x, y, w, h) {
            for (const v of [x, y, w, h])
                assert.ok(Number.isInteger(v), `${v} is not a whole pixel`);
            path.push([x, y, w, h]);
        },
        setSourceRGBA(...rgba) {
            source = rgba.join(',');
        },
        fill() {
            for (const [x, y, w, h] of path) {
                for (let j = y; j < y + h; j++) {
                    for (let i = x; i < x + w; i++) {
                        assert.ok(i >= 0 && j >= 0 && i < width && j < height, 'drawn off the picture');
                        screen[j * width + i] = source;
                    }
                }
            }
            path = [];
        },
    };
}

const COLORS = {line: [1, 1, 1, 1], accent: [1, 0.5, 0, 1]};
const LINE = '1,1,1,1';
const LIT = '1,0.5,0,1';

// The shipped map drawn at one screen pixel per map pixel
function draw(map, state = {}, seen = 0, placeholder = false) {
    const cr = fakeCairo(map.width, map.height);
    drawController(cr, map.width, map.height, map, lookOf(map, state, seen), COLORS, placeholder);
    return cr.screen;
}

const shipped = parseMap(readFileSync('data/pad/controller.txt', 'utf8'));
const cellsWhere = (map, match) => {
    const found = [];
    for (let i = 0; i < map.cells.length; i++) {
        if (match(kindOf(map.cells[i]), buttonOf(map.cells[i])))
            found.push(i);
    }
    return found;
};

test('parses rows and the legend, as pad_map_parse does', () => {
    const map = parseMap('; a comment\r\n.#+\r\naH<\n');
    assert.equal(map.width, 3);
    assert.equal(map.height, 2);
    assert.equal(kindOf(map.cells[1]), Cell.LINE);
    assert.equal(kindOf(map.cells[3]), Cell.AREA);
    assert.equal(buttonOf(map.cells[3]), Button.A);
    assert.equal(buttonOf(map.cells[4]), Button.XBOX);
    assert.equal(kindOf(map.cells[5]), Cell.CAP_LINE);
    assert.throws(() => parseMap('...\n..\n'), /row 2 is 2 pixels long/);
    assert.throws(() => parseMap('..?\n'), /legend doesn't know/);
    assert.throws(() => parseMap('; nothing\n'), /no rows/);
});

test('paints a pixel as pad_cell_paint does', () => {
    const area = cellOf(Cell.AREA, Button.A), letter = cellOf(Cell.GLYPH, Button.A);
    const logo = cellOf(Cell.GLYPH, Button.XBOX);
    assert.equal(cellPaint(area, 0, 0), Paint.NONE);
    assert.equal(cellPaint(area, bit(Button.A), 0), Paint.LIT);
    assert.equal(cellPaint(area, bit(Button.B), bit(Button.A)), Paint.SEEN);
    // A held button's letter is cut out of it; a seen one keeps its letter.
    assert.equal(cellPaint(letter, 0, 0), Paint.LINE);
    assert.equal(cellPaint(letter, bit(Button.A), 0), Paint.NONE);
    assert.equal(cellPaint(letter, 0, bit(Button.A)), Paint.LINE);
    // The Xbox logo lights itself.
    assert.equal(cellPaint(logo, 0, 0), Paint.LINE);
    assert.equal(cellPaint(logo, bit(Button.XBOX), 0), Paint.LIT);
    assert.equal(cellPaint(logo, 0, bit(Button.XBOX)), Paint.SEEN);
    assert.equal(cellPaint(cellOf(Cell.FAINT), ~0, ~0), Paint.FAINT);
    assert.equal(cellPaint(cellOf(Cell.CAP_AREA, Button.LS), bit(Button.LS), 0), Paint.LIT);
});

test('reads the shipped map, with every button on it', () => {
    assert.ok(shipped.width > shipped.height);
    let present = 0;
    for (const cell of shipped.cells) {
        if ([Cell.AREA, Cell.GLYPH, Cell.CAP_AREA].includes(kindOf(cell)))
            present |= bit(buttonOf(cell));
    }
    assert.equal(present, bit(Button.RIGHT + 1) - 1);
    for (const [x0, x1] of shipped.gauges)
        assert.ok(x0 < x1);
});

test('draws lines in the text color and nothing lit at rest', () => {
    const screen = draw(shipped);
    const lines = cellsWhere(shipped, kind =>
        [Cell.LINE, Cell.GAUGE, Cell.CAP_LINE, Cell.GLYPH].includes(kind));
    assert.ok(lines.every(i => screen[i] === LINE));
    assert.ok(!screen.includes(LIT));
    // The D-pad's middle lines, at 40%
    const faint = cellsWhere(shipped, kind => kind === Cell.FAINT);
    assert.ok(faint.length && faint.every(i => screen[i] === '1,1,1,0.4'));
});

test('lights a held button with its letter cut out', () => {
    const screen = draw(shipped, {buttons: bit(Button.A)});
    const area = cellsWhere(shipped, (kind, b) => kind === Cell.AREA && b === Button.A);
    const letter = cellsWhere(shipped, (kind, b) => kind === Cell.GLYPH && b === Button.A);
    assert.ok(area.every(i => screen[i] === LIT));
    assert.ok(letter.length && letter.every(i => screen[i] === null));
    assert.equal(screen.filter(c => c === LIT).length, area.length);
});

test('tints what the checklist has seen', () => {
    const screen = draw(shipped, {}, bit(Button.B));
    const area = cellsWhere(shipped, (kind, b) => kind === Cell.AREA && b === Button.B);
    assert.ok(area.every(i => screen[i] === '1,0.5,0,0.36'));
});

test('fills the trigger gauges from the outside in', () => {
    const screen = draw(shipped, {lt: 0.5, rt: 0.5, buttons: bit(Button.LT) | bit(Button.RT)});
    const [[lx0, lx1], [rx0, rx1]] = shipped.gauges;
    const lit = (b, x) => cellsWhere(shipped, (kind, cb) => kind === Cell.AREA && cb === b)
        .filter(i => i % shipped.width === x).every(i => screen[i] === LIT);
    const half = Math.round((lx1 - lx0 + 1) / 2);
    assert.ok(lit(Button.LT, lx0) && lit(Button.LT, lx0 + half - 1) && !lit(Button.LT, lx1));
    assert.ok(lit(Button.RT, rx1) && !lit(Button.RT, rx0));
    // A trigger's own bit lights nothing: the gauge shows how far it's pulled.
    assert.ok(!draw(shipped, {buttons: bit(Button.LT)}).includes(LIT));
});

test("moves a stick's cap up to its travel, in a circle", () => {
    const cap = cellsWhere(shipped, (kind, b) => kind === Cell.CAP_LINE && b === Button.LS);
    const screen = draw(shipped, {lx: 1, ly: 0});
    assert.ok(cap.every(i => screen[i + STICK_TRAVEL] === LINE));
    const corner = lookOf(shipped, {lx: 1, ly: 1});
    assert.equal(corner.lx, 2);
    assert.equal(corner.ly, 2);
    assert.equal(lookOf(shipped, {rx: -0.5}).rx, -2);
});

test('skips a repaint for a state that looks the same', () => {
    assert.ok(sameLook(lookOf(shipped, {}), lookOf(shipped, {lx: 0.05, ly: -0.05})));
    assert.ok(!sameLook(lookOf(shipped, {}), lookOf(shipped, {buttons: bit(Button.Y)})));
});

test('scales by whole pixels, centred', () => {
    const cr = fakeCairo(shipped.width * 2 + 7, shipped.height * 2 + 1);
    drawController(cr, shipped.width * 2 + 7, shipped.height * 2 + 1, shipped, lookOf(shipped),
        COLORS);
    const first = cr.screen.indexOf(LINE);
    const row = Math.floor(first / (shipped.width * 2 + 7));
    assert.equal(cr.screen[first + 1], LINE, 'two screen pixels per map pixel');
    assert.equal(cr.screen[first + shipped.width * 2 + 7], LINE);
    assert.equal(row % 2, 0);
});

test('dims the placeholder and leaves out the gauges', () => {
    const screen = draw(shipped, {}, 0, true);
    const gauge = cellsWhere(shipped, kind => kind === Cell.GAUGE);
    assert.ok(gauge.every(i => screen[i] === null));
    assert.ok(screen.includes('1,1,1,0.3'));
    assert.ok(!screen.includes(LINE));
});

let failed = 0;
for (const [name, run] of tests) {
    try {
        run();
        console.log(`ok - ${name}`);
    } catch (e) {
        failed++;
        console.log(`not ok - ${name}\n  ${e.message}`);
    }
}
if (failed)
    process.exit(1);
