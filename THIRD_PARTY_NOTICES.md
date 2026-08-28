# Third-party notices

The corresponding source snapshot records exact versions, URLs, commits and
hashes in `sources.lock.toml`, `container.lock.toml`, `alpine.lock.toml`,
`Containerfile`, `package-lock.json`, target asset locks and profile-owned source
locks. Binary archives carry the target asset lock as `assets.lock.toml`, plus
content receipts and `SHA256SUMS`. Aport `APKBUILD` files pin their own upstream
archives or commits and verify remote and local source members with SHA-512
sums.

Original FPLinux code and documentation are licensed under `GPL-2.0-only`
unless an individual file carries a different SPDX identifier.

| Component                       | Role                                           | Declared license / provenance                                                   |
| ------------------------------- | ---------------------------------------------- | ------------------------------------------------------------------------------- |
| Linux 6.18.42                   | Target kernel                                  | GPL-2.0-only; official kernel.org archive                                       |
| Alpine Linux 3.24.1             | Target userspace and APK package base          | Multiple upstream licenses; exact armv7 artifacts pinned in `alpine.lock.toml`  |
| OpenRC                          | Init, service supervision and runlevels        | BSD-2-Clause; supplied by the pinned Alpine package set                         |
| Dropbear                        | USB-network SSH server                         | MIT; supplied by the pinned Alpine package set                                  |
| OpenSSH SFTP server             | SSH file-transfer subsystem                    | SSH-OpenSSH; supplied by the pinned Alpine package set                          |
| skalibs / utmps                 | Dropbear runtime libraries                     | ISC; supplied by the pinned Alpine package set                                  |
| zlib                            | Dropbear compression library                   | Zlib; supplied by the pinned Alpine package set                                 |
| TyrQuake 0.71                   | Quake engine for FPLinux                       | GPL-2.0-or-later; bundled decoders use MIT-0, CC0-1.0 and MIT                   |
| MicroPythonOS 0.16.2 stack      | Optional graphical runtime and applications    | GPL-2.0-only, MIT, OFL-1.1, Zlib and bundled custom notices                     |
| BusyBox                         | Shell and base userspace applets               | GPL-2.0-only; supplied by the pinned Alpine package set                         |
| musl                            | Target C library                               | MIT; full notice packaged at `licenses/musl/COPYRIGHT`                          |
| fpdoom bootstrap closure        | T117 bootstrap, relocation tool and USB helper | The Unlicense; pinned fpdoom source                                             |
| U-Boot 2026.07                  | RAM second stage and FIT tooling               | GPL-2.0-only; official DENX archive; target binary is embedded in `ramboot.bin` |
| libusb                          | Host USB access                                | LGPL-2.1-or-later; linked into the static bundled host tools at build time      |
| `spreadtrum_flash` / `spd_dump` | Spreadtrum loader transport                    | The Unlicense; pinned upstream source                                           |
| fpdoom `t117_maps.7z`           | Firmware-derived TA-1618 register-map data     | Pinned fpdoom release mirror (`NOASSERTION`)                                    |
| `t117_fdl1.bin`                 | T117 first-stage RAM loader                    | The Unlicense; pinned spreadtrum_flash release asset                            |

The TyrQuake APKBUILD verifies the upstream 0.71 source archive and each local
source or patch through its checked-in SHA-512 sums. Its FLAC, MP3 and WAV
decoders use MIT-0; the minimp3 portions of the MP3 decoder use CC0-1.0; and
stb_vorbis uses MIT. Quake PAK files are separate game data. They are not part
of the source tree, root filesystem, RAM image, source companion or release
archive.

The MicroPythonOS APKBUILD pins the MicroPythonOS, `lvgl_micropython`,
MicroPython, `micropython-lib`, LVGL, freezeFS and pycparser source commits. It
verifies those archives and every local source or patch with checked-in SHA-512
sums. The resulting APK installs the applicable runtime, font, image-decoder and
FPLinux adapter license texts under
`/usr/share/licenses/fplinux-micropythonos/`.

The Git tree does not contain `spreadtrum_flash` source, `spd_dump`, the board
map archive, extracted map files, or `t117_fdl1.bin`. The local build downloads
exact pinned inputs, verifies their hashes, and writes them only below `.cache/`.

The `pinmap.bin` and `keymap.bin` members originate as model-specific register
initialization data extracted from phone firmware and mirrored by fpdoom.
FPLinux records their source and exact hashes without assigning a license to
them.
