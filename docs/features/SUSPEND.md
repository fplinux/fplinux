# Suspend

FPLinux uses Linux s2idle on targets and profiles where suspend is supported. Check
the selected phone's documentation, or `README.txt` in a standalone archive,
for supported storage conditions and wake sources before sleeping. s2idle
preserves the running system, but it is not deep suspend-to-RAM and does not
power down the SoC or DRAM.

Enter s2idle from a root shell:

```sh
echo freeze > /sys/power/state
```

The display and USB gadget turn off before sleep. Use the phone's documented
wake-capable power key or an armed normal [RTC alarm](RTC.md) to wake it. The USB gadget
reconnects after wake, and a display that was active before sleep turns on
again. A display that was already blank stays blank.

The matrix keypad is not a supported wake source. Keypad keys pressed and
released during sleep are not reported after wake.

On phones with the shared vibrator interface, an active vibration is stopped
before sleep and is not resumed after wake.

A mounted data card, active card-backed swap in the RAM profile, and the
[microSD system root](../guides/MICROSD_ROOT.md) can remain in use across s2idle
where the target documentation lists that storage condition as supported. Keep the card installed
throughout sleep and wake. Suspend is not a safe-removal or shutdown procedure.

[Bluetooth](BLUETOOTH.md#suspend) can remain powered
with pairings retained; follow its connection and wake limits before sleeping.

A sleep request fails while the [FM radio](FM_RADIO.md) receiver is on, from
its first tune until `/dev/radio0` is closed; this includes `fplinux-fm scan`
and `fplinux-fm play`. After an FM command fails or times out, sleep requests
keep failing until a cold boot.

Reboot and deep suspend-to-RAM are not supported.
