# Source formats

Use the [shared format and check commands](../../guides/BUILDING.md#format-source).
Both commands calculate the same canonical bytes. Formatting publishes selected
files after every tool succeeds; checking compares the result without writing
the source checkout. File permissions, unselected files and the source inventory
are preserved.

Canonical order groups related fields by their role. Alphabetical order applies
only to independent dictionaries identified below. Unknown fields and optional
fields keep their values; missing fields are not added. Comments stay with their
field or section, and license headers stay at the beginning. Formatting does not
repair semantic errors in fixtures or add hardware support.

Arrays and execution sequences retain their order except for the two TOML cases
explicitly listed below. This includes patches, copies, appends, tool recipes,
object and link lists, workflow jobs and steps, issue questions and choices,
navigation, plugins, extensions and palettes.

## TOML

Ordinary fields precede child tables. Child tables stay with their owner, and a
nested array of tables stays with its parent record. Scalar values, hexadecimal
spelling and template placeholders are preserved; Taplo owns spacing and wrapping.

Document sections use this order; absent sections are skipped:

| Document         | Section order                                                                                                                                                   |
| ---------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Target           | `platform`, identity, NAND, display brightness, rootfs, bundle, Linux, bootstrap, adapter, device data, board maps, Bluetooth, audio profile, FM radio, microSD |
| Platform         | identity, rootfs, bundle, U-Boot, Linux, bootstrap, runtime, host                                                                                               |
| Profile          | `name`, Linux root, rootfs, bootstrap, U-Boot, FIT, layout, storage, runtime                                                                                    |
| Source lock      | Linux, bootstrap source, host source and files, transfer source and files                                                                                       |
| Alpine lock      | release/branch/architecture/triplet, repositories, minirootfs, runtime, runtime additions, sysroot, package catalogue                                           |
| Container lock   | Kern, OCI                                                                                                                                                       |
| Asset lock       | each source followed by its outputs                                                                                                                             |
| Release manifest | image, bundle files, runtime files, documents                                                                                                                   |

Target fields follow their consumer roles:

| Table              | Field order                                                                                                                  |
| ------------------ | ---------------------------------------------------------------------------------------------------------------------------- |
| identity           | `brand`, `product`, `hardware_codes`, `compatible`                                                                           |
| nand               | `raw_device`, `id`, `raw_page_bytes`                                                                                         |
| display_brightness | `backlight`, `levels`                                                                                                        |
| rootfs, bundle     | `packages`                                                                                                                   |
| linux              | `config_fragment`, `memory`, `dtb`, `debug_dtb`, `patches`, `forbidden_dtb_markers`, `forbidden_config`, `copies`, `appends` |
| memory             | `base`, `size`                                                                                                               |
| copy, append       | `source`, `destination`                                                                                                      |
| bootstrap          | `lcd_config`, `image`, `map`, `dtb_destination`, `record_prefix`                                                             |
| adapter            | `spi_mode`, `lcd_id`, `backlight_channels`, `backlight_level`, `exec_distance`, `session_name`, `boot_instructions`          |
| device_data        | `parser`                                                                                                                     |
| firmware record    | `source`, `destination`, `size`, `sha256`                                                                                    |
| microsd            | `linux_patches`, bootstrap, U-Boot                                                                                           |
| microsd.bootstrap  | `source`, `image`, `map`                                                                                                     |
| microsd.uboot      | `defconfig`, `patches`, `copies`                                                                                             |

Platform identity starts with `vendor`, `soc`, `aliases`, `compatible`. Linux
fields group source and toolchain, configuration, destinations and outputs, then
patches, copies, appends and configuration checks. Bootstrap fields group source
and destination paths, files and patches, toolchain and build settings, load
limits, copies and memory layout. Runtime groups the load address, adapter and
USB identities. Host runtime tools precede tool recipes; recipe members precede
nonempty copy tables. Empty copy arrays remain ordinary fields.

Profile fields group root kind/filesystem/wait, package selection, boot kinds,
resident and load ranges, storage partitions, then runtime transport and
runnability. Their package arrays retain their order.

Independent source-file hash dictionaries, verified-release entries, spelling
identifier/word dictionaries, Alpine repositories and runtime additions use
case-sensitive lexical key order. Target and platform rootfs/bundle package sets
use package-name order; Alpine package records use filename order. Other arrays,
including profile packages and Alpine runtime/sysroot packages, retain their order.

REUSE annotations retain their record and path order. Each annotation uses
`path`, `precedence`, copyright, then license. Ruff configuration groups target
version, line length, cache and exclusions, then lint and format settings. Mypy
uses Python version, strictness, search path and cache path. Per-file lint rules
retain their order.

## JSON and YAML

JSON descriptions group schema, identity/version, description/type/license,
contact and repository links, engines/package manager, scripts, dependency maps,
hashes and settings. Dependency, hash and settings dictionaries use lexical key
order. Arrays keep their order. JSONC comments are retained. The npm-owned
`package-lock.json` remains outside source formatting.

YAML bindings use `$id`, `$schema`, title, maintainers, description, select,
`allOf`, properties, pattern properties, required fields, dependencies, additional
or unevaluated properties, then examples. Literal examples and scalar types are
preserved.

Workflows group name, trigger, permissions, concurrency, environment/defaults
and jobs. Job settings group dependencies and conditions, execution environment,
permissions, environment and execution options, inputs/outputs, then steps.
Steps group name/id/condition, `uses` or `run`, inputs, working directory, shell,
environment and error/time limits. Job and step sequences are preserved.

Issue forms group name, description, title and assignment settings, then the
body. Each item uses type, id, attributes and validations; attributes group label,
description, placeholder/value, rendering and choices.

MkDocs groups site identity, repository links, directories and validation, theme,
extra settings and assets, extensions, plugins, navigation and exclusions.
Ordered mappings inside these settings retain their order. YAML field reordering
uses block mappings without aliases. Prettier owns JSON and YAML layout.

## Devicetree and other text

Devicetree properties group `device_type`, `compatible`, `reg`, `ranges`, common
properties, device-specific properties and `status`. Names inside a group use
natural order. Complete property statements move together; values and arrays
retain their bytes. Includes, preprocessing, node declarations, node additions
and overrides retain their position. Ambiguous syntax remains in place.

APKBUILD declarations, shell commands, sources, subpackages and checksums retain
their order. Makefile recipes, assignments, object and link lists also retain
their order; Kconfig choices, defaults and help text are preserved. These formats
and assembly receive only safe final-newline normalization. INI and EditorConfig
declarations receive trailing-space normalization while section precedence and
values remain unchanged.

C and headers use the existing Clang style with include sorting disabled.
Python uses Ruff's selected import-order fix and formatter. Recognized shell
scripts keep their declared dialect and use shfmt.

## Phone documents and templates

Phone READMEs group identity, status, features, applications, hardware interfaces,
device data, loading and target-specific use, session termination and release
limits. Feature and application rows follow the shared
[target document contract](../../porting/TARGET.md); a row moves together with its
links, support state and phone-specific note. Optional sections are not created.

Supported `.in` templates use the same rules as their rendered format. Placeholder
text is preserved. Target creation keeps the canonical template order and applies
the same phone-document ordering after substitution, including table widths for
the resulting identity strings.
