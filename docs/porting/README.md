# Porting FPLinux

A phone port is a small, complete description of one hardware variant. It uses
shared components only where the current targets demonstrably share the same
behaviour.

## Layers

| Layer              | Owns                                                                                            |
| ------------------ | ----------------------------------------------------------------------------------------------- |
| `bootstrap/`       | Reusable freestanding pre-Linux components and their callback contracts.                        |
| `common/`          | Shared post-kernel userspace and host-side tools.                                               |
| `platforms/<soc>/` | SoC-wide Linux support and the fixed RAM-loader flow used by its targets.                       |
| `targets/<phone>/` | One phone's board wiring, addresses, panel, keymap, assets, bootstrap inputs and support state. |

A platform never chooses a phone panel, keypad matrix, memory reservation or
board-specific loader data. A target never duplicates SoC addresses, controller
support or the platform loader sequence.

## Porting checklist

1. Start from the [target template](TARGET.md) and select an existing platform.
2. Keep board-specific data in the target. Move code to a platform only after
   more than one current target uses the same behaviour.
3. Give the target a clear support table: hardware presence, FPLinux support,
   and physical validation are separate facts.
4. Build and package through the public CLI. For a RAM run, start
   `./fplinux run <target>` before connecting the powered-off phone; connect it
   only when the loader asks.
5. Exercise every feature labelled **Supported** on the named hardware variant.
   A release requires the exact executable payload to pass the phone gate; a
   successful build or candidate package alone is not release qualification.

Use the [platform template](PLATFORM.md) for reusable SoC support and the
[console contract](CONSOLE.md) when a target offers the local terminal.
