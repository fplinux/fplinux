# Charger status

FPLinux reports whether external charger input is present through the standard
Linux power-supply class. The selected
[target's documentation](../../targets/README.md) states its charger device
name and hardware limits. A standalone archive carries that status in
`README.txt`.

## Interface

Read the `online` attribute of the target's charger device under
`/sys/class/power_supply/`.

`1` means external charger input is detected; `0` means it is not detected.

This is connection status, not proof that the battery is charging. The
interface does not enable, disable or configure charging and does not report a
charge rate or battery level. External charger input also prevents the
[power-off](POWER_OFF.md) path on targets where it is supported.
