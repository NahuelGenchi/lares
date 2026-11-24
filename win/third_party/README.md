# Vendored code for lares-share.exe

Built into `lares-share.exe` only (AirDrop on Windows, issue #109), never into `lares.exe` or `lares-pad.exe`, and not part of the Linux build. Each keeps its own licence beside it. Nothing here was changed: files were copied as they are, and only the parts listed were kept.

| Library | Version | Source | Licence | What's kept | Used for |
| :--- | :--- | :--- | :--- | :--- | :--- |
| lwIP | 2.2.1 (`STABLE-2_2_1_RELEASE`, commit `77dcd25a72509eb83f72b033d219b1d40cd8eb95`) | https://github.com/lwip-tcpip/lwip | BSD-3-Clause (`lwip/COPYING`) | `src/core` (no IPv4), `src/core/ipv6`, `src/api`, `src/include` | IPv6 link-local, UDP for mDNS and TCP for the transfers, on the AWDL link, with no Windows network adapter. Its configuration and Windows port are Lares' own, in `win/share/net/` |
| zlib | 1.3.1 (commit `51b7f2abdade71cd9bb0e7a373ef2610ec6f9daf`) | https://github.com/madler/zlib | zlib (`zlib/LICENSE`) | the library's C files and headers | gzip and DVZip, the compressions of AirDrop's `/Upload` |

The Realtek firmware `rtl8188eufw.bin` is in `win/share/firmware/` with its own licence (`LICENCE.rtlwifi_firmware.txt`).
