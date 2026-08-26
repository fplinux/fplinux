# <SoC / platform name>

<!-- Copy this file to platforms/<platform>/README.md and replace placeholders. -->

## Identity

| Field                   | Value                               |
| ----------------------- | ----------------------------------- |
| Vendor                  | `<vendor>`                          |
| SoC / family            | `<part numbers>`                    |
| Architecture            | `<architecture>`                    |
| Linux platform symbol   | `<CONFIG_ARCH_...>`                 |
| DTS compatible          | `<vendor,soc>`                      |
| Reference documentation | `<public links or provenance note>` |

## Scope

Describe the SoC-wide facilities the platform provides and the phone-specific
work that remains in every target. `platform.toml` records the platform inputs
used by the shared build and RAM-run workflow; targets supply only validated
board data to that workflow.

Keep panel choice, keypad wiring and keymap, board memory layout, board assets,
and target bootstrap inputs out of the platform.

## Shared hardware support

Use **Supported**, **Partial**, **Not supported** and **Unknown** with the same
meaning as the [target template](../../docs/porting/TARGET.md). This table
describes shared implementation and hardware evidence; it does not qualify a
complete phone runtime closure. Bundled installable APKs join that runtime in
the executable payload used for release qualification.

| Block                | Status     | Target-facing contract or limitation |
| -------------------- | ---------- | ------------------------------------ |
| CPU / SMP            | `<status>` | `<what a target configures>`         |
| Interrupt controller | `<status>` | `<interrupt numbering or limits>`    |
| Timers               | `<status>` | `<input clocks or limits>`           |
| Clock controller     | `<status>` | `<fixed clocks or missing pieces>`   |
| GPIO / pin control   | `<status>` | `<board assignment boundary>`        |
| UART                 | `<status>` | `<available instances>`              |
| USB controller       | `<status>` | `<host/device and inherited state>`  |
| DMA                  | `<status>` | `<addressing or mode constraints>`   |
| Display controller   | `<status>` | `<target-owned panel contract>`      |
| Keypad controller    | `<status>` | `<target-owned wiring and keymap>`   |
| Audio controller     | `<status>` | `<codec or board boundary>`          |
| SPI / I2C            | `<status>` | `<available buses>`                  |
| Watchdog / reset     | `<status>` | `<reboot or power behaviour>`        |

## Target requirements

State only the stable rules a target must satisfy: which SoC DTS nodes it may
enable, prerequisites inherited from bootstrap or firmware, and which board data
must remain target-owned. Do not repeat target manifests, build recipes or
implementation file maps here.

## Known constraints

- `<cold-init versus inherited state>`
- `<address-width or DMA limitation>`
- `<unsupported controller modes>`
- `<required target-side setup>`

## Targets using this platform

| Target path         | Phone     | Enabled shared capabilities |
| ------------------- | --------- | --------------------------- |
| `targets/<target>/` | `<phone>` | `<capabilities>`            |

Replace the placeholder with a real target link. A platform
capability can be physically validated without qualifying every target's full
executable payload.
