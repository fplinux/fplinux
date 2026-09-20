# Auxiliary ADC

FPLinux exposes five SC2720 auxiliary ADC channels through the standard Linux
IIO interface. The selected [target's documentation](../../targets/README.md)
states its hardware limits. A standalone archive carries that status in
`README.txt`.

## Interface

Find `sc2720-auxadc` under `/sys/bus/iio/devices/` by reading each device's
`name` attribute; the numeric `iio:deviceN` index is assigned at boot. The
device provides raw attributes for channels 0, 1, 2, 4 and 14:

```text
in_voltage0_raw
in_voltage1_raw
in_voltage2_raw
in_voltage4_raw
in_voltage14_raw
```

## Limits

Only raw ADC codes are supported. No scale, processed value, voltage,
temperature or other physical-unit conversion is provided. The external signal
connected to each channel has not been identified, so a raw reading must not be
presented as a named sensor value.
