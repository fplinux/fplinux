# Charger status

FPLinux reports external charger input presence and USB source classification
through the standard Linux power-supply class. The selected
[target's documentation](../../targets/README.md) states its hardware limits.
A standalone archive carries that status in `README.txt`.

## Interface

Read `/sys/class/power_supply/sc2720-charger/online`.

`1` means external charger input is detected; `0` means it is not detected.

Read `/sys/class/power_supply/sc2720-charger/usb_type` for the detected source
type. The active value appears in brackets: `SDP` is a standard downstream
port, `CDP` is a charging downstream port, and `DCP` is a dedicated charging
port. `Unknown` means the input is absent, detection is incomplete, or no
single source type is reported. Reading this attribute does not start detection.
The classification does not establish the source's available power or a
permitted charging current. The supported source types have not all been
qualified on physical hardware.

This is connection status, not proof that the battery is charging. The
interface does not enable, disable or configure charging and does not report a
charge rate or battery level. External charger input also prevents the
[power-off](POWER_OFF.md) path on targets where it is supported.
