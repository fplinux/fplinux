# Power-off on Nokia 3210 4G (TA-1618)

This page applies only to Nokia 3210 4G (TA-1618). Battery-only Linux power-off
is supported while external charger input is absent.

## Safe shutdown

1. Flush and unmount any mounted microSD filesystem as described in
   [microSD](MICROSD.md).
2. Exit the host shell and disconnect USB.
3. Make sure charger power is absent.
4. Hold the red handset key continuously for five seconds.

A short press remains an ordinary input event. Releasing the key before five
seconds cancels the request. If external charger input is detected, shutdown is
refused.

A successful shutdown discards the volatile RAM session. Boot the phone
normally to return to the vendor firmware. If it remains powered after shutdown
starts, remove and reinsert the battery before booting.

Linux reboot is not supported. [Suspend](SUSPEND.md) preserves the RAM session
and is not a substitute for this shutdown sequence.
