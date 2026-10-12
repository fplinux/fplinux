# Third-party notices

The corresponding source snapshot records exact versions, URLs, commits and
hashes in `sources.lock.toml`, `container.lock.toml`, `environment.lock.toml`,
`alpine.lock.toml`, `Containerfile`, package locks, host-runtime locks,
platform and target asset locks and the platform U-Boot source lock. Binary
archives carry the resolved asset lock as `assets.lock.toml`,
plus content receipts and `SHA256SUMS`. Aport `APKBUILD` files pin their own
upstream archives or commits and verify remote and local source members with
SHA-512 sums.

Original FPLinux code and documentation are licensed under `GPL-2.0-only`
unless an individual file carries a different SPDX identifier.

| Component                                | Role                                           | Declared license / provenance                                                                     |
| ---------------------------------------- | ---------------------------------------------- | ------------------------------------------------------------------------------------------------- |
| Linux 7.2.9                              | Target kernel and bootstrap bitmap fonts       | GPL-2.0-only; official kernel.org archive                                                         |
| Alpine Linux 3.24.2                      | Target userspace and APK package base          | Multiple upstream licenses; exact armv7 artifacts pinned in `alpine.lock.toml`                    |
| OpenRC 0.64.1                            | Init, service supervision and runlevels        | BSD-2-Clause; upstream release built by the corresponding FPLinux aport                           |
| Dropbear 2026.94                         | USB-network SSH server                         | MIT; upstream release built by the corresponding FPLinux aport                                    |
| OpenSSH SFTP server                      | SSH file-transfer subsystem                    | SSH-OpenSSH; supplied by the pinned Alpine package set                                            |
| skalibs / utmps                          | Dropbear runtime libraries                     | ISC; supplied by the pinned Alpine package set                                                    |
| zlib                                     | Dropbear compression library                   | Zlib; supplied by the pinned Alpine package set                                                   |
| BlueZ 5.87                               | Bluetooth daemons and `bluetoothctl`           | GPL-2.0-or-later, BSD-2-Clause and MIT; upstream release built by the corresponding FPLinux aport |
| D-Bus 1.16.2                             | Message bus for the Bluetooth services         | AFL-2.1 OR GPL-2.0-or-later; supplied by the pinned Alpine package set                            |
| libical 3.0.20                           | vCard parser for the BlueZ OBEX daemon         | LGPL-2.1-only OR MPL-2.0; upstream release archive built without ICU or glib                      |
| GLib 2.88.1                              | Core library for the BlueZ daemons             | LGPL-2.1-or-later; upstream release archive built without GIO, GObject or introspection           |
| apk-tools 3.0.8                          | Package manager for optional APKs              | GPL-2.0-only; upstream release archive built with Mbed TLS instead of OpenSSL                     |
| Mbed TLS 3.6                             | APK signatures and HTTPS transport             | Apache-2.0 OR GPL-2.0-or-later; supplied by the pinned Alpine package set                         |
| hostapd / wpa_supplicant 2.11            | Wi-Fi access point and station authentication  | BSD-3-Clause; supplied by the pinned Alpine package set                                           |
| iw / wireless-regdb                      | Wi-Fi configuration and regulatory data        | ISC; supplied by the pinned Alpine package set                                                    |
| OpenSSL 3.5.9                            | Cryptography for the Wi-Fi tools               | Apache-2.0; supplied by the pinned Alpine package set                                             |
| libnl3                                   | Netlink interface for the Wi-Fi tools          | LGPL-2.1-or-later; supplied by the pinned Alpine package set                                      |
| pcsc-lite libraries                      | Smart-card support in wpa_supplicant           | BSD-3-Clause AND BSD-2-Clause AND ISC; supplied by the pinned Alpine package set                  |
| libjpeg-turbo 3.2.0                      | Software JPEG reference in `fplinux-jpeg-cpu`  | IJG, with Zlib-licensed SIMD code; upstream release archive linked statically                     |
| ALSA utils 1.2.16                        | Playback, recording and mixer tools            | GPL-2.0-or-later; upstream aplay, arecord and amixer built by the audio aport                     |
| ALSA library 1.2.16.1                    | PCM conversion and mixer interface             | LGPL-2.1-or-later; upstream release built by the corresponding FPLinux aport                      |
| BlueALSA 5.0.0                           | Optional SBC A2DP source playback              | MIT, GPL-2.0-only, LGPL-2.1-or-later, BSD-3-Clause and Zlib; declared by its APKBUILD             |
| SBC library 2.2                          | SBC codec for optional Bluetooth audio         | LGPL-2.1-or-later; declared by its APKBUILD                                                       |
| FFmpeg 9.0.2                             | Optional CPU video decoding                    | LGPL-2.1-or-later and IJG; declared by its APKBUILD                                               |
| libinput / libevdev / mtdev              | Input device event libraries                   | MIT; supplied by the pinned Alpine package set                                                    |
| libudev-zero 1.0.5                       | Device enumeration for libinput and BlueZ      | ISC; upstream release archive with phone input-device tagging                                     |
| mdevd                                    | Uevent rebroadcast for hotplug                 | ISC; supplied by the pinned Alpine package set                                                    |
| xkeyboard-config 2.48                    | Keyboard layout data for applications          | MIT, X11, HPND and xkeyboard-config-Zinoviev notices; `COPYING` shipped in each layout APK        |
| libxkbcommon 1.13.2                      | Keyboard text in the terminal                  | MIT, MIT-open-group, HPND and HPND-sell-variant; shared runtime library                           |
| libdrm 2.4.134                           | DRM/KMS display access                         | MIT; shared runtime library from the upstream release archive                                     |
| libtsm 4.8.0                             | Terminal screen and escape-sequence handling   | MIT and LGPL-2.1-or-later; the hash table carries the LGPL notice                                 |
| Terminus 4.49.1                          | Terminal, brightness control and showcase text | OFL-1.1; upstream release archive                                                                 |
| Noto Sans                                | Documentation website font                     | OFL-1.1; Copyright 2022 The Noto Project Authors                                                  |
| Bash 5.3.20                              | Interactive local shell and Readline editing   | GPL-3.0-or-later; upstream release built by the corresponding FPLinux aport                       |
| OpenWrt uclient, libubox and ustream-ssl | HTTP and HTTPS download client and libraries   | ISC; exact source commits pinned by the `fplinux-uclient` APKBUILD                                |
| CA certificates bundle                   | HTTPS certificate verification                 | MPL-2.0; supplied by the pinned Alpine package set                                                |
| TyrQuake 0.71                            | Quake engine for FPLinux                       | GPL-2.0-or-later; bundled decoders use MIT-0, CC0-1.0 and MIT                                     |
| BusyBox                                  | Shell and base userspace applets               | GPL-2.0-only; supplied by the pinned Alpine package set                                           |
| musl                                     | Target C library                               | MIT; full notice packaged at `licenses/musl/COPYRIGHT`                                            |
| fpdoom bootstrap closure                 | T117 bootstrap, relocation tool and USB helper | The Unlicense; pinned fpdoom source                                                               |
| U-Boot 2026.07                           | RAM second stage and FIT tooling               | GPL-2.0-only; official DENX archive; target binary is embedded in `ramboot.bin`                   |
| libusb                                   | Host USB access                                | LGPL-2.1-or-later; static Linux host tools and the bundled Windows DLL                            |
| `spreadtrum_flash` / `spd_dump`          | Spreadtrum loader transport                    | The Unlicense; pinned upstream source                                                             |
| `fphelper_t117`                          | Stock firmware table reader for board maps     | The Unlicense; pinned spreadtrum_flash source                                                     |
| BCH library                              | Host correction of selected NAND main data     | GPL-2.0-only; Copyright 2011 Parrot S.A.; pinned python-bchlib source                             |
| fpdoom `t117_maps.7z`                    | Firmware-derived T117 phone register-map data  | Pinned fpdoom release mirror (`NOASSERTION`)                                                      |
| `t117_fdl1.bin`                          | T117 first-stage RAM loader                    | The Unlicense; pinned spreadtrum_flash release asset                                              |

