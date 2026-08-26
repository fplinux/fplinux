# Charger status on Nokia 3210 4G (TA-1618)

This page applies only to Nokia 3210 4G (TA-1618). FPLinux reports whether
external charger input is present through the standard Linux power-supply
class.

## Interface

Read:

```sh
cat /sys/class/power_supply/ta1618-charger/online
```

`1` means external charger input is detected; `0` means it is not detected.

This is connection status, not proof that the battery is charging. The
interface does not enable, disable or configure charging and does not report a
charge rate or battery level. External charger input also prevents the
supported [power-off](POWER_OFF.md) path.
