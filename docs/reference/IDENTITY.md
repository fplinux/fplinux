# Target and platform identity

FPLinux keeps build identifiers, public device names, hardware codes and
Devicetree identifiers separate. Directory names own target and platform slugs;
the manifests own platform selection and hardware identity. Generated build
inputs and published metadata derive from those values instead of repeating
them.

## Target identity

Every `targets/<target>/target.toml` selects a platform and defines the exact
device identity:

```toml
platform = "ums9117"

[identity]
brand = "Nokia"
product = "3210 4G"
hardware_codes = ["TA-1618"]
compatible = "nokia,ta-1618"
```

The fields have distinct contracts:

- The `targets/<target>` directory name is the stable lowercase target slug
  used by `./fplinux`, package suffixes and machine-readable manifests.
- `brand` and `product` are the public brand and product names. They do not
  claim the legal identity of the device manufacturer or trademark licensee.
- `hardware_codes` contains only codes established for the exact supported
  variant. An unknown code is represented by an empty list, not a guess.
- `compatible` is the target's exact board-compatible string. It identifies
  the hardware and does not describe FPLinux or the RAM-boot method.

Human identity fields use trimmed printable ASCII with single spaces. Hardware
codes and aliases use uppercase letters, digits, dots, underscores and hyphens;
compatible strings use the lowercase `vendor,device` form. Arrays preserve
their declared order and do not allow duplicate values.

The public display name is derived as `brand + " " + product`. When
`hardware_codes` is non-empty, the codes are appended in parentheses, separated
by a comma and a space. For example, the fields above produce
`Nokia 3210 4G (TA-1618)`.

Candidate and release archive names use the stable `FPLinux-<target>` prefix.
They do not derive a second slug from punctuation or capitalization in the
public display name.

Use the complete display name on first reference in user-facing text. A
hardware code such as `TA-1618` may be used alone on later references.
Normalized forms such as `ta1618` and `TA1618` are reserved for filenames, C
identifiers, Kconfig symbols and other interfaces that cannot use punctuation.

## Platform identity

Every `platforms/<platform>/platform.toml` identifies the reusable SoC layer:

```toml
[identity]
vendor = "Unisoc"
soc = "UMS9117"
aliases = ["T117"]
compatible = "sprd,ums9117"
```

- The `platforms/<platform>` directory name is the lowercase platform slug;
  targets select it through their top-level `platform` field.
- `vendor` and `soc` form the public platform name, here `Unisoc UMS9117`.
- `aliases` records vendor or reference names. An alias is not a second product
  name and is used only where provenance or an external interface requires it.
- `compatible` is the platform fallback compatible.

The `sprd` namespace in Linux and Devicetree is retained where it is already
the kernel ABI. It is not replaced with `unisoc` merely to match public prose.
Likewise, T117 remains in the vendor loader interfaces that require that name,
but it is not used as the FPLinux platform or phone identity.

## Generated consumers

The build derives the following values from the manifest identity:

- runtime and release metadata;
- the bootstrap screen identity;
- the Devicetree root `model` and `compatible` properties;
- the exact root-node Devicetree binding;
- user-facing target names emitted by the host tools.

A target DTS includes `fplinux-target-identity.dtsi` and must not repeat its
root `model` or `compatible`. The generated `compatible` list orders the exact
board compatible first and the platform fallback second.

The target bootstrap `record_prefix` labels diagnostic records only. It does
not participate in the binary handoff. The prefix remains explicit so logs stay
recognizable without deriving a machine identifier from punctuation or
capitalization in the display name.

Do not add another stored display name, copy identity fields into a build
recipe, or encode a phone model in the platform. A new consumer must read the
validated manifest model or a generated artifact derived from it.

Use the [porting overview](../porting/README.md) when adding a target or
platform. The [target index](../../targets/README.md) and
[platform index](../../platforms/README.md) link the current hardware documents.
