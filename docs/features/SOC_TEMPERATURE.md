# SoC temperature

FPLinux exposes the calibrated THM1 reading through the standard Linux thermal
class. The selected [target's documentation](../../targets/README.md) states
its thermal-zone name and physical accuracy limits. A standalone archive carries
that status in `README.txt`.

## Interface

Find the target's zone under `/sys/class/thermal/` by reading its `type` file.
The adjacent `temp` file reports temperature in millidegrees Celsius. The
numeric `thermal_zoneN` index is assigned at boot and is not part of the target
contract.

## Limits

The reading has not been checked against an external temperature reference.
No thermal trips, cooling device or automatic clock policy is provided; the
interface reports temperature only.
