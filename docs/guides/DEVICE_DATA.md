# Device data and NAND backups

This guide covers phone-specific inputs and the read-only internal NAND reader.
Select the exact phone in the [target index](../../targets/README.md) and use
its [source-checkout loading procedure](LOADING.md) for live operations.
Preparation is available only from a source checkout.

## Local fitted device data

The current targets declare fitted device-data groups that must come from the
exact physical phone. Bluetooth firmware and FM settings are delivered through
the root filesystem. The fitted audio profile is built into the kernel image;
it holds the headphone, speaker and combined-output gains, the stock equalizer
and ALC processing for each of those outputs and, on phones that vibrate
through their speaker, the vibrate tone. FPLinux does not download or
supply these phone-specific inputs.

Each group is independently optional. When a complete group is absent, the
build keeps that feature's generic behavior: Bluetooth and FM remain
unavailable, headphone audio uses the generic volume levels, and no speaker
output is created. If any part of a group is present, the complete group must
pass its declared size and digest checks; a partial, damaged or mismatched
group fails the build.

Normal `build` and `run` commands consume only already prepared local data.
They do not read the phone's NAND.

A target may also declare the `board-maps` group, which its platform extracts
from the phone's stock firmware in the same NAND backup; a target created by
`./fplinux target new` declares it. The group holds the loader pin map
`pinmap.bin` and keymap `keymap.bin`. Their declarations omit the size, because
each phone's maps have their own. The loader does not read them and takes its
maps only from the target's asset lock. Preparing the group
also writes the board report `reports/board-maps/board-report.json` into the new
generation and prints its path. The report lists the board values found in the
stock firmware, where each came from, the values that were not found and the
decisions left to a person; builds never read it. Preparation builds the
required host extraction tool in the pinned build environment. Preparing from a
saved backup does not require a target bundle or connect to the phone. Each
platform describes what its report contains; see
[UMS9117 board data](../../platforms/ums9117/README.md#board-data-from-the-stock-firmware).

## Prepare device data

Prepare every group declared by `<target>` from one complete physical NAND
backup. This prepares Bluetooth, FM, the independent audio gain group and, when
declared, the board maps.
The fitted data must come from the exact phone selected by `<target>`; do not
reuse data from another handset or model. No manual extraction, renaming or
patching is needed. Preparation requires this source checkout, not a standalone
archive.

The complete command syntax is:

```sh
./fplinux device-data prepare TARGET [--from-dump PATH] [--jobs N] [--offline] [--events PATH]
```

Start with the phone powered off and USB disconnected, then run:

```sh
./fplinux device-data prepare <target>
```

The command first builds and starts the default RAM system. Wait
until its loader asks for the phone; only then hold its boot key and connect the
powered-off phone. This is the normal loader-first sequence in
[Loading from a source checkout](LOADING.md#connect-the-phone); see
the selected phone's instructions in the [target index](../../targets/README.md)
for the target-specific key.

For a backup already saved from this exact physical NAND, use:

```sh
./fplinux device-data prepare <target> --from-dump PATH
```

This form does not require an existing target build, build a loader or connect
to the phone. It builds the required extraction utility in the pinned build
environment. `--offline` uses the locally available environment and source
archives without downloading them. `--events PATH` is
only available for live preparation and cannot be combined with `--from-dump`;
its [loader events](LOADING.md#loader-progress-events) describe the RAM load. `--jobs N` limits
parallel work when the read-only loader is built, and `--offline` requests that
build without network access.

The backup must contain the selected phone's complete physical NAND, with each
page's main bytes followed by its OOB bytes. Its page layout comes from the
geometry receipt `PATH.json` that [`nand backup`](#save-a-nand-backup) writes
beside it. The receipt must describe exactly the bytes at `PATH`. A target that
declares its NAND chip also accepts a backup without a receipt; when both are
present, they must report the same chip and page size. Without either, the
command refuses the backup instead of inferring a layout from its length.
Preparation interprets only 2048 main bytes per page, 64 pages per block and
65536 pages in total. The command also rejects an unsupported target, a length
that does not match the layout, damaged required data, and ambiguous
selected-block mappings. A saved input and its receipt remain unchanged at their
original paths.

The dump, extracted originals, prepared groups, and any image containing them
are private and non-redistributable unless you have the necessary rights. They
are local inputs and are not supplied by the source checkout or its pinned build
environment. The live reader is read-only: it does not mount, erase, restore,
or otherwise write the phone's NAND or NV storage.

When preparation finishes, it prints the exact next commands:

```sh
./fplinux build <target>
./fplinux run <target>
```

Build the selected profile, then end the preparation RAM session with the
selected target's shutdown procedure
and disconnect USB. For the prepared RAM load, start `run` with the phone
again powered off and disconnected; wait for its loader invitation before
holding its boot key and connecting it.

## Save a NAND backup

For a phone already running the selected build, save its complete physical-page
stream with:

```sh
./fplinux nand backup <target> PATH [--profile NAME]
```

The command first opens the phone's raw NAND reader, which identifies the
fitted chip, and reads the geometry the reader reports. It stops before reading
any NAND page when the running kernel does not identify the chip, or when the
chip or its page size differs from the one the target declares. The expected
image size comes from the reported geometry, not from the target.

The command checks that the complete stream has that size, computes a SHA-256
digest and atomically publishes the file with mode `0600`. It then atomically
writes the geometry receipt `PATH.json` beside it, also with mode `0600`. The
receipt is a JSON object with these fields:

| Field                            | Value                                                 |
| -------------------------------- | ----------------------------------------------------- |
| `id_bytes`                       | chip ID bytes as hexadecimal, manufacturer byte first |
| `chip`                           | chip name reported by the reader                      |
| `page_main_bytes`, `oob_bytes`   | main and OOB bytes per page                           |
| `pages_per_block`, `block_count` | pages per erase block and number of blocks            |
| `raw_bytes`                      | size of the saved image                               |
| `sha256`                         | SHA-256 digest of the saved image                     |
| `target`                         | target used for the backup                            |

Saving to the same `PATH` replaces the previous backup. An incomplete transfer
leaves an existing destination and its receipt unchanged. Once the complete
stream is checked, the command removes the old receipt before replacing the
backup. If writing the new receipt fails, the complete new backup remains
without a receipt; a target that declares its NAND chip can still use it for
device-data preparation. The digest identifies the saved bytes; the command
does not compare them with a second device read. Restoring NAND is unsupported.
Treat the backup as private device data. The receipt holds only the fields above
and no NAND contents; it can be shared, for example attached to an issue report.

## Identify the NAND chip

To see what the reader reports without saving a backup, run:

```sh
./fplinux nand identify <target> [--profile NAME]
```

It opens the reader the same way and prints the reader's report unchanged, one
`key=value` line each for `id_bytes`, `chip`, `page_main_bytes`, `oob_bytes`,
`pages_per_block`, `block_count`, `raw_bytes`, `geometry_source`, `feature_a0`,
`feature_b0` and `feature_c0`. For a chip that the running kernel does not
identify, the report shows `chip=unknown`, zero geometry values and
`geometry_source=unknown`, together with the chip's ID bytes and feature
register values.
