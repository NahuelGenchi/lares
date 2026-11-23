# AirDrop in Lares

[Back to README](../README.md) · Research issue [#64](https://github.com/NahuelGenchi/lares/issues/64)

This is the design for sending and receiving files between Lares and iPhones, iPads and Macs over AirDrop. Each statement is marked **confirmed** (seen in our own capture or in Apple's documents), **reported** (stated by another project or by press, not checked by us), **planned** (what Lares will do) or **unknown**.

AirDrop is a trademark of Apple Inc. Lares is not affiliated with Apple. Nothing here is from Apple: the protocol is known from published research and independent implementations.

## What a non-Apple device can do

| Feature | Lares | Why |
| :--- | :--- | :--- |
| Send to an Apple device set to *Everyone for 10 minutes* | planned | Reported working by OpenDrop and owly; Google's Quick Share does the same |
| Receive from any Apple device | planned | The Apple sender only needs Lares to answer; reported by owly |
| Send to an Apple device set to *Contacts Only* | blocked | Needs an identity Apple issues to signed-in Apple devices (an Apple-signed certificate and validation record). Only extraction from the owner's own Mac gives one, which is not a path for a shipped app. Google's Quick Share has the same limit |
| Show as a contact on the iPhone | blocked | Same identity |
| NameDrop, AirDrop codes, finishing over the internet | blocked | Need an Apple account; codes are new in iOS 26.2 and undocumented |
| Accept only known senders | planned, later | A receiver can still filter senders by the record they send, without an identity of its own |

So the iPhone's owner sets AirDrop to *Everyone for 10 minutes* before sending to Lares or receiving from it. That is the same step Google's Pixel phones ask for. Receiving from the iPhone needs nothing extra on the iPhone beyond that setting.

## How AirDrop works

AirDrop is seven layers. Lares needs all of them.

| Layer | What it is | Lares part |
| :--- | :--- | :--- |
| Wake-up | Bluetooth LE advertisement, Apple company ID `0x004C`, type `0x05` | `src/airdrop/ble.c`, through BlueZ over D-Bus |
| Link | Apple Wireless Direct Link (AWDL): peer-to-peer Wi-Fi on channels 6, 44 and 149 | `lares-awdl`, a small separate program |
| Network | IPv6 link-local on the AWDL interface (`awdl0`) | the kernel, through `lares-awdl`'s TAP interface |
| Discovery | mDNS / DNS-SD, service `_airdrop._tcp`, TXT `flags` | `src/airdrop/mdns.c` |
| Security | TLS 1.2 or 1.3 with self-signed certificates | GIO's TLS (GnuTLS through glib-networking) |
| Exchange | HTTP/1.1 `POST /Discover`, `/Ask`, `/Upload` with binary plist bodies | `src/airdrop/http.c`, `bplist.c`, `session.c` |
| Payload | CPIO (odc) archive, gzip or DVZip compressed | `src/airdrop/cpio.c`, `dvzip.c` |

A transfer goes like this. The sender advertises over Bluetooth LE, which wakes receivers in Everyone mode. Both turn on AWDL, synchronise their schedules and find each other over mDNS. The sender posts `/Discover`; a receiver that answers with its name appears in the sender's share sheet. When the user picks it, the sender posts `/Ask` with the file list on a new TLS connection, and the receiver holds the request open until its user accepts. Then `/Upload` carries the archive.

The full, numbered rules are in owly's specification ([SPEC.md](https://github.com/t4t5/owly/blob/main/docs/SPEC.md), MIT), which Lares follows and cites by rule ID (`AWDL-12`, `ASK-2`) in its code comments.

### AWDL in brief

Nodes elect a master and follow its schedule. Time is cut into 16 slots of 64 TU (about 65.5 ms each, 1.05 s in all), and each node advertises which channel it will be on in each slot. Peers can only exchange data in slots where both are on the same channel. Control traffic is 802.11 action frames (PSF and MIF) carrying TLVs; data is 802.11 data frames with an LLC/SNAP and AWDL header around IPv6.

Linux has no AWDL driver. A program has to send and receive raw 802.11 frames through a monitor interface and present a normal network interface (a TAP) to the system. The card must also acknowledge (ACK) the iPhone's unicast frames, which a plain monitor interface doesn't do. owly solves that three ways, chosen per driver: an ad-hoc cell on AWDL's BSSID, a hidden access point on cards with no ad-hoc mode, or an *active* monitor on cards that have one (reported).

## Our capture

On 2026-11-19 the development PC listened on channel 6 with its USB adapter while an iPhone 17 had its share sheet open, recording only AWDL frames (BSSID `00:25:00:ff:94:73`) for about 4.5 minutes. **Confirmed:**

- The adapter in monitor mode received 1,263 AWDL action frames from the iPhone (1,115 PSF, 148 MIF), about 4.7 a second, with device class 2 (iOS).
- The iPhone advertised all three social channels (data path state map `0x0007`).
- Its channel sequence, in opclass encoding, was mostly: 149, 149, 149, off, off, off, off, 149, **6**, 149, 149, 149, off, off, off, 149. Channel 6 gets one slot in sixteen, about 65 ms a second.

owly reports the same pattern and its cost: a 2.4 GHz-only Realtek USB adapter received a 2.6 MB photo at 32 to 35 kB/s, about 75 seconds (reported). A dual-band card on channel 149 shares many more slots with the iPhone.

## Hardware

| Part | Need | Development PC |
| :--- | :--- | :--- |
| Wi-Fi | Monitor mode with frame injection, plus ad-hoc mode, a hidden AP or active monitor so the card ACKs. 5 GHz (44 or 149) is several times faster than channel 6 | Realtek RTL8188CUS-family USB adapter (`0bda:018a`, driver `rtl8192cu`): monitor mode and ad-hoc (IBSS) confirmed by `iw`, no active monitor, 2.4 GHz only. Receiving AWDL confirmed; sending untested |
| Bluetooth | LE advertising, for waking receivers when sending | USB Bluetooth dongle, LE advertising listed by BlueZ (31-byte adverts) |

owly's driver table is the best list of cards known to work: dual-band Realtek `rtw88`/`rtw89` and MediaTek `mt76` cards work best; Intel cards work on channel 6 only, with Wi-Fi off during a session (reported).

**The development PC's adapter failed** (2026-11-19). After a first session that sent and received with the iPhone, its USB link broke at the moment `lares-awdl` gave the card back (`usbctrl_vendorreq TimeOut`, then `rtl_usb: Urb has error status 0xFFFFFFB9`, EPROTO, for every request). From then on it neither received nor sent, even as a plain Wi-Fi card scanning for networks, and a replug into the same port didn't bring it back. It sat on a bus-powered USB 2.0 hub, and `rtl8192cu` is known for such timeouts when it powers the radio off and on. Which of the two caused it isn't known yet. A card that stops receiving shows in `~/.cache/lares/lares-awdl.log` as `rx action 0`, and in the kernel log; Share says *No Apple device heard yet* after 20 s of silence.

USB adapters owly reports working both ways with an iPhone, which `lares-awdl` drives: the TP-Link Archer T3U Plus (RTL8812BU, `rtw88_8822bu`, ad-hoc cell on channel 44) and MediaTek MT7612U adapters (`mt76x2u`, active monitor, a 3 MB photo in about 5 s on channel 149). 5 GHz needs the PC's regulatory domain set to its country: under the world domain (`00`) a card may not transmit there and falls back to channel 6.

**A card with neither ad-hoc mode nor active monitor can still work.** The Realtek RTL8188EU (`0bda:8179`, driver `rtl8xxxu`) offers only station and monitor mode. Its plain monitor was measured to ACK unicast all the same (issue #98): probe requests sent from it to nearby access points, once with the card's own address and once with one made up, were answered with first copies of the probe response and no retries, where a card that doesn't ACK gets the same response several times over. `lares-awdl` therefore has a third mode for this driver, a plain monitor on the AWDL channel and nothing else. It was measured on 2.4 GHz, channel 6, and has not been tried with an Apple device yet.

AWDL takes the Wi-Fi card for itself while it runs, unless the card can inject while connected. On the development PC that costs nothing: it is on Ethernet, and the USB adapter is used for nothing else.

## Design for Lares

### Two parts

**`lares-awdl`** is a small C program that runs the AWDL link. It needs `CAP_NET_ADMIN` (interface modes, channels, the TAP, neighbour entries) and `CAP_NET_RAW` (raw 802.11 frames), so it is the only part with privileges, and it does nothing else: no files, no TLS, no parsing beyond 802.11 and AWDL. It links nothing but libc (netlink, packet sockets, `timerfd` and `epoll` by hand), because its timing is tight (a 3 TU guard inside 16 TU windows) and its memory must stay small. It starts only while sharing is on, puts the card back as it was on exit, and repairs the card at its next start if it ever crashed. Planned budget: under 1 MB of its own memory, idle between frames.

**Lares itself** does everything above the link, without privileges: the Bluetooth LE advertisement through BlueZ, mDNS on `awdl0`, TLS through GIO, HTTP, plists, archives and the files. It is GLib-only code in `src/airdrop/`, unit-tested on plain data in `make check`, and it runs only while the Share section is open or receiving is on.

### Permission

Like the HyperX rule, an **Allow** button asks for the password once. It installs the `lares-awdl` built with Lares as `/usr/local/libexec/lares-awdl`, owned by root and runnable only by the user's group (mode 0750), with file capabilities `cap_net_admin,cap_net_raw+ep`, so Lares can start it without a password each time. Lares compares the installed copy with its own and asks again after an update. `make uninstall` says how to remove it. `lares-awdl` keeps only those two capabilities, tells NetworkManager to leave the card alone for the session (`nmcli device set … managed no`), records the card's state in a journal under `$XDG_RUNTIME_DIR` first, and puts everything back on exit, or at its next start after a crash. With ufw active, it also opens TCP 8770 to 8779 (the receiver's ports) and UDP 5353 (mDNS) on `awdl0` only, since ufw's default policy would keep nearby devices from reaching the receiver. It ends by itself when Lares closes its standard input, so Lares ending in any way gives the card back. What it reports goes to `~/.cache/lares/lares-awdl.log`, on this PC only.

The mDNS host name is `lares-` and the session's service ID, never the machine's own name, as owly does: strangers nearby don't learn it, and avahi, which answers for the machine's name, can't clash with it. avahi also starts answering on `awdl0` once it exists, announcing the machine's own services to AWDL peers; adding `deny-interfaces=awdl0` to `/etc/avahi/avahi-daemon.conf` stops that.

### In the window

A **Share** section, shown when a card that can run AWDL is present:

- *Receive* turns receiving on for 10 minutes, as the iPhone does, with a countdown. An incoming transfer shows the sender's name and the files, with **Accept** and **Decline**; files go to `~/Downloads`, never overwriting.
- *Send* lists nearby Apple devices in Everyone mode. Pick files (or drop them on the section) and a device; a progress bar follows the upload.
- A short note tells the user to set the iPhone to *Everyone for 10 minutes*, and the section says which card it uses and how fast it can be on it.

### Lightness

Nothing runs while sharing is off. Measured: `lares-awdl` idles at about 240 kB of its own memory and 0 CPU ticks in 12 s; a transfer adds about 2.7 MB to Lares itself, with a sender and a receiver in one process. TLS connections are made with no certificate database, since none is ever checked (TLS-2): otherwise GIO loads the system's, 6.5 MB, and proxy lookup is off for link-local peers, which saves 2.6 MB of the desktop's proxy libraries. While it's on: `lares-awdl` sends about 9 PSF and 15 MIF frames a second and wakes for each AWDL frame it hears; Lares wakes only for mDNS and the transfer itself. Archives stream through: nothing is held whole in memory, and received files are written to a staging folder and moved into place only once complete (SEC-1).

### Finding devices quickly

On a card fixed to one channel, an iPhone is there only in the slot it gives that channel, one in sixteen on channel 6, about 65 ms a second. Anything sent outside it is lost, so finding each other is about meeting in that slot:

- `lares-awdl` holds multicast (mDNS queries, answers and announcements) for a slot a valid peer spends on its channel, and sends identical copies queued for it once. owly sends multicast in any slot on such a card, which an iPhone hears about one time in sixteen. Until a peer's schedule is known, multicast goes at once, as before.
- While looking, Lares asks for `_airdrop._tcp` once a second instead of backing off to every 10 s, and tries `/Discover` at each AWDL peer's address once a second (DISC-5) with a short timeout, so a lost SYN is replaced by a fresh connection rather than waiting out the kernel's 1 s, then 3 s, backoff.
- Picking files starts the link, the search and the Bluetooth advert at once, so the 2.7 s `lares-awdl` takes to set the card up, and the phone's wake-up, pass while the files are chosen.
- The link stays up a minute after its last use, so a second send or receive doesn't wait for it again.

`~/.cache/lares/share.log` records when each step happened, in seconds from the start (no names or addresses), on this PC only.

### Security

Anyone in radio range can reach a receiver, so every input is hostile until checked (owly's SEC rules): strict HTTP framing, plist and archive limits, no `..`, absolute paths, links or devices in archives, no overwriting, and names shown escaped. The hashed contact identifiers in Bluetooth adverts and records can be brute-forced back to phone numbers and emails (PrivateDrop, USENIX Security 2021; still unfixed as of 2026, reported), so Lares never logs or stores them.

## Status

| Part | Code | Checked |
| :--- | :--- | :--- |
| Plists, CPIO, gzip and DVZip, HTTP | `src/airdrop/{bplist,cpio,dvzip,http,names}.c` | Unit tests with owly's and Apple-style vectors, under ASan and UBSan; a million mutated inputs |
| mDNS and the Bluetooth LE wake-up | `src/airdrop/{mdns,ble}.c` | Unit tests; a live publish and browse between two interfaces in a network namespace; BlueZ calls against a stand-in BlueZ |
| Receiver and sender over TLS | `src/airdrop/session.c` | Whole transfers between Lares' own sender and receiver on loopback: discover, files and folders, gzip and DVZip, a name clash, a decline |
| AWDL link | `src/awdl/`, `lares-awdl` | Unit tests on frames from the iPhone 17 capture (addresses and names replaced), and on multicast held for the phone's slot; `awdl0`, capabilities, peers and the crash journal in a network namespace; idle at about 240 kB of its own memory, 0 CPU ticks in 12 s |
| Lares with lares-awdl | `src/share.c` | Lares' Share model against `lares-awdl`'s test mode in a network namespace: the link comes up, the receiver binds to `awdl0`'s address, a photo goes through to Downloads, and the link and `awdl0` go away when receiving stops |
| Share section | `src/share.c`, `src/share_view.c` | Dry run in a hidden GNOME Shell, light and dark, every state |
| On Windows, through WSL | `src/sharecli/main.c`, `win/wsl/`, `win/src/share_win.cpp` | The link comes up from the Windows window and the adapter goes back to Windows afterwards; no Apple device yet (`docs/WINDOWS.md`) |
| With an iPhone | | Checked by the owner with an iPhone 17 on the USB adapter: a file each way. Finding each other took several seconds before the changes under *Finding devices quickly* |

## Code we can reuse

| Project | Language | Licence | Use in Lares |
| :--- | :--- | :--- | :--- |
| [owly](https://github.com/t4t5/owly) | Rust | MIT | The specification and the reference for `lares-awdl` and `src/airdrop/`, ported to C with its copyright notice kept |
| [OWL](https://github.com/seemoo-lab/owl) | C | GPL-3.0 | Read only: archived, and its code would make Lares GPL-3.0 |
| [OpenDrop](https://github.com/seemoo-lab/opendrop) | Python | GPL-3.0 | Read only: unmaintained since 2021 and broken with current iOS |

Wi-Fi Aware (NAN) is not a way in. Apple's AirDrop still uses AWDL (reported), and the EU's Digital Markets Act decision of March 2025 opens iOS to third-party *apps* that share like AirDrop, not AirDrop itself to other devices. A Lares companion app for iOS would be a separate project.

## Decisions

1. **Name of the section.** *Share*, with AirDrop named in its text, keeps Apple's trademark out of the sidebar.
2. **The Allow step.** Installing a binary with file capabilities under `/usr/local` is the first thing Lares puts outside the user's home besides udev rules. It is root-owned, limited to the user's group, and holds two capabilities.
3. **A dual-band adapter.** The current adapter is enough to build and test with, at about 35 kB/s from the iPhone. A dual-band card from owly's working list is needed for AirDrop to feel like it does on Apple devices.

## Sources

- owly: specification, driver table and open questions (`docs/SPEC.md`, `crates/owly-radio/drivers.toml`, `docs/open-questions.md`), commit `21171d8`, 2026-11-11.
- OWL and OpenDrop source (commits `0ee3263` and `11fe7ba`) and their issue trackers.
- Stute et al., *One Billion Apples' Secret Sauce*, MobiCom 2018; *A Billion Open Interfaces for Eve and Mallory*, USENIX Security 2019.
- Heinrich et al., *PrivateDrop*, USENIX Security 2021.
- Apple Support, [AirDrop settings](https://support.apple.com/en-us/119857), and the Apple Platform Security guide, *AirDrop security*.
- Google Security Blog, [Android Quick Share support for AirDrop](https://blog.google/security/android-quick-share-support-for-airdrop-security/), November 2025.
- European Commission, DMA.100203 decision, 19 March 2025.
