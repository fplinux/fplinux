# Shared Linux integration

Use this contract when adding platform or target Linux inputs. Select a target,
profile and build type as described in [Building FPLinux](../guides/BUILDING.md);
these selectors define the separate compilation context.

## Shared Linux sources

Targets and platforms pinned to the same Linux archive SHA-256 share one
prepared source tree under `.cache/linux/sources/<sha256>/`. Selecting another
target or profile reuses that tree without extracting or copying Linux again.
Source changes update the affected integration files. The cache retains only
the upstream originals needed to rebuild those files, not a second full tree.

Kernel configuration, generated profile inputs and compilation output use
separate Kbuild `O=` directories for each target, profile and build type.
Parallel kernel checks read the same prepared source tree and write to their
own output directories. Sharing sources does not enable other boards' drivers
in the selected image.

All Linux integrations sharing an archive must coexist. Keep board drivers
guarded by their target configuration and use distinct destinations for
board-owned files. Target copies add new board-owned files; use patches or
platform copies to change upstream files. Conflicting file ownership, patches
that cannot apply together and profile-specific Linux patches are rejected.
A missing integration input or invalid shared Kconfig can prevent other targets
from building too.
Source sharing does not establish support for another platform, architecture
or userspace ABI.

## microSD card detection

A slot that permits card replacement during a powered session declares
`cd-gpios` and enables `CONFIG_MMC_REMOVABLE` and `CONFIG_MMC_GPIO`.
For the UMS9117 EIC card-detect input, also enable `CONFIG_GPIO_EIC_SPRD`.
These choices belong to the target configuration.

A slot whose card stays installed throughout the session declares
`non-removable`. Its kernel disables removable-card and MMC GPIO support;
the card is probed when the host starts. Initial discovery, SD transfers,
request recovery and suspend/resume remain available. General GPIO support
for the keypad, camera or PMIC is independent of MMC GPIO support.
