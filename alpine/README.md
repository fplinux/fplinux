# Alpine userspace

FPLinux uses Alpine Linux for the phone root filesystem, installable bundle
packages and service management. This directory owns FPLinux userspace packages
and the policy that selects how they are delivered.

## Contributor contract

- Keep each userspace component in one conventional aport under `aports/`.
- Put runtime and build dependencies in its APK metadata.
- Use OpenRC for service ownership and supervision.
- Declare platform and target package selection in the corresponding manifests.
  Use `[rootfs].packages` for packages installed in the standard root filesystem
  and `[bundle].packages` for installable APKs published beside the image. Do not
  select packages from target names in code.
- Do not add rootfs overlays, duplicate package recipes, target-local copies, or
  ad-hoc installers for software that already belongs in an aport.
- Add a hardware-specific package only when its public interface is genuinely
  specific to that hardware. Shared kernel and userspace interfaces belong in a
  shared package.

`alpine.lock.toml` pins the Alpine inputs used by a build. The normal FPLinux
build installs rootfs-selected APKs into the ARM root filesystem. Bundle-selected
APKs are built and published under `apks/`, but are not installed in that root
filesystem. Contributors do not need a separate target-local packaging workflow.

See [Building FPLinux](../docs/BUILDING.md) for host setup and source checks.
