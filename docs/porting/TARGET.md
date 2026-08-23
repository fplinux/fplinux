# {Device} support

<!--
Copy this file to targets/<target>/README.md. Describe the current target as a
phone user would encounter it. Keep implementation details in code and
platform documentation; keep only device-specific actions, limits and evidence
here.
-->

## Device

| Field    | Value                                                |
| -------- | ---------------------------------------------------- |
| Device   | `{manufacturer} {model} ({variant})`                 |
| Platform | [`{platform}`](../../platforms/{platform}/README.md) |
| Boot     | `{volatile RAM / other supported method}`            |

## Status

In one short paragraph, state what a person can use on this exact phone and
what remains unavailable. Distinguish physical-device evidence from a source
build; neither is a release qualification. State plainly when no prebuilt
archive or qualified executable payload exists.

## Hardware support

Keep hardware presence separate from FPLinux support:

- **Hardware:** **Present**, **Absent**, **Unknown**, or **N/A** for a
  software-only capability. Absence requires evidence for this exact variant;
  a missing driver or DTS node does not prove it.
- **FPLinux:** **Supported** when implemented and exercised on this exact phone,
  **Partial** when an exercised limitation or qualification gap remains,
  **Not supported** when the current target provides no supported path,
  **Unknown** when current support or validation has not been established, or
  **N/A** when the capability does not apply.

Keep this compact row set in every phone document so targets remain comparable.
Add a row only for another user-visible capability of that phone; keep internal
controllers and register-level evidence out of this table.

| Area                        | Hardware     | FPLinux     | What a user can rely on / limit       |
| --------------------------- | ------------ | ----------- | ------------------------------------- |
| RAM boot                    | **N/A**      | `{support}` | `{volatile behavior}`                 |
| Persistent boot             | **N/A**      | `{support}` | `{supported method or absence}`       |
| Local screen                | `{presence}` | `{support}` | `{visible console or limitation}`     |
| Physical keypad             | `{presence}` | `{support}` | `{input path or limitation}`          |
| Keypad backlight            | `{presence}` | `{support}` | `{control or limitation}`             |
| USB shell and file transfer | `{presence}` | `{support}` | `{available interface or limitation}` |
| Host keyboard bridge        | **N/A**      | `{support}` | `{available interface or limitation}` |
| USB host mode               | `{presence}` | `{support}` | `{available role or limitation}`      |
| Removable storage           | `{presence}` | `{support}` | `{mounting or limitation}`            |
| Internal phone storage      | `{presence}` | `{support}` | `{access policy}`                     |
| Audio                       | `{presence}` | `{support}` | `{speaker, headphones, microphone}`   |
| Modem and mobile service    | `{presence}` | `{support}` | `{calls, SMS, and data}`              |
| Bluetooth                   | `{presence}` | `{support}` | `{connectivity}`                      |
| Wi-Fi                       | `{presence}` | `{support}` | `{connectivity}`                      |
| Camera                      | `{presence}` | `{support}` | `{capture path or limitation}`        |
| Battery and charging        | `{presence}` | `{support}` | `{reporting or control}`              |
| Indicator LEDs / vibration  | `{presence}` | `{support}` | `{control or limitation}`             |
| Power-off                   | **N/A**      | `{support}` | `{safe way to end a session}`         |
| Reboot and suspend          | **N/A**      | `{support}` | `{supported behavior or limitation}`  |

## Build and start from source

Follow [Building FPLinux](../../docs/BUILDING.md) for host requirements and
source checks. Build this target from the repository root:

```sh
./fplinux build {target}
```

For a RAM run, follow the shared loader-first connection sequence in
[Building FPLinux](../../docs/BUILDING.md). Record only the details owned by
this target:

- Boot key or sequence: `{target-specific boot key or sequence}`
- Variant-specific timing or release condition: `{target-specific detail or none}`

Do not copy the shared loader procedure into the target document. State any
target-specific persistent-storage limitation in the status or hardware table.

## Use after boot

State the local console behavior and the target's USB interfaces. Use these
source-checkout commands when they apply:

```sh
./fplinux verify {target}
./fplinux console {target}
./fplinux console {target} --keyboard /dev/input/eventN
```

Link to [the console contract](../../docs/porting/CONSOLE.md) and
[file transfer](../../docs/TRANSFER.md) instead of duplicating generic behavior.
Explain any target-specific storage procedure, game controls or other user
workflow below.

## Target-specific use

Keep only actions that differ for this phone: for example, how to mount a card,
where to put game data, an input layout, backlight control or a safe power-off
procedure. Do not include register descriptions, configuration listings,
implementation maps, cache paths or build internals.

## End the RAM session

Document the exact safe exit for this target and how the user returns to the
vendor firmware. State unsupported reboot or power-off behavior plainly.

## Release status

State whether a prebuilt archive and a qualified executable payload currently
exist. If neither exists, say that a locally packaged candidate is not a
release, and link to [Release archives](../../docs/RELEASES.md).
