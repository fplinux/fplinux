# Building FPLinux

FPLinux builds complete phone images from the current source checkout and
pinned upstream inputs. Generated build data is kept under `.cache/`, outside
tracked source files.

## Requirements and setup

- Linux x86-64
- rootless Podman
- Python 3.11 or newer
- network access until the pinned build inputs are available locally
- For a RAM run or reconnect over SSH: `ip` from iproute2, OpenSSH `ssh`,
  `ssh-keygen`, `ssh-keyscan`, and `sftp`, plus a host network manager that
  automatically runs IPv4 DHCP on a newly attached USB-NCM interface.
  NetworkManager is supported.

Check the host before building:

```sh
./fplinux doctor
```

`doctor` checks the host architecture, rootless Podman, and the pinned build
environment. Rootless Podman needs subordinate UID and GID mappings; follow the
[Podman installation guide](https://podman.io/docs/installation) and
[rootless-mode requirements](https://docs.podman.io/en/latest/markdown/podman.1.html#rootless-mode)
for the host distribution.

The first build prepares its required environment automatically. It can also be
prepared explicitly:

```sh
./fplinux setup
```

In a Git checkout, `setup` also selects the repository's commit-message hook.
Commits use `type(scope): subject`; the tracked commitlint configuration is the
source of truth for accepted scopes.

Use `./fplinux setup --force` to rebuild the pinned OCI image even when an image
for the current recipe is already ready.

## Check source

Run the source-quality gate when changing or reviewing source:

```sh
./fplinux check
./fplinux check --list
./fplinux check docs spelling
./fplinux check --no-cache
```

With no scopes, `check` runs the complete gate. Selected cacheable scopes reuse
an exact successful result when their current inputs match; otherwise they run
again. `--no-cache` reruns selected cacheable scopes. This gate is useful for
source work but is not a prerequisite for every build or RAM run.

## Regenerate Alpine checksums

When an Alpine aport source file changes, regenerate its `sha512sums` with the
supported command instead of editing individual digests:

```sh
./fplinux checksum <aport>
```

The command updates only the canonical `APKBUILD` checksum block and refuses to
publish if its declared inputs change while it runs.

After the image and required source archives have been prepared, regeneration
can run without network access:

```sh
./fplinux checksum <aport> --offline
```

`./fplinux checksum` is the sole supported path for regenerating FPLinux aport
checksums. Do not run `abuild checksum` directly in the checkout or manually
replace individual digest lines.

## Build a target

```sh
./fplinux build <target>
./fplinux build <target> --jobs 8
```

`--jobs` limits parallel compilation. A matching selected bundle is reused;
otherwise the command rebuilds it from the current inputs. Target names are
discovered from `targets/`; use the [target index](../targets/README.md) to
choose one.

After an online build has prepared the required inputs, an offline build miss
can run with networking disabled:

```sh
./fplinux build <target> --offline
```

If the required pinned environment is unavailable or stale, an offline build
asks for an online `./fplinux setup` first. A matching bundle remains usable
offline.

Use the target document for its RAM-loader sequence and hardware status:

```sh
./fplinux run <target>
```

Before the first physical run, configure the loader and console permissions in
[USB access](RELEASES.md#usb-access).

Start the loader before connecting the powered-off phone. Wait until it requests
the device, then connect the phone and follow that target's boot-key instructions.

## Logs, cache, and parallel commands

Build and check print compact stage status. Add `--verbose` to stream their tool
output; complete logs are retained under `.cache/logs/` and the command reports
their location on failure.

Public commands serialize writes to shared build state. Target output is kept
under `.cache/out/<target>/`; treat it as generated data, not as a user-managed
workspace.

Inspect cache cleanup candidates before deleting generated data:

```sh
./fplinux prune
./fplinux prune --json
./fplinux prune --apply
```

`prune` without `--apply` is read-only. Unknown, old, or mismatched generated
entries are cache misses and are not migrated.

## Verify a running target

With the selected bundle loaded and its console available:

```sh
./fplinux verify <target>
```

`verify` compares the running kernel identity with the selected local bundle
and refuses a stale local bundle. It confirms that relationship only; it is not
a phone hardware-qualification test.

## What a build proves

A successful build proves that the current checkout produced the selected
bundle. It does not prove that the image boots or that a hardware feature works
on a phone. Target documents record feature-level qualification and limitations.

See [Release archives](RELEASES.md) to create a physical-qualification
candidate. See [Host-to-phone transfer](TRANSFER.md) for working with an already
running target.
