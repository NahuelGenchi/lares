#!/usr/bin/env python3
"""Read-only HID++ 2.0 probe for Logitech devices paired to a receiver.

Lists each paired device's HID++ 2.0 features and reads its lighting,
brightness and onboard-profile state through getter functions only. Every
request passes through READS, an allowlist of feature functions that only
read; anything else is refused before it reaches the device. Unit IDs and
serial numbers are never printed.

The receiver's HID++ interface is root-only unless a udev rule opens it:

    sudo python3 -I tools/hidpp-probe.py [--node /dev/hidrawN] [--index N]

Written for Lares from the HID++ 2.0 behaviour documented by Logitech and
observed in the Linux kernel's hid-logitech-hidpp driver; no code is copied.
"""

import argparse
import glob
import os
import select
import sys
import time

SHORT, LONG = 0x10, 0x11
SHORT_LEN, LONG_LEN = 7, 20
# Software ID in the low nibble of each request's function byte. The kernel
# uses 1, notifications 0; OpenRGB 7, LGSTrayEx 0xA, Solaar 0xB, G HUB 0xD,
# firmware 0xF. 0xC is free, so replies to Lares can't be mistaken for theirs.
SW_ID = 0x0C
TIMEOUT = 1.0

NAMES = {
    0x0000: "Root", 0x0001: "Feature set", 0x0002: "Feature info",
    0x0003: "Firmware info", 0x0005: "Device name and type",
    0x0007: "Friendly name", 0x0008: "Keep alive", 0x0020: "Config change",
    0x0021: "Unique random ID", 0x0030: "Target software",
    0x0040: "Wireless signal strength", 0x00C1: "DFU control (legacy)",
    0x00C2: "DFU control (signed)", 0x00C3: "DFU control",
    0x00D0: "DFU", 0x1000: "Battery status", 0x1001: "Battery voltage",
    0x1004: "Unified battery", 0x1010: "Charging control",
    0x1300: "LED control", 0x1500: "Forced pairing", 0x1800: "Generic test",
    0x1801: "Manufacturing mode", 0x1802: "Device reset",
    0x1803: "GPIO access", 0x1805: "OOB state",
    0x1806: "Configurable device properties", 0x1811: "Device reset (2)",
    0x1813: "Test (internal)", 0x1814: "Change host", 0x1815: "Hosts info",
    0x1830: "Power modes", 0x1890: "RF test", 0x1891: "RF test (2)",
    0x18A1: "LED test", 0x18B1: "Monitor mode", 0x1981: "Backlight (legacy)",
    0x1982: "Backlight", 0x1983: "Backlight (3)", 0x1990: "Illumination",
    0x1A00: "Presenter control", 0x1B00: "Reprogrammable keys",
    0x1B04: "Reprogrammable controls v4", 0x1BC0: "Persistent remappable action",
    0x1C00: "Persistent remappable action (2)", 0x1D4B: "Wireless device status",
    0x1DF0: "Remaining pairings", 0x1DF3: "Internal", 0x1E00: "Hidden features",
    0x1E01: "Manage hidden features", 0x1E02: "Hidden features (2)",
    0x1EB0: "Internal", 0x1F1F: "Firmware properties", 0x1F20: "ADC measurement",
    0x40A0: "Fn inversion", 0x40A2: "Fn inversion with default",
    0x40A3: "Fn inversion (3)", 0x4100: "Encryption", 0x4220: "Lock key state",
    0x4301: "Solar keyboard dashboard", 0x4520: "Keyboard layout",
    0x4521: "Disable keys", 0x4522: "Disable keys by usage",
    0x4530: "Dual platform", 0x4531: "Multi-platform",
    0x4540: "Keyboard international layouts", 0x4600: "Crown",
    0x6010: "Touchpad firmware items", 0x6011: "Touchpad software items",
    0x6012: "Touchpad Win8 firmware items", 0x6100: "Touchpad raw XY",
    0x8010: "G-keys", 0x8020: "M-keys", 0x8030: "MR (macro record)",
    0x8040: "Brightness control", 0x8051: "Logitech modifiers",
    0x8060: "Report rate", 0x8061: "Extended report rate",
    0x8070: "Colour LED effects", 0x8071: "RGB effects",
    0x8080: "Per-key lighting", 0x8081: "Per-key lighting v2",
    0x8090: "Mode status", 0x8100: "Onboard profiles", 0x8110: "Mouse button spy",
    0x8111: "Latency monitoring", 0x8120: "Gaming attachments",
    0x8123: "Force feedback", 0x8300: "Sidetone", 0x8310: "Equalizer",
    0x8320: "Headset out",
}


