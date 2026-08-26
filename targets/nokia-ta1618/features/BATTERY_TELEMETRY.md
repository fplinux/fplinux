# Battery telemetry on Nokia 3210 4G (TA-1618)

This page applies only to Nokia 3210 4G (TA-1618). FPLinux exposes live battery
voltage and signed current through the standard Linux power-supply class.

## Interface

```sh
cat /sys/class/power_supply/ta1618-battery/voltage_now
cat /sys/class/power_supply/ta1618-battery/current_now
```

`voltage_now` is reported in microvolts and `current_now` in microamps, as
defined by the power-supply ABI. The current value is signed: negative means
that the battery is discharging, while positive means that it is charging.

## Limits

The absolute accuracy of the voltage and current readings has not been checked
against an external instrument. These values are PMIC telemetry, not a direct
measurement at the battery terminals. FPLinux does not provide capacity, state
of charge, battery temperature, health, charge status or charge control on this
target.
