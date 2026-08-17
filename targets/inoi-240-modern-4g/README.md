# INOI 240 Modern 4G

This is an experimental, volatile-RAM-only target for the INOI 240 Modern 4G
with the Unisoc UMS9117/T117 SoC (marketed as T107 for this phone).

## Current scope

The target builds a minimal Linux initramfs with a framebuffer console on the
NV3023 panel, the matrix keypad as a Linux input device and two non-ACM USB
serial interfaces. Interface 0 (`ttyGS0`) carries the shell and file transfers;
interface 1 (`ttyGS1`) carries forwarded host-keyboard events into a persistent
uinput keyboard. Audio, modem, Bluetooth, camera, microSD and phone power-off
paths are unavailable.

| Block                | Status                     | Notes                                                   |
| -------------------- | -------------------------- | ------------------------------------------------------- |
| CPU, GIC and timers  | Working on hardware        | Shared UMS9117 support                                  |
| USB device           | Working on hardware        | Shared inherited-state MUSB PIO driver                  |
| USB shell            | Working on hardware        | BusyBox `getty` on `ttyGS0`                             |
| Host keyboard bridge | Working on hardware        | Host evdev through `ttyGS1` to persistent uinput        |
| LCD                  | Working on hardware        | NV3023 profile over the shared LCM/DBI framebuffer core |
| Keypad               | Working on hardware        | Shared matrix driver with the board keymap              |
| Internal flash       | Intentionally inaccessible | No storage node or driver                               |
| microSD              | Not supported              | No storage node or driver                               |

The phone carries 64 MiB of RAM. The bootstrap verifies the detected size
before copying the kernel; Linux maps the full range and reserves the top
1 MiB for the framebuffer.

## TyrQuake

The root filesystem includes the TyrQuake 0.71 engine without Quake game data.
The shared backend reads the RGB565 framebuffer geometry from fbdev and has no
INOI-specific profile. `--input phone` selects the physical keypad through the
stable `fplinux/keypad0` identity; `--input keyboard` selects the FPLinux host
keyboard through its stable virtual input identity. On this panel TyrQuake
renders `320×256`, downsamples by two to `160×128` and rotates that landscape
image into the phone's `128×160` framebuffer.

This target has no microSD driver. Mount a filesystem at `/mnt/card` before
starting the launcher; a volatile tmpfs is suitable for a RAM-only session:

```sh
./fplinux console inoi-240-modern-4g \
  --exec 'mkdir -p /mnt/card && mount -t tmpfs tmpfs /mnt/card && mkdir -p /mnt/card/fplinux/quake/id1'
./fplinux console inoi-240-modern-4g \
  --upload ./pak0.pak /mnt/card/fplinux/quake/id1/pak0.pak
./fplinux console inoi-240-modern-4g --exec 'quake --input phone'
./fplinux console inoi-240-modern-4g --exec 'quake --input keyboard'
```

The engine uses a fixed 32 MiB heap. Keeping a full PAK in tmpfs leaves a narrow
memory margin and no swap. TyrQuake works on physical INOI 240 hardware in both
input modes.

## Build and run

```sh
./fplinux check kernel
./fplinux build inoi-240-modern-4g
./fplinux run inoi-240-modern-4g
```

Power the phone off, hold `*`, then connect USB while keeping the key pressed.
The runner performs only the BootROM FDL1 and volatile payload sequence. It does
not issue flash, erase, partition or NV commands. Linux re-enumerates on USB as
`0525:a4a6`; interface 0 serves the interactive `ttyGS0` shell and interface 1
carries host-keyboard events through `ttyGS1`.
