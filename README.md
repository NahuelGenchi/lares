# Lares

Lares is one calm place to check and manage this PC and its devices. In Roman homes, the Lares were the spirits that watched over the house and everything in it.

## Sections

**Overview** opens first. It shows the processor, graphics and memory load, every paired controller, headset, keyboard and mouse with its battery, and the Remote mode. Click any part to open its section.

**Hardware** has live load graphs and temperatures for the processor and graphics card, memory use, and the system drive's space and temperature.

**Controller** is everything Padlight did, ported to C: Padlight's live controller picture with Sticks, Buttons and Vibration tabs (the stick check with drift warnings, the button test, the motor lab), battery, connection, model and firmware with the update hint, which programs have it open, Test vibration, Find controller, Disconnect, nicknames and Pair a controller. Its settings hold the low battery alerts and level, connection notices, keeping the screen awake, idle disconnect, the top-bar icon and the background helper. **Headphones** lists Bluetooth headphones with their battery and a Connect button; AirPods appear there. Each shows only when such a device is paired.

**Remote** is for playing this PC from another device, at home or away, with three free apps: Tailscale links the devices, Sunshine shares the screen, and Moonlight plays it on the other device. Until those are set up, the sidebar says *Set up remote* and the section is a three-step guide that ticks each step once Lares sees it done, and then shows the address to add in Moonlight. After that it switches between Home and Remote: Home puts the monitor back to its own size and turns Sunshine and Tailscale off; Remote turns them on and can set a screen size that fills the other device, so Moonlight shows no black bars. Click a row to change what a mode sets.

## Light on the PC

Lares is C with GTK 4. Its window uses about 18 to 30 MB of its own memory (47 to 49 MB counting every shared library in full). It draws with GTK's software renderer, since the GPU renderers load the graphics driver and use about 100 MB. Nothing is read unless a section that shows it is on screen and the window isn't minimized; controllers are read straight from their evdev nodes, never through a helper process. Closing the window ends it.

The background helper, `lares-pad`, links GLib alone and uses under 1 MB of its own (about 7 MB RSS). It runs alerts, notices, keeping the screen awake and idle disconnect while Lares is closed, and gives the top-bar addon its readings over D-Bus. It reads a controller only while a feature needs it, waiting on the device with no polling, and quits by itself when no setting needs it. Lares starts and stops it through systemd, and the Run in the background switch turns it off.

The top-bar addon (`lares@nahuelgenchi.github.io`) is JavaScript, because GNOME Shell runs nothing else in the top bar. It only draws: every reading and action comes from the helper. It stays off until Show in the top bar is turned on in Lares, and GNOME loads it at the next log-in after it is first installed.

## Build and install

It needs a C compiler, `make`, GTK 4.22 or newer with its development files, and GNOME. `make install` puts everything in `~/.local` and needs no administrator rights; it also installs the Monocraft font (SIL Open Font License) if it isn't there yet.

```bash
make && make install
```

This puts stripped `lares` and `lares-pad` in `~/.local/bin`, Lares in the app grid, the helper's unit in `~/.local/share/systemd/user`, and the addon in GNOME Shell's extensions folder, switched off.

## From a terminal

```bash
lares home      # or: lares remote
lares status    # what is set right now
lares restore   # apply the saved mode again
```

Tailscale asks for your password once before Lares can switch it. The Remote section's Allow button does that, or run `pkexec tailscale set --operator=$USER`.

## Trying it without changing anything

`LARES_DRY_RUN=1 lares` pretends every change (screen, services, Bluetooth) and keeps its own settings file. `LARES_DRY_STREAM=1` adds a pretend Moonlight stream, `LARES_COLOR_SCHEME=dark` forces dark, `LARES_SECTION=hardware` opens on a section, and `LARES_CSS=data/style.css` loads the stylesheet from disk so it can be edited without rebuilding.

## Contributing

Work is tracked with issues, milestones and pull requests; see [CONTRIBUTING.md](CONTRIBUTING.md). `make check` runs what every pull request must pass.

## License

GPL-2.0-or-later. The controller picture, its glow and `data/addon/controllerArt.js` come from Padlight, by the same author, under the same license. Xbox is a trademark of Microsoft; Lares is not affiliated with it.
