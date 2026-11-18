// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later

// Lares in the top bar: an icon lit while an Xbox controller is connected,
// and a menu with each controller's live picture, battery and quick actions.
// GNOME Shell runs top-bar items only in JavaScript, so this file only draws:
// every reading and action comes from Lares' helper, lares-pad, over D-Bus.

import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import GObject from 'gi://GObject';
import St from 'gi://St';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as PanelMenu from 'resource:///org/gnome/shell/ui/panelMenu.js';
import * as PopupMenu from 'resource:///org/gnome/shell/ui/popupMenu.js';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

import {drawController, lookOf, parseMap, sameLook} from './controllerArt.js';

const BUS_NAME = 'com.nahuelgenchi.Lares.Pad';
const OBJECT_PATH = '/com/nahuelgenchi/Lares/Pad';
const IFACE = 'com.nahuelgenchi.Lares.Pad';

const METER_BLOCKS = 10;

function call(method, params = null) {
    return new Promise(resolve => {
        Gio.DBus.session.call(BUS_NAME, OBJECT_PATH, IFACE, method, params, null,
            Gio.DBusCallFlags.NO_AUTO_START, 3000, null, (bus, res) => {
                try {
                    resolve(bus.call_finish(res).recursiveUnpack());
                } catch {
                    resolve(null);
                }
            });
    });
}

// The helper sends pressed buttons as bits, in the order of controllerArt.js' Button.
function stateOf([buttons, lx, ly, rx, ry, lt, rt]) {
    return {buttons, lx, ly, rx, ry, lt, rt};
}

const rgba = color => [color.red / 255, color.green / 255, color.blue / 255, color.alpha / 255];

function since(seconds) {
    if (!seconds)
        return '';
    return ` since ${GLib.DateTime.new_from_unix_local(seconds).format('%H:%M')}`;
}

// The live picture: the window's pixel-art controller, lit as it is used.
// One screen pixel per map pixel, as the menu has no room for two.
const Picture = GObject.registerClass(
class Picture extends St.DrawingArea {
    _init(map, placeholder) {
        super._init({style_class: 'lares-picture', height: map.height, x_expand: true});
        this._map = map;
        this._placeholder = placeholder;
        this._look = lookOf(map);
    }

    // A state that looks the same, such as a stick resting a hair off centre,
    // paints nothing again.
    setState(state) {
        const look = lookOf(this._map, state);
        if (sameLook(look, this._look))
            return;
        this._look = look;
        this.queue_repaint();
    }

    vfunc_repaint() {
        const cr = this.get_context();
        const [width, height] = this.get_surface_size();
        // Lines in the menu's text color; what's held in the accent, from the stylesheet.
        const node = this.get_theme_node();
        const line = rgba(node.get_foreground_color());
        const [found, accent] = node.lookup_color('-lares-accent', false);
        drawController(cr, width, height, this._map, this._look,
            {line, accent: found ? rgba(accent) : line}, this._placeholder);
        cr.$dispose();
    }
});

// A row of square blocks, as Lares draws its meters.
function meter(fraction, low) {
    const row = new St.BoxLayout({style_class: low ? 'lares-meter lares-low' : 'lares-meter'});
    const filled = fraction > 0 ? Math.max(1, Math.round(fraction * METER_BLOCKS)) : 0;
    for (let i = 0; i < METER_BLOCKS; i++)
        row.add_child(new St.Widget({style_class: i < filled ? 'lares-block lares-on' : 'lares-block'}));
    return row;
}

