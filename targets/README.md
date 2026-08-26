# Phone targets

Each target is one exact phone variant. Its documentation is the source of
truth for hardware support, safe use and limitations; a successful source build
does not by itself qualify a phone or a release.
Names and machine identifiers follow the shared
[identity contract](../docs/reference/IDENTITY.md).

| Target               | Device                  | Platform                                    | Device documentation                                       |
| -------------------- | ----------------------- | ------------------------------------------- | ---------------------------------------------------------- |
| `inoi-240-modern-4g` | INOI 240 Modern 4G      | [`ums9117`](../platforms/ums9117/README.md) | [Read support and use notes](inoi-240-modern-4g/README.md) |
| `inoi-244-modern-4g` | INOI 244 Modern 4G      | [`ums9117`](../platforms/ums9117/README.md) | [Read support and use notes](inoi-244-modern-4g/README.md) |
| `nokia-ta1618`       | Nokia 3210 4G (TA-1618) | [`ums9117`](../platforms/ums9117/README.md) | [Read support and use notes](nokia-ta1618/README.md)       |

## Status and common limits

In a phone table, **Hardware** is **Present**, **Absent**, **Unknown** or **N/A**.
**Unknown** does not mean absent. **FPLinux** is **Supported** only after the
feature has been exercised on that exact phone, **Partial** when a stated limit
or qualification gap remains, and **Not supported** when the current target has
no supported path.

All current targets share these limits:

- Linux runs only in volatile RAM; there is no autonomous Linux boot path.
- Internal phone storage is deliberately not exposed.
- USB operates as a peripheral; USB host and OTG are not supported.
- Audio, calls, SMS, mobile data, Bluetooth, Wi-Fi, camera, indicator LEDs and
  vibration have no supported FPLinux path.
- Linux reboot and suspend are not supported.

Hardware presence still belongs to each phone table. For example, a missing
camera driver does not prove that a camera is physically absent.

Shared guides own workflows that do not change between phones:

- [Building FPLinux](../docs/guides/BUILDING.md) covers source setup, checks and builds.
- [Loading from a source checkout](../docs/guides/LOADING.md) covers USB access, RAM loading,
  reconnecting and verification. Read the selected target document for its boot
  key and safe way to end the session.
- [Using a standalone archive](../docs/guides/STANDALONE.md) covers the bundled
  runner and offline documentation shipped with a package.
- [Release archives](../docs/guides/RELEASES.md) defines candidates, qualification and
  releases. Target support does not by itself qualify an executable payload.

Feature and application documents own behavior shared by the current targets:

- [Local console](../docs/features/LOCAL_CONSOLE.md)
- [USB networking](../docs/features/USB_NETWORKING.md)
- [SSH access](../docs/features/SSH.md)
- [File transfer](../docs/features/FILE_TRANSFER.md)
- [Host keyboard forwarding](../docs/features/HOST_KEYBOARD.md)
- [CPU clock reporting](../docs/features/CPU_CLOCK.md)
- [TyrQuake](../docs/apps/TYRQUAKE.md)
- [MicroPythonOS](../docs/apps/MICROPYTHONOS.md)

New target documentation starts from the [phone target template](../docs/porting/TARGET.md).
Keep target documents focused on exact-phone qualification and differences.
Put reusable SoC behavior in the [platform documentation](../platforms/README.md),
and keep implementation detail in code.
