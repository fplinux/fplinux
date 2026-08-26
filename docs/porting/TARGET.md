# {Device} support

<!--
Copy this file to targets/<target>/README.md. Describe the current target as a
phone user would encounter it. Keep implementation details in code and
platform documentation; keep only device-specific actions, limits and evidence
here. Define its manifest fields according to the
[identity contract](../../docs/reference/IDENTITY.md); do not store a second display name.
-->

## Identity

| Field         | Value                                                                             |
| ------------- | --------------------------------------------------------------------------------- |
| Target        | `{target slug}`                                                                   |
| Device        | `{derived display name}`                                                          |
| Hardware code | `{confirmed code or omit this row}`                                               |
| Platform      | `platforms/{platform}/README.md`; see [platform index](../../platforms/README.md) |
| Boot          | `{volatile RAM / other supported method}`                                         |

## Status

In one short paragraph, state what a person can use on this exact phone and
what remains unavailable.

## Features

Keep hardware presence separate from FPLinux support:

- **Hardware:** **Present**, **Absent**, **Unknown**, or **N/A** for a
  software-only capability. Absence requires evidence for this exact variant;
  a missing driver or DTS node does not prove it.
- **FPLinux:** **Supported** when implemented and exercised on this exact phone,
  **Partial** when an exercised limitation or qualification gap remains,
  **Not supported** when the current target provides no supported path,
  **Unknown** when current support or validation has not been established, or
  **N/A** when the capability does not apply.

Keep an explicit status for this exact phone even when it uses shared code.
Link a shared feature or application page instead of copying its commands,
controls or generic limitations. The final column contains only a difference
specific to this phone; use an em dash when there is none. Add another row only
for a user-visible capability, not for an internal controller.

| Feature                                                          | Hardware     | FPLinux     | This phone                     |
| ---------------------------------------------------------------- | ------------ | ----------- | ------------------------------ |
| RAM boot                                                         | **N/A**      | `{support}` | `{target-specific limitation}` |
| Persistent boot                                                  | **N/A**      | `{support}` | `{target-specific limitation}` |
| [Local console](../../docs/features/LOCAL_CONSOLE.md)            | `{presence}` | `{support}` | `{resolution or other delta}`  |
| [USB networking](../../docs/features/USB_NETWORKING.md)          | `{presence}` | `{support}` | `{target-specific delta or —}` |
| [SSH access](../../docs/features/SSH.md)                         | **N/A**      | `{support}` | `{target-specific delta or —}` |
| [File transfer](../../docs/features/FILE_TRANSFER.md)            | **N/A**      | `{support}` | `{target-specific delta or —}` |
| [Host keyboard forwarding](../../docs/features/HOST_KEYBOARD.md) | **N/A**      | `{support}` | `{target-specific delta or —}` |
| [CPU clock reporting](../../docs/features/CPU_CLOCK.md)          | **N/A**      | `{support}` | `{target-specific delta or —}` |
| USB host mode                                                    | `{presence}` | `{support}` | `{available role or limit}`    |
| Removable storage                                                | `{presence}` | `{support}` | `{mounting or limitation}`     |
| Internal phone storage                                           | `{presence}` | `{support}` | `{access policy}`              |
| Audio                                                            | `{presence}` | `{support}` | `{available path or limit}`    |
| Modem and mobile service                                         | `{presence}` | `{support}` | `{calls, SMS and data}`        |
| Bluetooth                                                        | `{presence}` | `{support}` | `{connectivity}`               |
| Wi-Fi                                                            | `{presence}` | `{support}` | `{connectivity}`               |
| Camera                                                           | `{presence}` | `{support}` | `{capture path or limit}`      |
| Battery and charging                                             | `{presence}` | `{support}` | `{reporting or control}`       |
| Indicator LEDs / vibration                                       | `{presence}` | `{support}` | `{control or limitation}`      |
| Power-off                                                        | **N/A**      | `{support}` | `{safe end-of-session path}`   |
| Reboot and suspend                                               | **N/A**      | `{support}` | `{supported behavior}`         |

## Applications

Application behavior and controls belong in the shared page. Keep only the
exact phone's support status and its storage or display difference here.

| Application                                       | FPLinux     | This phone                   |
| ------------------------------------------------- | ----------- | ---------------------------- |
| [TyrQuake](../../docs/apps/TYRQUAKE.md)           | `{support}` | `{storage or display delta}` |
| [MicroPythonOS](../../docs/apps/MICROPYTHONOS.md) | `{support}` | `{storage or display delta}` |

## Load into RAM

Follow [Loading from a source checkout](../../docs/guides/LOADING.md). Record only the
details owned by this target:

- Boot key or sequence: `{target-specific boot key or sequence}`
- Variant-specific timing or release condition: `{target-specific detail or none}`

Do not copy the shared loader procedure into the target document. State any
target-specific persistent-storage limitation in the status or feature table.

## Target-specific use

Keep only actions and limits that differ for this phone: for example a supported
removable-storage lifecycle, a display-size limitation, backlight control or a
safe power-off procedure. Common console controls, application controls, SSH,
file transfer and host-keyboard behavior belong in their feature pages. Do not
include register descriptions, configuration listings, implementation maps,
cache paths or build internals.

Give a substantial target-only function its own file under
`targets/<target>/features/` and link it from the status table. That file owns
the stable user interface and its limits; the target README remains the compact
phone overview.

## End the RAM session

Document the exact safe exit for this target and how the user returns to the
vendor firmware. State unsupported reboot or power-off behavior plainly.

## Release boundary

Explain that target support alone does not qualify an executable payload and
that a locally packaged candidate is not a release. Link to
[Release archives](../../docs/guides/RELEASES.md) for current availability and the
qualification boundary instead of copying that information here.
