# Battery telemetry on Nokia 3210 4G (TA-1618)

This page applies only to Nokia 3210 4G (TA-1618). FPLinux exposes live battery
voltage, signed current and a relative charge counter through the standard Linux
power-supply class.

## Interface

```sh
cat /sys/class/power_supply/ta1618-battery/voltage_now
cat /sys/class/power_supply/ta1618-battery/current_now
cat /sys/class/power_supply/ta1618-battery/charge_counter
```

`voltage_now` is reported in microvolts and `current_now` in microamps, as
defined by the power-supply ABI. The current value is signed: negative means
that the battery is discharging, while positive means that it is charging.

`charge_counter` is a signed relative accumulator reported in microamp-hours.
It can be negative and has no empty or full value. Compare readings over a
known time interval to measure charge entering or leaving the battery; do not
interpret the absolute value as remaining charge or battery percentage.

## Measure an application

The optional `fplinux-charge.apk` package in the Nokia build bundle measures
one command without storing a sampling history. Install it into the current RAM
session:

```sh
# Source checkout: use the checkout-relative path printed after `output:`
bundle=.cache/out/nokia-ta1618/bundles/<generation>
./fplinux console nokia-ta1618 --upload \
  "$bundle/apks/fplinux-charge.apk" /tmp/fplinux-charge.apk
./fplinux console nokia-ta1618 --exec \
  'apk add --no-network --allow-untrusted --force-non-repository /tmp/fplinux-charge.apk'

# Standalone archive
./runner/run.py --reconnect --upload \
  ./apks/fplinux-charge.apk /tmp/fplinux-charge.apk
./runner/run.py --reconnect --exec \
  'apk add --no-network --allow-untrusted --force-non-repository /tmp/fplinux-charge.apk'
```

Run an installed command through the helper:

```sh
fplinux-charge -- sleep 60
fplinux-charge -- quake --input phone
fplinux-charge -- micropythonos
```

When the command exits, the helper reports elapsed monotonic time, charge delta
and average battery current. Positive values mean net charge entered the
battery; negative values mean net discharge. The helper preserves the command's
exit or signal result when the final measurement succeeds. If the final clock
or charge-counter measurement fails, it prints no metrics: it returns `125` for
a successful command and otherwise preserves the command's exit or signal
result.

## Limits

The absolute accuracy of the voltage, current and charge-counter readings has
not been checked against an external instrument. These values are PMIC
telemetry, not a direct measurement at the battery terminals. FPLinux does not
provide capacity, state of charge, battery temperature, health, charge status
or charge control on this target.
