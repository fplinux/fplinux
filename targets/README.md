# Phone targets

Each target is one exact phone variant. Its documentation is the source of
truth for hardware support, safe use and limitations; a successful source build
does not by itself qualify a phone or a release.

| Target               | Device                  | Platform                                    | Profile   | Device documentation                                       |
| -------------------- | ----------------------- | ------------------------------------------- | --------- | ---------------------------------------------------------- |
| `inoi-240-modern-4g` | INOI 240 Modern 4G      | [`ums9117`](../platforms/ums9117/README.md) | `console` | [Read support and use notes](inoi-240-modern-4g/README.md) |
| `inoi-244-modern-4g` | INOI 244 Modern 4G      | [`ums9117`](../platforms/ums9117/README.md) | `console` | [Read support and use notes](inoi-244-modern-4g/README.md) |
| `nokia-ta1618`       | Nokia 3210 4G (TA-1618) | [`ums9117`](../platforms/ums9117/README.md) | `console` | [Read support and use notes](nokia-ta1618/README.md)       |

Build and run commands are shared across targets; see
[Building FPLinux](../docs/BUILDING.md). Before a RAM run, read the target's
support table and follow its loader-first connection sequence exactly.

New target documentation starts from the [phone target template](../docs/porting/TARGET.md).
Keep target documents focused on what the exact phone can do. Put reusable SoC
behavior in the [platform documentation](../platforms/README.md), and keep
implementation detail in code.
