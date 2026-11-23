// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
// Releases an RTL8188EU's MCU IO wrapper (REG_RSV_CTRL + 1, bit 3), which
// the Windows driver leaves held in reset: with it held the chip's 8051
// never reports its firmware started, and rtl8xxxu gives up.
#include <fcntl.h>
#include <linux/usbdevice_fs.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int reg(int fd, int in, uint16_t addr, uint8_t *v)
{
    struct usbdevfs_ctrltransfer c = { in ? 0xc0 : 0x40, 0x05, addr, 0, 1, 500, v };
    return ioctl(fd, USBDEVFS_CONTROL, &c);
}

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    int fd = open(argv[1], O_RDWR);
    if (fd < 0) { perror(argv[1]); return 1; }
    uint8_t v = 0;
    if (reg(fd, 1, 0x001d, &v) < 0) { perror("read"); return 1; }
    uint8_t was = v;
    v |= 0x08;
    if (reg(fd, 0, 0x001d, &v) < 0) { perror("write"); return 1; }
    reg(fd, 1, 0x001d, &v);
    printf("MCU IO wrapper: %02x -> %02x\n", was, v);
    return 0;
}
