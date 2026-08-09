# Third-party notices

The corresponding source snapshot records exact versions, URLs, commits and
hashes in `sources.lock.toml`, `container.lock.toml`,
`Containerfile`, `package-lock.json`, and
`targets/nokia-ta1618/loader/assets.lock.toml`. Binary archives carry the target
asset lock as `assets.lock.toml`, plus content receipts and `SHA256SUMS`.

Original FPLinux code and documentation are licensed under `GPL-2.0-only`
unless an individual file carries a different SPDX identifier.

| Component                       | Role                                           | Declared license / provenance                                               |
| ------------------------------- | ---------------------------------------------- | --------------------------------------------------------------------------- |
| Linux 6.18.42                   | Target kernel                                  | GPL-2.0-only; official kernel.org archive                                   |
| Buildroot 2026.05.1             | Toolchain and root filesystem build            | GPL-2.0-or-later with documented exceptions; official buildroot.org archive |
| BusyBox                         | Init and shell applets                         | GPL-2.0-only; selected and built by Buildroot                               |
| musl                            | Target C library                               | MIT; full notice packaged at `licenses/musl/COPYRIGHT`                      |
| fpdoom bootstrap closure        | T117 bootstrap, relocation tool and USB helper | The Unlicense; pinned fpdoom source                                         |
| libusb                          | Host USB access                                | LGPL-2.1-or-later; build and host runtime dependency                        |
| `spreadtrum_flash` / `spd_dump` | Spreadtrum loader transport                    | The Unlicense; pinned upstream source                                       |
| fpdoom `t117_maps.7z`           | Firmware-derived TA-1618 register-map data     | Pinned fpdoom release mirror (`NOASSERTION`)                                |
| `t117_fdl1.bin`                 | T117 first-stage RAM loader                    | The Unlicense; pinned spreadtrum_flash release asset                        |

The Git tree does not contain `spreadtrum_flash` source, `spd_dump`, the board
map archive, extracted map files, or `t117_fdl1.bin`. The local build downloads
exact pinned inputs, verifies their hashes, and writes them only below `.cache/`.

The `pinmap.bin` and `keymap.bin` members originate as model-specific register
initialization data extracted from phone firmware and mirrored by fpdoom.
FPLinux records their source and exact hashes without assigning a license to
them.
