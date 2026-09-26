# Phone targets

Each target is one exact phone variant. Its documentation is the source of
truth for hardware support, safe use and limitations; a successful source build
does not by itself demonstrate working phone hardware or release readiness.
Names and machine identifiers follow the shared
[identity contract](../docs/reference/IDENTITY.md).

| Target               | Device                  | Platform                                    | Device documentation                                       |
| -------------------- | ----------------------- | ------------------------------------------- | ---------------------------------------------------------- |
| `inoi-240-modern-4g` | INOI 240 Modern 4G      | [`ums9117`](../platforms/ums9117/README.md) | [Read support and use notes](inoi-240-modern-4g/README.md) |
| `inoi-244-modern-4g` | INOI 244 Modern 4G      | [`ums9117`](../platforms/ums9117/README.md) | [Read support and use notes](inoi-244-modern-4g/README.md) |
| `nokia-ta1618`       | Nokia 3210 4G (TA-1618) | [`ums9117`](../platforms/ums9117/README.md) | [Read support and use notes](nokia-ta1618/README.md)       |

## Status and common limits

In a phone table, **Hardware** is **Present**, **Absent**, **Unknown**, or
**N/A** for a software-only capability. **Unknown** does not mean absent.
In both the feature and application tables, **FPLinux** is **Supported** only
after the capability has been exercised on that exact phone, **Partial** when a
stated limit or untested physical boundary remains, **Not supported** when the
current target has no supported path, and **Unknown** when support on that
phone has not been established.

All current targets share these limits:

- A cold FPLinux start requires USB loading; the stock boot chain is unchanged.
  The default profile uses a RAM root. The `microsd-uboot` profile uses a
  persistent microSD root on supported targets.
- Internal phone storage is not writable. A supported target may expose a
  fixed-command read-only physical NAND backup, not a mounted filesystem.
- USB operates as a peripheral; USB host and OTG are not supported.
- Calls, SMS, mobile data, Wi-Fi, camera and indicator LEDs have no supported
  FPLinux path.
- Audio support is target-specific: the phone document states its
  [headphone](../docs/features/HEADPHONE_AUDIO.md),
  [speaker](../docs/features/SPEAKER_AUDIO.md),
  [microphone](../docs/features/MICROPHONE_AUDIO.md) and
  [FM radio](../docs/features/FM_RADIO.md) support.
- Bluetooth support is target-specific and uses the same interfaces in both
  global profiles; follow the phone document for support and limitations.
- Linux reboot is not supported. Suspend and vibration support are
  target-specific and documented by the exact phone where available.

Hardware presence still belongs to each phone table. For example, a missing
camera driver does not prove that a camera is physically absent.

## Shared documentation

Shared guides, feature pages and application pages own behavior that does not
change between phones; the project
[documentation index](../README.md#documentation) lists them. Read the selected
target document for its boot key and safe way to end the session. Target support
does not by itself make an executable payload release-ready; see
[Release archives](../docs/guides/RELEASES.md).

## Adding a target

For a new UMS9117 phone, follow [the new phone route](../docs/porting/NEW_PHONE.md).
New target documentation starts from the [phone target template](../docs/porting/TARGET.md).
Keep target documents focused on exact-phone support and differences.
Put reusable SoC behavior in the [platform documentation](../platforms/README.md),
and keep implementation detail in code.
