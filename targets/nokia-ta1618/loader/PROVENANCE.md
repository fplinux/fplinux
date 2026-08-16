# TA-1618 board-asset provenance

The Git tree does not contain these binary board assets. The shared generic
builder reads the pinned `fplinux.assets/v1` entries in `assets.lock.toml`,
verifies each archive and extracted member, and writes generated data only below
`.cache/`.

| Generated file  | Pinned upstream input                                           | SHA256                                                             |
| --------------- | --------------------------------------------------------------- | ------------------------------------------------------------------ |
| `pinmap.bin`    | `nokia_ta1618/pinmap.bin` in fpdoom `1.20251101` `t117_maps.7z` | `e01baf9e82129e58218d2d16f66a89607f93572382945137c21965a95d2e790d` |
| `keymap.bin`    | `nokia_ta1618/keymap.bin` in fpdoom `1.20251101` `t117_maps.7z` | `81368f19a166dcd30e4911713c0290afd34ddd65cdcea6e8485ce3d2980bbace` |
| `t117_fdl1.bin` | spreadtrum_flash `1.20260403` release asset                     | `062d5e1a298c0e378eeefcc67dddba6fd4d6be10a683511c849ec59c17fb7414` |

The fpdoom map archive itself is pinned as
`9295f209f33ab688711ae4496a90d319ae2aef79420a2fd823d1beabd7f2ede1`.
The upstream release asset was refreshed on 2026-08-14 to add INOI 244 maps;
the Nokia member hashes listed above are unchanged in the refreshed archive.
`pinmap.bin` and `keymap.bin` are model-specific register initialization data
originating from phone firmware and mirrored by the pinned fpdoom release.
FPLinux assigns no license to these `NOASSERTION` inputs.

The spreadtrum_flash source repository and its `t117_fdl1.bin` release asset are
published under the Unlicense.
