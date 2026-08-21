# Alpine userspace

FPLinux uses Alpine Linux for the phone root filesystem, packages, and service
management. This directory owns FPLinux userspace packages and the policy needed
to assemble them into a rootfs.

## Contributor contract

- Keep each userspace component in one conventional aport under `aports/`.
- Put runtime and build dependencies in its APK metadata.
- Use OpenRC for service ownership and supervision.
- Declare common, platform, and target package selection in the corresponding
  manifests; do not select packages from target names in code.
- Do not add rootfs overlays, duplicate package recipes, target-local copies, or
  ad-hoc installers for software that already belongs in an aport.
- Add a hardware-specific package only when its public interface is genuinely
  specific to that hardware. Shared kernel and userspace interfaces belong in a
  shared package.

`alpine.lock.toml` pins the Alpine inputs used by a build. The normal FPLinux
build composes the selected APKs into the ARM rootfs; contributors do not need a
separate target-local rootfs workflow.

See [Building FPLinux](../docs/BUILDING.md) for host setup and source checks.
