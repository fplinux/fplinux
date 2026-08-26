# SoC temperature on Nokia 3210 4G (TA-1618)

This page applies only to Nokia 3210 4G (TA-1618). FPLinux exposes the calibrated
THM1 reading through the standard Linux thermal class.

## Interface

The thermal zone whose `type` file contains `ta1618-soc` reports its current
temperature through the adjacent `temp` file. Linux thermal-zone temperatures
use millidegrees Celsius. The numeric `thermal_zoneN` index is assigned at boot
and is not part of the target contract.

## Limits

The reading has not been checked against an external temperature reference.
No thermal trips, cooling device or automatic clock policy is provided; the
interface reports temperature only.