def get_sub(params):
    """Functions that read or write by a sub-operation byte: 0 is the get."""
    return len(params) > 0 and params[0] == 0x00


ANY = lambda params: True  # noqa: E731

# feature -> {function: check on the parameters}. Only these are ever sent.
READS = {
    0x0000: {0: ANY, 1: ANY},              # getFeature, getProtocolVersion
    0x0001: {0: ANY, 1: ANY},              # getCount, getFeatureId
    0x0003: {0: ANY, 1: ANY},              # getDeviceInfo, getFwInfo
    0x0005: {0: ANY, 1: ANY, 2: ANY},      # getCount, getName, getType
    0x1004: {0: ANY, 1: ANY},              # getCapabilities, getStatus
    0x1814: {0: ANY},                      # getHostInfo
    0x1B04: {0: ANY},                      # getCount
    0x4521: {0: ANY, 1: ANY},              # getCapabilities, getDisabledKeys
    0x8040: {0: ANY, 1: ANY, 3: ANY},      # getInfo, getBrightness, getIllumination
    0x8060: {0: ANY, 1: ANY},              # getReportRateList, getReportRate
    0x8061: {1: ANY, 2: ANY},              # getReportRateList, getReportRate
    0x8070: {0: ANY, 1: ANY, 2: ANY, 0xE: ANY},  # info, zone, zone effect, current effect
    0x8071: {0: ANY, 3: get_sub, 5: get_sub},    # getInfo, nvConfig get, swControl get
    0x8080: {0: ANY},                      # getInfo
    0x8081: {0: ANY},                      # getInfo
    0x8100: {0: ANY, 2: ANY, 4: ANY, 5: ANY, 0xB: ANY},  # descr, mode, profile, memoryRead, dpi index
}

ERR20 = {1: "unknown", 2: "invalid argument", 3: "out of range", 4: "hardware error",
         5: "internal", 6: "invalid feature index", 7: "invalid function", 8: "busy",
         9: "unsupported"}
ERR10 = {1: "invalid sub-id", 2: "invalid address", 3: "invalid value", 4: "connect failed",
         5: "too many devices", 6: "already exists", 7: "busy", 8: "unknown device",
         9: "device not reachable (asleep or off)", 0xA: "request unavailable",
         0xB: "invalid parameter", 0xC: "wrong PIN"}


class HidppError(Exception):
    pass


def hexs(b):
    return " ".join(f"{x:02x}" for x in b)


class Device:
    def __init__(self, fd, index):
        self.fd = fd
        self.index = index
        self.features = {0x0000: 0}

    def drain(self):
        while select.select([self.fd], [], [], 0)[0]:
            os.read(self.fd, 64)

    def call(self, feature, function, *params):
        checks = READS.get(feature, {})
        if function not in checks or not checks[function](params):
            raise HidppError(f"refused: 0x{feature:04x} function {function} is not a known read")
        if feature not in self.features:
            raise HidppError(f"0x{feature:04x} not present")
        fidx = self.features[feature]
        fn = (function << 4) | SW_ID
        if len(params) <= 3:
            msg = bytes([SHORT, self.index, fidx, fn, *params]).ljust(SHORT_LEN, b"\0")
        else:
            msg = bytes([LONG, self.index, fidx, fn, *params]).ljust(LONG_LEN, b"\0")
        self.drain()
        os.write(self.fd, msg)
        end = time.monotonic() + TIMEOUT
        while True:
            left = end - time.monotonic()
            if left <= 0 or not select.select([self.fd], [], [], left)[0]:
                raise HidppError("no answer")
            r = os.read(self.fd, 64)
            if len(r) < 5 or r[0] not in (SHORT, LONG) or r[1] != self.index:
                continue
            if r[2] == fidx and r[3] == fn:
                return r[4:]
            if r[2] == 0xFF and r[3] == fidx and r[4] == fn:
                raise HidppError(f"error {r[5]}: {ERR20.get(r[5], '?')}")
            if r[2] == 0x8F and r[3] == fidx and r[4] == fn:
                raise HidppError(f"receiver error {r[5]}: {ERR10.get(r[5], '?')}")


def find_receiver():
    for path in sorted(glob.glob("/sys/class/hidraw/hidraw*")):
        try:
            with open(f"{path}/device/uevent") as f:
                ev = dict(l.strip().split("=", 1) for l in f if "=" in l)
            with open(f"{path}/device/report_descriptor", "rb") as f:
                rd = f.read()
        except OSError:
            continue
        if ev.get("DRIVER") != "logitech-djreceiver":
            continue
        if b"\x85\x10" in rd and b"\x85\x11" in rd and b"\x06\x00\xff" in rd:
            return path
    return None


