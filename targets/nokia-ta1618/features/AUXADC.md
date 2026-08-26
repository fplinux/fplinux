# Auxiliary ADC on Nokia 3210 4G (TA-1618)

This page applies only to Nokia 3210 4G (TA-1618). FPLinux exposes five SC2720
auxiliary ADC channels through the standard Linux IIO interface.

## Interface

Find the `iio:deviceN` whose `name` is `ta1618-sc2720-auxadc`. It provides raw
attributes for channels 0, 1, 2, 4 and 14:

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
