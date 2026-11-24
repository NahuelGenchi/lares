// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
// Two registers of an RTL8188EU lent to WSL, over USB:
//
//   rtlwake <dev>          before rtl8xxxu loads: releases the MCU IO wrapper
//                          (REG_RSV_CTRL + 1, bit 3), which the Windows driver
//                          leaves held in reset: held, the chip's 8051 never
//                          reports its firmware started, and rtl8xxxu gives up.
//   rtlwake <dev> noretry  once rtl8xxxu has it: sets the retry limit
//                          (REG_RETRY_LIMIT) to 0. rtl8xxxu ignores radiotap's
//                          no-ACK flag and sets 48 retries at probe, so a
//                          unicast frame sent while an iPhone is off the
//                          channel was retried for long enough to hold up every
//                          frame behind it (issue #107).
//   rtlwake <dev> address <aa:bb:cc:dd:ee:ff>
//                          once the link is up: gives the chip AirDrop's
//                          address (REG_MACID). The chip ACKs only frames to
//                          the address it holds, and rtl8xxxu sets one only
//                          for interfaces that aren't monitors: without
//                          this an iPhone's unicast went unacknowledged and
//                          was sent again and again (issue #107).
#include <fcntl.h>
#include <linux/usbdevice_fs.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define REG_MCU_IO     0x001d
#define REG_RETRY_LONG 0x042a   /* REG_RETRY_LIMIT: long limit, then short */
#define REG_MACID      0x0610

static int reg(int fd, int in, uint16_t addr, uint8_t *v)
{
    struct usbdevfs_ctrltransfer c = { in ? 0xc0 : 0x40, 0x05, addr, 0, 1, 500, v };
    return ioctl(fd, USBDEVFS_CONTROL, &c);
}

static int no_retry(int fd)
{
    uint8_t was[2], v[2];
    for (int i = 0; i < 2; i++) {
        uint8_t zero = 0;
        if (reg(fd, 1, REG_RETRY_LONG + i, &was[i]) < 0 || reg(fd, 0, REG_RETRY_LONG + i, &zero) < 0
            || reg(fd, 1, REG_RETRY_LONG + i, &v[i]) < 0) {
            perror("retry limit");
            return 1;
        }
    }
    printf("retry limit: %02x%02x -> %02x%02x\n", was[1], was[0], v[1], v[0]);
    return v[0] || v[1];
}

static int own_address(int fd, const char *text)
{
    unsigned a[6];
    if (sscanf(text, "%2x:%2x:%2x:%2x:%2x:%2x", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]) != 6) {
        fprintf(stderr, "not an address: %s\n", text);
        return 2;
    }
    int same = 1;
    for (int i = 0; i < 6; i++) {
        uint8_t v = (uint8_t)a[i], back = 0;
        if (reg(fd, 0, REG_MACID + i, &v) < 0 || reg(fd, 1, REG_MACID + i, &back) < 0) {
            perror("address");
            return 1;
        }
        same &= back == (uint8_t)a[i];
    }
    printf("chip address: %s\n", same ? "AirDrop's" : "not taken");
    return !same;
}

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    int fd = open(argv[1], O_RDWR);
    if (fd < 0) { perror(argv[1]); return 1; }
    if (argc > 2 && strcmp(argv[2], "noretry") == 0) return no_retry(fd);
    if (argc > 3 && strcmp(argv[2], "address") == 0) return own_address(fd, argv[3]);
    uint8_t v = 0;
    if (reg(fd, 1, REG_MCU_IO, &v) < 0) { perror("read"); return 1; }
    uint8_t was = v;
    v |= 0x08;
    if (reg(fd, 0, REG_MCU_IO, &v) < 0) { perror("write"); return 1; }
    reg(fd, 1, REG_MCU_IO, &v);
    printf("MCU IO wrapper: %02x -> %02x\n", was, v);
    return 0;
}
