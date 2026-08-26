# Porting FPLinux

A phone port is a small, complete description of one hardware variant. It uses
shared components only where the current targets demonstrably share the same
behaviour.

## Layers

| Layer                                           | Owns                                                                                            |
| ----------------------------------------------- | ----------------------------------------------------------------------------------------------- |
| [`bootstrap/`](../../bootstrap/README.md)       | Reusable freestanding components and callback contracts used before Linux starts.               |
| [`platforms/<soc>/`](../../platforms/README.md) | SoC-wide Linux support and the fixed RAM-loader flow shared by its targets.                     |
| [`targets/<phone>/`](../../targets/README.md)   | One phone's board wiring, addresses, panel, keymap, assets, bootstrap inputs and support state. |
| [`alpine/`](../../alpine/README.md)             | Post-kernel userspace, APK recipes, root-filesystem package policy and OpenRC services.         |
| [`common/`](../../common/README.md)             | Target-neutral host and runtime tools and the contracts shared by those tools.                  |

A platform never chooses a phone panel, keypad matrix, memory reservation or
board-specific loader data. A target never duplicates SoC addresses, controller
support or the platform loader sequence.

## Porting checklist

1. Define the target and platform fields according to the shared
   [identity contract](../reference/IDENTITY.md), then start from the
   [target template](TARGET.md) and select an existing platform.
2. Keep board-specific data in the target. Move code to a platform only after
   more than one current target uses the same behaviour.
3. Give the target a clear support table: hardware presence, FPLinux support,
   and physical validation are separate facts.
4. Follow the shared [build workflow](../guides/BUILDING.md) and
   [loader procedure](../guides/LOADING.md). Keep loader ordering and common host
   procedures out of target and platform documents.
5. Exercise every feature labelled **Supported** on the named hardware variant.
   A release requires the exact executable payload to pass the phone gate; a
   successful build or candidate package alone is not release qualification.

Use the [platform template](PLATFORM.md) for reusable SoC support and the
[console port contract](CONSOLE.md) when a target offers the
[local console](../features/LOCAL_CONSOLE.md).

The project [documentation index](../../README.md#documentation) links the user
workflows and the remaining contributor contracts without duplicating them
here. The [C code guide](../reference/C_STYLE.md) covers the language and
lifetime rules for each layer.
