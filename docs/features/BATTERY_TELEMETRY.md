# Battery telemetry

FPLinux exposes battery voltage, signed current and a relative charge counter
through the standard Linux power-supply class. The selected
[target's documentation](../../targets/README.md) states its measurement
limits. A standalone archive carries that status in `README.txt`.

## Interface

The battery is `/sys/class/power_supply/sc2720-battery` and has the `type`
value `Battery`. Read its `voltage_now`, `current_now` and `charge_counter`
attributes.

`voltage_now` is reported in microvolts and `current_now` in microamps, as
defined by the power-supply ABI. The current value is signed: negative means
that the battery is discharging, while positive means that it is charging.

`charge_counter` is a signed relative accumulator reported in microamp-hours.
It can be negative and has no empty or full value. Compare readings over a
known time interval to measure charge entering or leaving the battery; do not
interpret the absolute value as remaining charge or battery percentage.

## Measure an application

The optional `fplinux-charge.apk` package measures one command without storing
a sampling history. It discovers the battery charge counter automatically.
Install it into the active system root; installation is temporary in the
`default` RAM profile and persistent in `microsd-uboot`. Its installed package
name is `fplinux-charge`. Follow
[Installing and removing optional APK packages](../guides/APK_PACKAGES.md) for
either a source checkout or a standalone archive.

Run an installed command through the helper:

```sh
fplinux-charge -- sleep 60
fplinux-charge -- quake --input phone
fplinux-charge -- micropythonos
```

Use `fplinux-charge -h` or `fplinux-charge --help` to list options without
reading a counter or starting a command. `--counter PATH` or `--counter=PATH`
selects a specific counter file and may appear only once. The `--` separator
is required before the command; everything after it belongs to that command.

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
or charge control through this interface. Readings made without a battery do
not qualify battery measurements or charging.
