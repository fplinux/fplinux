# SoC temperature

FPLinux exposes the calibrated THM1 reading through the standard Linux thermal
class. The selected [target's documentation](../../targets/README.md) states
its physical accuracy limits. A standalone archive carries that status in
`README.txt`.

## Interface

Find the zone whose `type` is `ums9117-thm1` under `/sys/class/thermal/`. The
adjacent `temp` file reports temperature in millidegrees Celsius. The numeric
`thermal_zoneN` index is assigned at boot and is not part of the interface.

## Limits

The reading has not been checked against an external temperature reference.
No thermal trips, cooling device or automatic clock policy is provided; the
interface reports temperature only.