const Indicator = GObject.registerClass(
class Indicator extends PanelMenu.Button {
    _init(extension) {
        super._init(0.5, 'Lares controller');
        this._extension = extension;
        this._pads = [];
        this._prefs = {};
        this._pictures = new Map();
        this._helper = false;
        this._map = undefined;

        const box = new St.BoxLayout({style_class: 'panel-status-menu-box lares-panel'});
        this._icon = new St.Icon({
            gicon: Gio.icon_new_for_string(`${extension.path}/icons/lares-pad-symbolic.svg`),
            style_class: 'system-status-icon lares-icon',
        });
        this._level = new St.Label({style_class: 'lares-level', y_align: Clutter.ActorAlign.CENTER, visible: false});
        box.add_child(this._icon);
        box.add_child(this._level);
        this.add_child(box);

        this._content = new PopupMenu.PopupBaseMenuItem({reactive: false, can_focus: false, style_class: 'lares-item'});
        this.menu.addMenuItem(this._content);
        this.menu.addMenuItem(new PopupMenu.PopupSeparatorMenuItem());
        this.menu.addAction('Open Lares', () => this._openLares()).label.add_style_class_name('lares-open');

        this._changedId = Gio.DBus.session.signal_subscribe(BUS_NAME, IFACE, 'Changed', OBJECT_PATH, null,
            Gio.DBusSignalFlags.NONE, () => this._refresh());
        this._inputId = Gio.DBus.session.signal_subscribe(BUS_NAME, IFACE, 'Input', OBJECT_PATH, null,
            Gio.DBusSignalFlags.NONE, (...args) => this._onInput(args[5].recursiveUnpack()));
        this._nameId = Gio.bus_watch_name(Gio.BusType.SESSION, BUS_NAME, Gio.BusNameWatcherFlags.NONE,
            () => this._setHelper(true), () => this._setHelper(false));

        this.menu.connect('open-state-changed', (_menu, open) => this._watch(open));
        // Dark or light menu, from its text color, as Lares follows the system.
        this._content.connect('style-changed', () => this._applyTone());
        this._build();
    }

    _setHelper(running) {
        this._helper = running;
        if (running)
            this._refresh();
        else
            this._show([], {});
    }

    async _refresh() {
        const state = await call('GetState');
        if (state)
            this._show(...state);
    }

    _show(pads, prefs) {
        this._pads = pads;
        this._prefs = prefs;
        const connected = pads.length > 0;
        if (connected)
            this._icon.add_style_class_name('lares-active');
        else
            this._icon.remove_style_class_name('lares-active');

        // The lowest charge beside the icon: that controller needs charging first.
        const levels = pads.filter(p => p.battery >= 0).sort((a, b) => a.battery - b.battery);
        this._level.visible = Boolean(prefs['battery-level'] && levels.length);
        if (this._level.visible) {
            this._level.text = levels[0]['battery-charge'] ?? '';
            if (levels[0].low)
                this._level.add_style_class_name('lares-low');
            else
                this._level.remove_style_class_name('lares-low');
        }
        // An open menu stays until it closes.
        this.visible = connected || this.menu.isOpen || !prefs['hide-without-controller'];
        this._build();
        if (this.menu.isOpen)
            this._watch(true);
    }

    _build() {
        this._content.remove_all_children();
        this._pictures.clear();
        const column = new St.BoxLayout({vertical: true, style_class: 'lares-menu', x_expand: true});
        if (!this._helper) {
            column.add_child(this._note('The background helper is off. Turn on Run in the background in Settings.'));
        } else if (!this._pads.length) {
            const empty = new St.BoxLayout({vertical: true, style_class: 'lares-card'});
            const map = this._pictureMap();
            if (map)
                empty.add_child(new Picture(map, true));
            empty.add_child(this._note('No controller connected. Plug in a USB cable, or connect one over Bluetooth.'));
            column.add_child(empty);
        }
        for (const pad of this._pads)
            column.add_child(this._card(pad));
        this._content.add_child(column);
        this._column = column;
        this._applyTone();
    }

    _applyTone() {
        if (!this._column || !this._content.get_stage())
            return;
        const {red, green, blue} = this._content.get_theme_node().get_foreground_color();
        const light = 0.299 * red + 0.587 * green + 0.114 * blue < 128;
        if (light)
            this._column.add_style_class_name('lares-on-light');
        else
            this._column.remove_style_class_name('lares-on-light');
    }

    // The picture's pixel map, read the first time a picture is shown
    _pictureMap() {
        if (this._map === undefined) {
            try {
                const [, bytes] = GLib.file_get_contents(`${this._extension.path}/controller.txt`);
                this._map = parseMap(new TextDecoder().decode(bytes));
            } catch (e) {
                console.error(`Lares: the controller picture's map: ${e.message}`);
                this._map = null;
            }
        }
        return this._map;
    }

    _note(text) {
        const label = new St.Label({text, style_class: 'lares-muted lares-note', x_expand: true});
        label.clutter_text.line_wrap = true;
        return label;
    }

    _card(pad) {
        const card = new St.BoxLayout({vertical: true, style_class: 'lares-card'});
        card.add_child(new St.Label({text: pad.name, style_class: 'lares-name'}));
        card.add_child(new St.Label({text: `${pad.connection}${since(pad.since)}`, style_class: 'lares-muted'}));

        const map = this._pictureMap();
        if (map) {
            const picture = new Picture(map, false);
            this._pictures.set(pad.path, picture);
            card.add_child(picture);
        }

        const battery = new St.BoxLayout({style_class: 'lares-battery'});
        if (pad.battery >= 0)
            battery.add_child(meter(pad.battery, pad.low));
        if (pad['battery-text'])
            battery.add_child(new St.Label({text: pad['battery-text'], style_class: pad.low ? 'lares-low' : 'lares-text'}));
        card.add_child(battery);

        const actions = new St.BoxLayout({style_class: 'lares-actions'});
        const action = (label, method) => {
            const button = new St.Button({label, style_class: 'lares-button', can_focus: true});
            button.connect('clicked', () => call(method, new GLib.Variant('(s)', [pad.path])));
            actions.add_child(button);
        };
        if (pad.rumble)
            action('Vibrate', 'Rumble');
        if (pad.rumble && pad.wireless)
            action(pad.finding ? 'Stop finding' : 'Find', 'Find');
        if (pad.bluetooth)
            action('Disconnect', 'Disconnect');
        if (actions.get_n_children())
            card.add_child(actions);
        return card;
    }

    // The helper reads a controller only while its picture is showing.
    _watch(open) {
        if (!this._helper)
            return;
        for (const pad of this._pads)
            call(open ? 'Watch' : 'Unwatch', new GLib.Variant('(s)', [pad.path]));
        if (!open) {
            for (const picture of this._pictures.values())
                picture.setState({});
            this.visible = this._pads.length > 0 || !this._prefs['hide-without-controller'];
        }
    }

    _onInput([path, state]) {
        const picture = this._pictures.get(path);
        if (!picture || !this.menu.isOpen)
            return;
        picture.setState(stateOf(state));
    }

    // Settings while the helper is off, as that's where it's turned on.
    _openLares() {
        const section = this._helper ? 'controller' : 'settings';
        try {
            Gio.Subprocess.new(['lares', '--section', section], Gio.SubprocessFlags.NONE);
        } catch {
            Gio.Subprocess.new([`${GLib.get_home_dir()}/.local/bin/lares`, '--section', section],
                Gio.SubprocessFlags.NONE);
        }
    }

    destroy() {
        this._watch(false);
        Gio.DBus.session.signal_unsubscribe(this._changedId);
        Gio.DBus.session.signal_unsubscribe(this._inputId);
        Gio.bus_unwatch_name(this._nameId);
        super.destroy();
    }
});

export default class LaresExtension extends Extension {
    enable() {
        this._indicator = new Indicator(this);
        Main.panel.addToStatusArea(this.uuid, this._indicator);
    }

    disable() {
        this._indicator?.destroy();
        this._indicator = null;
    }
}
