# Lares

Lares is one calm place to check and manage this PC and its devices. In Roman homes, the Lares were the spirits that watched over the house and everything in it.

## Sections

**Overview** opens first. It shows the processor, graphics and memory load, every paired controller, headset, keyboard and mouse with its battery, and the Remote mode. Click any part to open its section.

**Hardware** has live load graphs and temperatures for the processor and graphics card, memory use, and the system drive's space and temperature.

**Controller** is everything Padlight did, ported to C: a live pixel-art picture of the controller on the Buttons tab, traced from Padlight's, which marks every button pressed so far, plus Sticks and Vibration tabs (the stick check with drift warnings, the motor lab), battery, connection, model and firmware with the update hint, which programs have it open, Test vibration, Find controller, Disconnect, nicknames and Pair a controller. Its settings hold the low battery alerts and level, connection notices, keeping the screen awake and idle disconnect. **Headphones** lists Bluetooth headphones with their battery and a Connect button; AirPods appear there. **Keyboard** and **Mouse** list the keyboards and mice plugged in by USB, wired or on their own receiver, and the paired Bluetooth ones, with the battery when the device reports one, who made it, how it's connected (USB, a USB receiver or Bluetooth), and a Connect button for Bluetooth ones. Each shows only when such a device is paired.

A HyperX Pulsefire Haste 2 Wireless on its receiver reports its battery and settings only to HyperX's own app, through an interface Linux keeps for administrators. Its card has an Allow button, which asks for your password once and adds `/etc/udev/rules.d/70-lares-batteries.rules`, letting you open that interface and nothing else of the mouse. `make uninstall` leaves the rule in place; remove it with `sudo rm` to take the permission back. Once allowed, the card also shows and changes the mouse's DPI stages and the one in use, its polling rate, lift-off distance, lighting (off, a solid or breathing colour, or the colour cycle) and brightness. Each change goes to the mouse at once and into the profile it loads when switched on, as HyperX's app saves it. Pressing the mouse's DPI button shows on the card. A sleeping mouse can't be read or changed; moving it wakes it. Plugged in by its cable, the mouse still has one card, read and changed the same way through its own matching interface, which the rule also opens; if you allowed it before Lares knew the cable, its card asks once more. The protocol follows the notes of [ngenuity-linux-cli](https://github.com/PMarinov1994/ngenuity-linux-cli) (MIT).

