# Alpine userspace

FPLinux uses Alpine Linux as its only userspace, package and service-management
layer. The phone kernel, RAM bootstrap, board integration and host tooling are
FPLinux-owned; everything installed into the root filesystem is composed as APK
packages and started through OpenRC.

The rootfs package set is the exact union of the fixed common packages
`fplinux-base`, `fplinux-console`, `fplinux-input` and `fplinux-tyrquake`, the
selected platform's `[rootfs].packages`, and the selected target's
`[rootfs].packages`. Both manifest sections are required; either list may be
empty. Package selection never depends on a target name. Builds with the same
final package set and other composition inputs reuse the same `armv7` rootfs.
Targets do not maintain target-specific rootfs overlays.

## Locked base system

`../alpine.lock.toml` pins the Alpine release, architecture, official minirootfs
and every official APK consumed by the runtime and cross-build sysroot. Each
artifact is identified by its filename, byte count and SHA-256 digest. Normal
builds fetch those exact files into the shared download cache and never resolve a
live package version from an APK index.

The official armv7 minirootfs supplies the initial Alpine filesystem and package
database. FPLinux builds a temporary signed composition repository from the
locked Alpine runtime APKs and locally built FPLinux APKs, installs the resolved
package set by name, verifies package ownership and service layout, normalizes
metadata, and emits the deterministic `rootfs.cpio` consumed by Kbuild. The
world file therefore contains normal package names rather than checksums for
packages installed from arbitrary APK paths.

The resulting rootfs is content-addressed below `.cache/rootfs/`. Its successful
receipt includes the exact package set, the other composition inputs and the
final cpio digest. Targets with the same final package set and composition inputs
reuse the same rootfs bytes.

## FPLinux aports

`aports/` is the canonical home of FPLinux userspace sources and package recipes:

- `fplinux-base` owns FPLinux system policy, `/init`, `inittab`, identity files
  and OpenRC runlevel membership;
- `fplinux-console` builds the physical-keypad console and owns both the local
  console and USB shell OpenRC services;
- `fplinux-input` builds the USB host-keyboard bridge and its OpenRC service;
- `fplinux-cpuclock` provides the CPU-clock diagnostic utility;
- `fplinux-tyrquake` builds TyrQuake and the FPLinux launcher.

The first, second, third and fifth packages are the fixed common set. The
platform manifest owns hardware-family additions: UMS9117 declares
`fplinux-cpuclock`. A target manifest adds only its own packages through its
required `[rootfs]` section.

An APKBUILD is the single package recipe for each component. Do not add a second
package recipe, rootfs overlay copy, post-build installer or target-local copy of
package sources for the same software.

Local source files use real SHA-512 entries in their APKBUILDs. Package creation,
repository indexing, signature verification and dependency resolution are
performed by Alpine's `abuild` and `apk`; FPLinux does not implement its own APK
writer or dependency resolver.

`abuild` uses one persistent local build key below `.cache/apk-signing/`. The
private key never enters the source tree, rootfs or published bundle. Its public
key SHA-256 is a causal rootfs and bundle input, so changing or deleting the
local key cannot silently reuse artifacts signed by another key. The key is for
build-time composition only; a distributable FPLinux package repository may use
a separate maintainer/release key trusted by the running system.

## Cross package build

The pinned amd64 build image is itself Alpine. `abuild` runs in its normal cross
mode with:

```text
CBUILD=x86_64-alpine-linux-musl
CHOST=armv7-alpine-linux-musleabihf
```

The target development ABI is an exact sysroot assembled from the armv7 APKs in
`alpine.lock.toml`. Clang targets `armv7-alpine-linux-musleabihf`, lld performs
the host-side link, and target binutils operations use the pinned ARM tools in
the build image. No QEMU or host binfmt registration is required for the normal
build.

`abuild.conf` contains the one shared Cortex-A7 hard-float compiler policy. Keep
architecture flags there rather than duplicating them across package recipes.

## OpenRC runtime

OpenRC is the only service-management layer. `fplinux-base` installs the
runlevels, while component packages own their `/etc/init.d/` scripts.

The default runlevel contains:

```text
fplinux-input
fplinux-usb-getty
fplinux-console
```

`fplinux-console` is attached directly to `/dev/tty1`. The USB shell is a
supervised getty on `ttyGS0`, and the host-keyboard bridge reads `ttyGS1` and
publishes a uinput keyboard. `supervise-daemon` owns restart policy; there is no
FPLinux-specific init process or getty supervisor loop.

The build rejects an image if key runtime files lose their expected APK owner,
if the OpenRC runlevel is incomplete, if interactive gettys reappear in
`inittab`, or if forbidden FPLinux runtime helper paths are present.

## Program package contract

Each FPLinux userspace program has one conventional aport under
`alpine/aports/<package>/`. Runtime and build dependencies live in APK metadata.
Generic application code and rootfs package selection are independent of phone
target names. The fixed common packages are selected for every rootfs; platform
and target additions come only from their `[rootfs].packages` arrays.

A hardware-specific userspace package is valid only when its interface is truly
target-specific. Stable kernel/device ABIs allow one APK to work on all phones,
as TyrQuake does for fbdev and `--input phone|keyboard`.