def children(sysdir):
    """Paired devices under the receiver: (index, name), from sysfs."""
    out = []
    for d in sorted(glob.glob(f"{sysdir}/device/0003:046D:*")):
        try:
            with open(f"{d}/uevent") as f:
                ev = dict(l.strip().split("=", 1) for l in f if "=" in l)
        except OSError:
            continue
        phys = ev.get("HID_PHYS", "")
        if ":" in phys.rsplit("/", 1)[-1]:
            out.append((int(phys.rsplit(":", 1)[-1]), ev.get("HID_NAME", "?"),
                        ev.get("HID_ID", "?").split(":")[-1][-4:], ev.get("DRIVER", "?")))
    return out


def try_(label, fn):
    try:
        return fn()
    except HidppError as e:
        print(f"  {label}: {e}")
        return None


def probe(dev):
    print(f"\n=== Device index {dev.index}")
    v = try_("protocol", lambda: dev.call(0x0000, 1, 0, 0, 0x5A))
    if v is None:
        print("  Not answering. Press a key on it to wake it and run again.")
        return
    print(f"  HID++ protocol {v[0]}.{v[1]} (ping {'ok' if v[2] == 0x5A else 'mismatch'})")

    r = try_("feature set", lambda: dev.call(0x0000, 0, 0x00, 0x01))
    if r is None or r[0] == 0:
        return
    dev.features[0x0001] = r[0]
    count = dev.call(0x0001, 0)[0]
    print(f"\n  Features ({count + 1}, index: id version type name)")
    print(f"    00: 0x0000 v? {'':4} Root")
    for i in range(1, count + 1):
        f = dev.call(0x0001, 1, i)
        fid, ftype, fver = (f[0] << 8) | f[1], f[2], f[3]
        dev.features.setdefault(fid, i)
        flags = "".join(c for bit, c in ((0x80, "O"), (0x40, "H"), (0x20, "E"), (0x10, "M")) if ftype & bit)
        print(f"    {i:02x}: 0x{fid:04x} v{fver} {flags:4} {NAMES.get(fid, 'unknown')}")
    print("    (O obsolete, H hidden, E engineering, M manufacturing-deactivatable)")

    if 0x0005 in dev.features:
        n = dev.call(0x0005, 0)[0]
        name = b""
        while len(name) < n:
            name += dev.call(0x0005, 1, len(name))
        kind = try_("type", lambda: dev.call(0x0005, 2))
        types = {0: "keyboard", 1: "remote control", 2: "numpad", 3: "mouse", 4: "touchpad",
                 5: "trackball", 6: "presenter", 7: "receiver", 8: "headset", 9: "webcam",
                 10: "steering wheel", 11: "joystick", 12: "gamepad", 13: "dock", 14: "speaker",
                 15: "microphone", 16: "illumination light", 17: "programmable controller",
                 18: "car sim pedals", 19: "adapter"}
        print(f"\n  Name: {name[:n].decode(errors='replace')}  Type: "
              f"{types.get(kind[0], kind[0]) if kind else '?'}")

    if 0x0003 in dev.features:
        info = dev.call(0x0003, 0)
        transports = [t for bit, t in ((1, "Bluetooth"), (2, "BLE"), (4, "eQuad/Lightspeed"), (8, "USB"))
                      if (info[6] & bit)]
        models = [f"{info[7 + 2 * k]:02x}{info[8 + 2 * k]:02x}" for k in range(3)]
        print(f"\n  Firmware: {info[0]} entities, transports {', '.join(transports) or hex(info[6])}, "
              f"model IDs {' '.join(models)} (unit ID not shown)")
        kinds = {0: "main app", 1: "bootloader", 2: "hardware", 3: "touchpad", 4: "optical sensor",
                 5: "softdevice", 6: "RF companion", 7: "factory app", 8: "RGB custom effect",
                 9: "motor drive"}
        for e in range(info[0]):
            fw = try_(f"entity {e}", lambda e=e: dev.call(0x0003, 1, e))
            if fw:
                print(f"    {kinds.get(fw[0], fw[0])}: {fw[1:4].decode(errors='replace')} "
                      f"{fw[4]:02x}.{fw[5]:02x} build {fw[6]:02x}{fw[7]:02x}")

    if 0x1004 in dev.features:
        caps = try_("battery caps", lambda: dev.call(0x1004, 0))
        st = try_("battery status", lambda: dev.call(0x1004, 1))
        if st:
            print(f"\n  Battery: {st[0]} %, level flags {st[1]:02x}, charging {st[2]}  "
                  f"(caps {hexs(caps[:2]) if caps else '?'})")

    if 0x8040 in dev.features:
        info = dev.call(0x8040, 0)
        mx, steps, caps, mn = (info[0] << 8) | info[1], info[2], info[3], (info[4] << 8) | info[5]
        cur = dev.call(0x8040, 1)
        print(f"\n  Brightness: {(cur[0] << 8) | cur[1]} (range {mn}..{mx}, steps byte {steps:02x}, "
              f"caps {caps:02x})  raw info {hexs(info[:8])}")
        if caps & 0x04:
            ill = try_("illumination", lambda: dev.call(0x8040, 3))
            if ill:
                print(f"    Illumination: {'on' if ill[0] & 1 else 'off'}")

    if 0x8060 in dev.features:
        lst = try_("report rates", lambda: dev.call(0x8060, 0))
        cur = try_("report rate", lambda: dev.call(0x8060, 1))
        if lst and cur:
            rates = [f"{1000 // (b + 1)} Hz" for b in range(8) if lst[0] & (1 << b)]
            print(f"\n  Report rate: {1000 // cur[0] if cur[0] else '?'} Hz, can do {', '.join(rates)}")
    if 0x8061 in dev.features:
        lst = try_("report rates", lambda: dev.call(0x8061, 1))
        cur = try_("report rate", lambda: dev.call(0x8061, 2))
        print(f"\n  Extended report rate: list {hexs(lst[:2]) if lst else '?'}, current {hexs(cur[:1]) if cur else '?'}")

    if 0x8070 in dev.features:
        info = dev.call(0x8070, 0)
        print(f"\n  Colour LED effects: {info[0]} zones  raw {hexs(info[:8])}")
        for z in range(info[0]):
            zi = try_(f"zone {z}", lambda z=z: dev.call(0x8070, 1, z))
            if not zi:
                continue
            print(f"    zone {z}: location {(zi[1] << 8) | zi[2]:04x}, {zi[3]} effects")
            for e in range(zi[3]):
                ei = try_(f"effect {e}", lambda z=z, e=e: dev.call(0x8070, 2, z, e))
                if ei:
                    print(f"      effect {e}: id {(ei[2] << 8) | ei[3]:04x} caps {(ei[4] << 8) | ei[5]:04x} "
                          f"period {(ei[6] << 8) | ei[7]}")
            cur = try_("current", lambda z=z: dev.call(0x8070, 0xE, z))
            if cur:
                print(f"      current: {hexs(cur[:12])}")

    if 0x8071 in dev.features:
        info = dev.call(0x8071, 0, 0xFF, 0xFF, 0x00)
        clusters, nv = info[2], (info[3] << 8) | info[4]
        print(f"\n  RGB effects: {clusters} clusters, NV caps {nv:04x}, ext caps "
              f"{(info[5] << 8) | info[6]:04x}  raw {hexs(info[:10])}")
        for c in range(clusters):
            ci = try_(f"cluster {c}", lambda c=c: dev.call(0x8071, 0, c, 0xFF, 0x00))
            if not ci:
                continue
            print(f"    cluster {c}: location {(ci[2] << 8) | ci[3]:04x}, {ci[4]} effects, "
                  f"persistence caps {ci[5]:02x}")
            for e in range(ci[4]):
                ei = try_(f"effect {e}", lambda c=c, e=e: dev.call(0x8071, 0, c, e, 0x00))
                if ei:
                    print(f"      effect {e}: id {(ei[2] << 8) | ei[3]:04x} caps {(ei[4] << 8) | ei[5]:04x} "
                          f"period {(ei[6] << 8) | ei[7]}")
        sw = try_("software control", lambda: dev.call(0x8071, 5, 0x00))
        if sw:
            print(f"    Software control: {hexs(sw[:4])} (sub-op, mode, flags)")
        for bit in range(16):
            if nv & (1 << bit):
                cap = 1 << bit
                r = try_(f"NV config {cap:04x}", lambda cap=cap: dev.call(0x8071, 3, 0x00, cap >> 8, cap & 0xFF))
                if r:
                    print(f"    NV config {cap:04x}: {hexs(r[:12])}")

    for fid in (0x8080, 0x8081):
        if fid in dev.features:
            r = try_(NAMES[fid], lambda fid=fid: dev.call(fid, 0, 0x00, 0x00))
            if r:
                keys = sum(bin(b).count("1") for b in r[2:16])
                print(f"\n  {NAMES[fid]}: info {hexs(r[:16])} ({keys} keys in the first bitmap)")

    if 0x8100 in dev.features:
        d = dev.call(0x8100, 0)
        size = (d[7] << 8) | d[8]
        print(f"\n  Onboard profiles: memory model {d[0]}, profile format {d[1]}, macro format {d[2]}, "
              f"{d[3]} profiles ({d[4]} built in), {d[5]} buttons, {d[6]} sectors of {size} bytes, "
              f"layout byte {d[9]:02x}  raw {hexs(d[:12])}")
        mode = try_("mode", lambda: dev.call(0x8100, 2))
        if mode:
            print(f"    Mode: {mode[0]} ({ {1: 'onboard', 2: 'host'}.get(mode[0], '?') })")
        cur = try_("current profile", lambda: dev.call(0x8100, 4))
        if cur:
            print(f"    Current profile: sector {(cur[0] << 8) | cur[1]:04x}")
        for base, label in ((0x0000, "user"), (0x0100, "ROM")):
            raw = b""
            for off in (0, 16):
                r = try_(f"{label} directory", lambda base=base, off=off:
                         dev.call(0x8100, 5, base >> 8, base & 0xFF, 0, off))
                if not r:
                    break
                raw += r[:16]
            if not raw:
                continue
            print(f"    {label} directory: {hexs(raw)}")
            if raw[:4] in (b"\0\0\0\0", b"\xff\xff\xff\xff"):
                continue
            entries = []
            for k in range(0, len(raw) - 3, 4):
                if raw[k:k + 2] == b"\xff\xff":
                    break
                entries.append(((raw[k] << 8) | raw[k + 1], raw[k + 2]))
            for sector, enabled in entries:
                print(f"      profile sector {sector:04x} {'enabled' if enabled else 'disabled'}")
            if entries and 0 < size <= 4096:
                sector = entries[0][0]
                print(f"    Sector {sector:04x} ({label}, first profile):")
                data = b""
                while len(data) < size:
                    o = min(len(data), size - 16)
                    r = try_(f"read {o}", lambda o=o: dev.call(0x8100, 5, sector >> 8, sector & 0xFF, o >> 8, o & 0xFF))
                    if not r:
                        break
                    data = data[:o] + r[:16]
                for o in range(0, len(data), 16):
                    print(f"      {o:04x}: {hexs(data[o:o + 16])}")

    if 0x1814 in dev.features:
        h = try_("hosts", lambda: dev.call(0x1814, 0))
        if h:
            print(f"\n  Change host: {h[0]} hosts, current {h[1] + 1}")
    if 0x4521 in dev.features:
        c = try_("disable keys", lambda: dev.call(0x4521, 0))
        k = try_("disabled keys", lambda: dev.call(0x4521, 1))
        if c and k:
            print(f"\n  Disable keys: can {c[0]:02x}, disabled {k[0]:02x} "
                  f"(1 Caps Lock, 2 Num Lock, 4 Scroll Lock, 8 Insert, 16 Windows)")
    if 0x1B04 in dev.features:
        n = try_("controls", lambda: dev.call(0x1B04, 0))
        if n:
            print(f"\n  Reprogrammable controls: {n[0]}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--node", help="the receiver's HID++ hidraw node (found by default)")
    ap.add_argument("--index", type=int, help="device index on the receiver (all paired by default)")
    a = ap.parse_args()
    sysdir = None
    if a.node:
        sysdir = "/sys/class/hidraw/" + os.path.basename(a.node)
    else:
        sysdir = find_receiver()
        if not sysdir:
            sys.exit("No Logitech receiver with a HID++ interface found.")
    node = "/dev/" + os.path.basename(sysdir)
    kids = children(sysdir)
    print(f"Receiver HID++ node: {node}; software ID 0x{SW_ID:x}; read requests only")
    for idx, name, pid, drv in kids:
        print(f"  paired: index {idx}, {name} (wireless PID {pid}, {drv})")
    indexes = [a.index] if a.index is not None else [k[0] for k in kids]
    try:
        fd = os.open(node, os.O_RDWR)
    except PermissionError:
        sys.exit(f"{node} is root-only: run with sudo.")
    try:
        for idx in indexes:
            probe(Device(fd, idx))
    finally:
        os.close(fd)


if __name__ == "__main__":
    main()
