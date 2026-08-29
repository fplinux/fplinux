# Suspend on Nokia 3210 4G (TA-1618)

The default RAM-boot system supports Linux s2idle. It preserves the running
session in memory, but it is not deep suspend-to-RAM and does not power down the
SoC or DRAM.

Enter s2idle from a root shell:

```sh
echo freeze > /sys/power/state
```

The display and USB gadget turn off before sleep. A short press of the red
handset key wakes the phone. The USB gadget reconnects after wake, and a display
that was active before sleep turns on again. A display that was already blank
stays blank.

The red handset key is the only supported wake source. The `8` key and the
matrix keypad do not wake the phone.

An active [vibration](VIBRATION.md) is stopped before sleep and is not resumed
after wake.

This path is qualified without a microSD card installed. Suspend with a data
card or the [microSD system root](../profiles/microsd-uboot/features/MICROSD.md)
is not supported yet. Unmount and remove a data card before entering s2idle.

Reboot and deep suspend-to-RAM are not supported.