A Logitech G715 on its Lightspeed receiver gets the same Allow button, which adds the receiver's HID++ interface to that rule file, and only that: the keyboard's own node under it, which carries its keystrokes, stays closed. Once allowed, its card shows and changes its brightness, in the same levels its brightness key steps through, and follows that key as it's pressed. It also shows and changes the lighting of its keys: off, a solid, breathing or ripple colour from the same nine as the mouse, the colour cycle, the wave, or the built-in effect it came with. Wave, Ripple and Built-in go into the keyboard's onboard profile, which it keeps and plays by itself without Lares running, after a power cycle and on another PC. The G715 can't keep the others there, so the background helper plays them while you're logged in, as Logitech's own app does: it dims them when the keyboard has gone unused for its dim time, turns them off at its lights-off time, lights them again at the next key, and takes the lights again after the keyboard sleeps or is switched off and on. Without the helper, the keyboard plays the effect its profile keeps. Under the lighting, switches turn the keyboard's startup and shutdown animations on or off, and chips set how long it waits before its lights dim and before they go off (1 to 15 minutes, as Logitech's app offers); the keyboard keeps all four. A pick is sent once you stop picking for a moment. A sleeping keyboard can't be read or changed; pressing a key wakes it, and the card follows. The requests follow Logitech's HID++ 2.0, and Lares sends only the ones it needs to read, plus setting the brightness, writing that one profile, turning the startup and shutdown effects on or off, and the live lighting; the keyboard's factory copy in its ROM, and its firmware updates, are never touched.

**Share** sends and receives files with iPhones, iPads and Macs over AirDrop. Turn on *Receive* and nearby Apple devices can send you files for 10 minutes, as an iPhone's *Everyone for 10 Minutes* does; each transfer asks first, with **Accept** and **Decline**, and files land in Downloads without replacing anything. *Send* picks files, then lists the nearby devices set to *Everyone for 10 Minutes*, with progress as each one receives. It needs a Wi-Fi card that can run Apple's peer-to-peer Wi-Fi (AWDL); Lares lends it to a small program of its own, `lares-awdl`, only while receiving or sending, and gives it back after. The first time, an **Allow** button installs that program with the two network permissions it needs and, with ufw on, lets AirDrop through the firewall on its own interface only, which asks for your password once; `make uninstall` says how to remove it. Contacts Only, NameDrop and AirDrop codes need an Apple account and work with no non-Apple device. [docs/AIRDROP.md](docs/AIRDROP.md) has the design and the cards known to work. AirDrop is a trademark of Apple Inc.; Lares is not affiliated with Apple.

**Remote** is for playing this PC from another device, at home or away, with three free apps: Tailscale links the devices, Sunshine shares the screen, and Moonlight plays it on the other device. Until those are set up, the sidebar says *Set up remote* and the section is a three-step guide that ticks each step once Lares sees it done, and then shows the address to add in Moonlight. After that it switches between Home and Remote: Home puts the monitor back to its own size and turns Sunshine and Tailscale off; Remote turns them on and can set a screen size that fills the other device, so Moonlight shows no black bars. Under the switch, a card shows whether Moonlight is streaming and this PC's IP address and host name to add it by. Click a row to change what a mode sets.

**Settings** sits below Remote and is always there, with or without a controller. It holds what applies to Lares as a whole: Run in the background, and a Top bar group with a sub-section for each device that has an icon there. Today that is Xbox controller: show the icon, its battery level, and whether it hides without a controller.

## Light on the PC

Lares is C with GTK 4. Its window uses about 18 to 30 MB of its own memory (47 to 49 MB counting every shared library in full). It draws with GTK's software renderer, since the GPU renderers load the graphics driver and use about 100 MB. Nothing is read unless a section that shows it is on screen and the window isn't minimized; controllers are read straight from their evdev nodes, never through a helper process. A mouse whose battery only its own protocol reports is asked every five minutes while its battery is on screen, in a worker thread; its settings are read when Mouse opens, and its receiver's node is listened to, not polled, while Mouse shows. Closing the window ends it.

The background helper, `lares-pad`, links GLib alone and uses under 1 MB of its own (about 7 MB RSS). It runs alerts, notices, keeping the screen awake and idle disconnect while Lares is closed, plays a G715's live lighting, and gives the top-bar addon its readings over D-Bus. It reads a controller only while a feature needs it, listens to the keyboard's receiver only while it plays a live lighting, waiting on each device with no polling, and quits by itself when no setting needs it. Lares starts and stops it through systemd, and the Run in the background switch, in Settings, turns it off.

Share starts nothing until *Receive* is on or files are picked to send. Then `lares-awdl`, which links libc alone and uses about 240 kB of its own memory, runs the Wi-Fi link, and Lares itself handles discovery and the transfer in worker threads, about 3 MB more while it's on (measured with a sender and a receiver in one process); when both are off, it ends a minute later, so a send or receive right after starts at once, and the card is given back. Picking files already starts the search, so nearby devices are often listed by the time the files are chosen.

The top-bar addon (`lares@nahuelgenchi.github.io`) is JavaScript, because GNOME Shell runs nothing else in the top bar. It only draws: every reading and action comes from the helper. It stays off until Show the icon is turned on under Top bar in Lares' Settings, and GNOME loads it at the next log-in after it is first installed.

## Measured gains

What a Windows 11 PC gains or pays with Lares is shown as measured numbers, never as a promise. `tools/measure.ps1` records a PC at rest (memory in use and committed, processes and threads, processor and graphics card use, start-up entries, and the own memory of the programs you name) into a small file with nothing private in it, and compares a before with an after. [docs/GAINS.md](docs/GAINS.md) has the method and the results so far, each marked confirmed, reported, planned or blocked.

## Crash safety on Windows

Lares for Windows will be able to put its own bar in place of the Windows taskbar, so what happens when something breaks comes first. [docs/SAFETY.md](docs/SAFETY.md) has the design, every check and what is still unchecked.

| Part | What it does | Status |
| :--- | :--- | :--- |
| `lares-guard.exe` | Gives the Windows taskbar back, with auto-hide as it was, when the bar crashes, hangs or is killed, and when the guard itself is killed; stops after two crashes in five minutes and asks | confirmed with a stand-in bar, and on the real taskbar of Windows Server; not yet checked on a real Windows 11 taskbar, sign-out, Explorer restart or change of screens |
| Safe mode | `--safe`, or Shift held while signing in, starts Lares with every shell part off and leaves the Windows desktop alone for the rest of the session | confirmed with a stand-in bar and in a dry run; Shift at a real sign-in is not yet checked |
| Change journal | Writes down every Windows setting the shell changes, and flushes it to disk, before changing it; **Undo all** in Settings and `lares undo` put them back, also after a crash halfway | confirmed with tests, a program killed halfway included; a real loss of power is not checked, and the settings Lares changed before the journal existed are not in it yet ([#133](https://github.com/NahuelGenchi/lares/issues/133)) |
| Crash reports | Written on the PC only, private details removed before the file is written, never sent; Settings can show and copy the newest | confirmed with a crash of the stand-in bar, and its report passes the privacy gate; a crash of Lares' own programs is not yet checked |

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

`LARES_DRY_RUN=1 lares` pretends every change (screen, services, Bluetooth) and keeps its own settings file. `LARES_DRY_STREAM=1` adds a pretend Moonlight stream, Share pretends its Wi-Fi card, link and a nearby iPhone (`LARES_DRY_FILES=a.jpg:b.jpg` picks files to send without the file dialog, and `LARES_DRY_QUIET=1` leaves no Apple device nearby), `LARES_COLOR_SCHEME=dark` forces dark, `LARES_SECTION=hardware` opens on a section, and `LARES_CSS=data/style.css` loads the stylesheet from disk so it can be edited without rebuilding.

## Contributing

Work is tracked with issues, milestones and pull requests; see [CONTRIBUTING.md](CONTRIBUTING.md). `make check` runs what every pull request must pass.

## License

GPL-2.0-or-later. The controller artwork, `data/pad/controller.png`, comes from Padlight, by the same author, under the same license; the pixel-art picture the window and the top-bar menu draw, `data/pad/controller.txt`, is traced from it. Xbox is a trademark of Microsoft; Lares is not affiliated with it. Share's AirDrop code follows the specification and implementation of [owly](https://github.com/t4t5/owly) (MIT, Copyright (c) 2026 T4T5; see `LICENSES/MIT-owly.txt`).
