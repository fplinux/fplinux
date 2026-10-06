# Dependency snapshots

Preserve the exact external inputs and build environment for a matching source
checkout. Complete [host setup](BUILDING.md#requirements-and-setup) first when
the environment is not available.

## Preserve build dependencies

Keep a dependency snapshot outside the checkout's disposable `.cache` directory:

```sh
./fplinux dependencies create ../fplinux-inputs
./fplinux dependencies verify ../fplinux-inputs
```

`create` selects the exact external inputs declared by the current locks,
package recipes and environment configuration. It verifies each original
download against its declared checksum and size, stores shared bytes once under
their SHA-256, and saves the current build environment through Kern's image
export. Run `setup` first when that environment is unavailable. `--offline`
requires every declared input to be present locally. Repeat `--from PATH`
to supply original downloads from additional local files or directories; only
bytes matching a declared checksum are accepted. Missing files are reported by
their exact declaration and URL, without selecting a replacement version.

The selected inputs also include the documentation site's locked Python wheels
for CPython 3.14 on Ubuntu 24.04, Linux x86-64 with glibc. Other site platforms
and Python versions are outside this selection.

The snapshot's `manifest.json` records the original URLs, checksums, sizes,
selected inputs and environment state. Its identity reflects the parsed external
declarations and verified input bytes. Comments, mapping key order and unrelated
driver changes do not create another external input set. Meaningful sequence
order remains part of the identity. Each snapshot directory holds one set; use
another directory to retain a different set. Existing snapshot contents are
verified before reuse.

For a matching source checkout, restore inputs and the saved environment without
downloading:

```sh
./fplinux dependencies restore ../fplinux-inputs
./fplinux doctor
```

The complete snapshot is verified before any input is restored. Restoration
requires the checkout's current external declarations to match and restores
only their declared cache destinations. Loading an environment additionally
requires its exact image recipe and checks its embedded state through Kern.
The saved environment includes installed-file metadata. Restoration preserves
the measured bytes, links, numeric owners and permission bits, and checks the
result before making it available for builds. Filesystem timestamps, extended
attributes and hardlink relationships are outside this comparison.
This restores the build environment; it does not restore compiled phone APKs,
root filesystems, kernels or successful build receipts.
An existing cache file with different bytes is reported and retained for
inspection before restoration writes any inputs.

Use `create --inputs-only` to omit the environment export. To recreate an
environment from its original inputs, restore only those inputs and request an
offline setup:

```sh
./fplinux dependencies restore ../fplinux-inputs --inputs-only
./fplinux setup --offline --force
```

Phone data extracted from a physical backup and private package-signing keys are
separate local inputs. Dependency snapshots do not include them. A successful
snapshot verification establishes stored-input integrity; it does not establish
a complete offline phone build or reproducible output bytes.
