# Alpine userspace

FPLinux uses Alpine Linux for the phone root filesystem, installable bundle
packages and service management. This directory owns FPLinux userspace packages
and the policy that selects how they are delivered.

Target-neutral host and runtime tools and their contracts belong in
[`common/`](../common/README.md). Reusable pre-Linux components belong in
[`bootstrap/`](../bootstrap/README.md), SoC integration in
[`platforms/`](../platforms/README.md), and board support in
[`targets/`](../targets/README.md).

## Contributor contract

- Keep each userspace component in one conventional aport under `aports/`.
- Put runtime and build dependencies in its APK metadata.
- Use OpenRC for service ownership and supervision.
- Declare platform and target package selection in the corresponding manifests.
  Use `[rootfs].packages` for packages installed in the standard root filesystem
  and `[bundle].packages` for installable APKs published beside the image. Do not
  select packages from target names in code.
- Keep shared C sources under `lib/fplinux/` and headers under `include/fplinux/`
  at the repository root. Map each aport
  consumer explicitly in `scripts/fplinux_cli/alpine_state.py`; do not copy the
  same implementation into multiple aports.
- Do not add rootfs overlays, duplicate package recipes, target-local copies, or
  ad-hoc installers for software that already belongs in an aport.
- Rebuild an upstream component in its own `fplinux-` aport only when the
  locked Alpine build drags a runtime closure the RAM root filesystem cannot
  afford. Install only the runtime files its consumer loads and drop the
  replaced Alpine artifacts from `alpine.lock.toml`; `fplinux-libical` and
  `fplinux-glib` replace `libical` and `glib` for the BlueZ daemons this way,
  and `fplinux-apk-tools` replaces the minirootfs `apk-tools` together with its
  OpenSSL libraries.
- Add a hardware-specific package only when its public interface is genuinely
  specific to that hardware. Shared kernel and userspace interfaces belong in a
  shared package.

`alpine.lock.toml` pins the Alpine inputs used by a build. The normal FPLinux
build installs rootfs-selected APKs into the ARM root filesystem. Bundle-selected
APKs are built and published under `apks/`, but are not installed in that root
filesystem. Contributors do not need a separate target-local packaging workflow.

See [Building FPLinux](../docs/guides/BUILDING.md) for host setup and source
checks, [TyrQuake](../docs/apps/TYRQUAKE.md) and
[MicroPythonOS](../docs/apps/MICROPYTHONOS.md) for the published APKs, and the
project [documentation index](../README.md#documentation) for the remaining user
and contributor guides. Project-owned sources follow the
[code style](../docs/reference/CODE_STYLE.md). C sources use the
[phone-userspace](../docs/reference/style/C.md#phone-userspace) or
[embedded-adapter](../docs/reference/style/C.md#code-embedded-into-another-project)
rules for their runtime.