The TyrQuake APKBUILD verifies the upstream 0.71 source archive and each local
source or patch through its checked-in SHA-512 sums. Its FLAC, MP3 and WAV
decoders use MIT-0; the minimp3 portions of the MP3 decoder use CC0-1.0; and
stb_vorbis uses MIT. Quake PAK files are separate game data. They are not part
of the source tree, root filesystem, RAM image, source companion or release
archive.

The website's Noto Sans font retains its copyright and Open Font License
notice in `site/public/fonts/OFL.txt`. The generated offline site carries the
font and its notice.

Autonomous Linux archives include Python 3.14.8 and OpenSSH 10.3p1;
Windows archives include Python 3.14.7 and Win32-OpenSSH 10.0.0.0p2-Preview.
Their included license directories retain the notices for these runtimes and
their libraries. The Windows driver helper uses libwdi 1.5.1 under
LGPL-3.0-or-later and includes the redistributable's displayed license.

The Git tree does not contain `spreadtrum_flash` source, `spd_dump`,
`fphelper_t117`, the board map archive, extracted map files, or `t117_fdl1.bin`.
The local build downloads exact pinned inputs, verifies their hashes, and writes
them only below `.cache/`.

The `pinmap.bin` and `keymap.bin` members are model-specific register
initialization data. Pinned maps are extracted from phone firmware and mirrored
by fpdoom; prepared maps come from the owner's phone backup. FPLinux records
their source and exact hashes without assigning a license to them.

Marlin2 wireless protocol definitions include material Copyright (C) 2015
Spreadtrum Communications Inc., under the GNU General Public License version 2.
The original protocol headers credit Keguang Zhang and Jingxiang Li as authors.
Source download: <https://github.com/MotorolaMobilityLLC/kernel-modules-wcn>.
