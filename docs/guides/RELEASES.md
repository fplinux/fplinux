# Release archives

FPLinux archives are target-specific standalone bundles for Linux x86-64 hosts,
made from a successful local build. Every current archive begins through the
same volatile RAM loader as a source checkout; a selected boot mode may then use
additional removable-media artifacts bundled with its candidate.

## Current availability

This checkout has no prebuilt release archive and no recorded qualified payload.
A candidate is for physical qualification only and must not be published as a
release.

## Create a candidate

Build the exact target as described in [Building FPLinux](BUILDING.md), then
package it:

```sh
./fplinux build <target>
./fplinux package <target> --candidate
```

The candidate ZIP is written below `.cache/out/candidates/`. Its filename and
included notice identify it as a qualification candidate. Packaging validates the
selected build and does not rebuild it.

A release archive can be created only after the exact executable payload from a
candidate has completed phone-specific qualification:

```sh
./fplinux package <target>
```

Until then, this command refuses to create a release archive. A successful
release archive is written below `.cache/out/releases/`.

## Qualification boundary

A candidate proves that the source checkout produced a packageable bundle. It
does not prove that the target boots or that any hardware feature works.
Qualification covers the RAM runtime and bundled APKs. Documentation, notices,
checksums and build metadata remain outside that phone-qualified payload but are
still covered by archive integrity checks. A boot-mode candidate also includes
its declared boot artifacts in the qualification payload.

The Nokia microSD system candidate is selected independently of the ordinary
RAM-only target archive:

```sh
./fplinux build nokia-ta1618 --profile microsd-uboot
./fplinux package nokia-ta1618 --boot microsd --candidate
```

Its archive name uses `microsd`, includes the whole-card image from the selected
context and cannot be packaged as a release. The contributor-facing
`--profile microsd-uboot` package command remains available for qualification
work, but it is not the public boot-mode name.

Changing the executable payload requires another phone qualification. A build,
archive checksum or host-side `verify` does not replace that phone test.

The [target index](../../targets/README.md) links every phone's exact support
status, boot key, hardware differences, and shutdown or recovery procedure.

## Archive contents and integrity

An archive contains the RAM image, required host tools and assets, bundled
installable APKs, the standalone runner, local USB rules, shared feature and
application documents, checksums, target instructions, and license notices. It
does not contain build trees, caches, or kernel debug output.

Keep the extracted top-level directory intact. Enter it and verify every listed
file before connecting a phone:

```sh
cd <extracted-top-level-directory>
sha256sum -c SHA256SUMS
```

`SHA256SUMS` protects the extracted archive contents. The archive digest printed
by `./fplinux package` protects the ZIP as a whole when that digest is retained
or published. Neither integrity check is hardware qualification.

After validation, follow [Using a standalone archive](STANDALONE.md). That guide
is the source of truth for host runtime requirements, USB access, loader order
and troubleshooting. The archive's bundled feature documents cover the running
session without depending on a source checkout.
