# Lares

Lares is one calm place to check and manage this PC and its devices. In Roman homes, the Lares were the spirits that watched over the house and everything in it.

## Sections

**Overview** opens first. It shows the processor, graphics and memory load, every paired controller, headset, keyboard and mouse with its battery, and the Remote mode. Click any part to open its section.

**Hardware** has live load graphs and temperatures for the processor and graphics card, memory use, and the system drive's space and temperature.

**Controller** is everything Padlight did, ported to C: a live pixel-art picture of the controller on the Buttons tab, traced from Padlight's, which marks every button pressed so far, plus Sticks and Vibration tabs (the stick check with drift warnings, the motor lab), battery, connection, model and firmware with the update hint, which programs have it open, Test vibration, Find controller, Disconnect, nicknames and Pair a controller. Its settings hold the low battery alerts and level, connection notices, keeping the screen awake and idle disconnect. **Headphones** lists Bluetooth headphones with their battery and a Connect button; AirPods appear there. **Keyboard** and **Mouse** list the keyboards and mice plugged in by USB, wired or on their own receiver, and the paired Bluetooth ones, with the battery when the device reports one, who made it, how it's connected (USB, a USB receiver or Bluetooth), and a Connect button for Bluetooth ones. Each shows only when such a device is paired.

A HyperX Pulsefire Haste 2 Wireless on its receiver reports its battery and settings only to HyperX's own app, through an interface Linux keeps for administrators. Its card has an Allow button, which asks for your password once and adds `/etc/udev/rules.d/70-lares-batteries.rules`, letting you open that interface and nothing else of the mouse. `make uninstall` leaves the rule in place; remove it with `sudo rm` to take the permission back. Once allowed, the card also shows and changes the mouse's DPI stages and the one in use, its polling rate, lift-off distance, lighting (off, a solid or breathing colour, or the colour cycle) and brightness. Each change goes to the mouse at once and into the profile it loads when switched on, as HyperX's app saves it. Pressing the mouse's DPI button shows on the card. A sleeping mouse can't be read or changed; moving it wakes it. Plugged in by its cable, the mouse still has one card, read and changed the same way through its own matching interface, which the rule also opens; if you allowed it before Lares knew the cable, its card asks once more. The protocol follows the notes of [ngenuity-linux-cli](https://github.com/PMarinov1994/ngenuity-linux-cli) (MIT).

A Logitech G715 on its Lightspeed receiver gets the same Allow button, which adds the receiver's HID++ interface to that rule file, and only that: the keyboard's own node under it, which carries its keystrokes, stays closed. Once allowed, its card shows and changes its brightness, in the same levels its brightness key steps through, and follows that key as it's pressed. It also shows and changes the lighting of its keys: off, a solid, breathing or ripple colour from the same nine as the mouse, the colour cycle, the wave, or the built-in effect it came with. Wave, Ripple and Built-in go into the keyboard's onboard profile, which it keeps and plays by itself without Lares running, after a power cycle and on another PC. The G715 can't keep the others there, so the background helper plays them while you're logged in, as Logitech's own app does: it dims them after a minute without typing, lights them again at the next key, and takes the lights again after the keyboard sleeps or is switched off and on. Without the helper, the keyboard plays the effect its profile keeps. A pick is sent once you stop picking for a moment. A sleeping keyboard can't be read or changed; pressing a key wakes it, and the card follows. The requests follow Logitech's HID++ 2.0, and Lares sends only the ones it needs to read, plus setting the brightness, writing that one profile and the live lighting; the keyboard's factory copy in its ROM, and its firmware updates, are never touched.

**Remote** is for playing this PC from another device, at home or away, with three free apps: Tailscale links the devices, Sunshine shares the screen, and Moonlight plays it on the other device. Until those are set up, the sidebar says *Set up remote* and the section is a three-step guide that ticks each step once Lares sees it done, and then shows the address to add in Moonlight. After that it switches between Home and Remote: Home puts the monitor back to its own size and turns Sunshine and Tailscale off; Remote turns them on and can set a screen size that fills the other device, so Moonlight shows no black bars. Under the switch, a card shows whether Moonlight is streaming and this PC's IP address and host name to add it by. Click a row to change what a mode sets.

**Settings** sits below Remote and is always there, with or without a controller. It holds what applies to Lares as a whole: Run in the background, and a Top bar group with a sub-section for each device that has an icon there. Today that is Xbox controller: show the icon, its battery level, and whether it hides without a controller.

## Light on the PC

Lares is C with GTK 4. Its window uses about 18 to 30 MB of its own memory (47 to 49 MB counting every shared library in full). It draws with GTK's software renderer, since the GPU renderers load the graphics driver and use about 100 MB. Nothing is read unless a section that shows it is on screen and the window isn't minimized; controllers are read straight from their evdev nodes, never through a helper process. A mouse whose battery only its own protocol reports is asked every five minutes while its battery is on screen, in a worker thread; its settings are read when Mouse opens, and its receiver's node is listened to, not polled, while Mouse shows. Closing the window ends it.

The background helper, `lares-pad`, links GLib alone and uses under 1 MB of its own (about 7 MB RSS). It runs alerts, notices, keeping the screen awake and idle disconnect while Lares is closed, plays a G715's live lighting, and gives the top-bar addon its readings over D-Bus. It reads a controller only while a feature needs it, listens to the keyboard's receiver only while it plays a live lighting, waiting on each device with no polling, and quits by itself when no setting needs it. Lares starts and stops it through systemd, and the Run in the background switch, in Settings, turns it off.

The top-bar addon (`lares@nahuelgenchi.github.io`) is JavaScript, because GNOME Shell runs nothing else in the top bar. It only draws: every reading and action comes from the helper. It stays off until Show the icon is turned on under Top bar in Lares' Settings, and GNOME loads it at the next log-in after it is first installed.

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

GPL-2.0-or-later. The controller artwork, `data/pad/controller.png`, comes from Padlight, by the same author, under the same license; the pixel-art picture the window and the top-bar menu draw, `data/pad/controller.txt`, is traced from it. Xbox is a trademark of Microsoft; Lares is not affiliated with it.
