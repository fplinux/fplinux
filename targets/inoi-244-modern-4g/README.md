# INOI 244 Modern 4G

This is an experimental, volatile-RAM-only target for the INOI 244 Modern 4G
(`F2444G`) with the Unisoc UMS9117/T117 SoC.

## Current scope

The target builds a minimal Linux initramfs with a BusyBox shell on one USB
serial gadget port (`ttyGS0`), a framebuffer console on the NV3030 panel and
the matrix keypad as a Linux input device. Audio, modem, Bluetooth, camera,
microSD and phone power-off paths are unavailable.

| Block               | Status                     | Notes                                                   |
| ------------------- | -------------------------- | ------------------------------------------------------- |
| CPU, GIC and timers | Working on hardware        | Shared UMS9117 support                                  |
| USB device          | Working on hardware        | Shared inherited-state MUSB PIO driver                  |
| USB shell           | Working on hardware        | BusyBox `getty` on `ttyGS0`                             |
| LCD                 | Working on hardware        | NV3030 profile over the shared LCM/DBI framebuffer core |
| Keypad              | Working on hardware        | Shared matrix driver with the board keymap              |
| Internal flash      | Intentionally inaccessible | No storage node or driver                               |
| microSD             | Not supported              | No storage node or driver                               |

The 48 MiB RAM size is taken from published specifications for model F2444G.
The bootstrap checks the detected size before copying the kernel. The hardware
reports 64 MiB, while Linux deliberately exposes the conservative 48 MiB range.

## Build and run

```sh
./fplinux check kernel --no-cache
./fplinux build inoi-244-modern-4g
./fplinux run inoi-244-modern-4g
```

Power the phone off, hold `*`, then connect USB while keeping the key pressed.
The runner performs only the BootROM FDL1 and volatile payload sequence. It does
not issue flash, erase, partition or NV commands. Linux re-enumerates on USB as
`0525:a4a6` and serves an interactive `ttyGS0` shell.
