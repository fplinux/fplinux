# Power-off

The shared SC2720 power-off interface requests an orderly Linux shutdown while
external charger input is absent. Use the sequence below only on a target whose
documentation lists battery-only power-off as supported. Check the selected phone's
documentation, or `README.txt` in a standalone archive, for its support status
and physical power key.

## Safe shutdown

1. In the default RAM profile, flush and unmount removable filesystems as
   described in [microSD](MICROSD.md). With a microSD system root, stop
   applications but do not try to unmount `/`; orderly shutdown handles it.
2. Exit the host shell and disconnect USB.
3. Make sure charger power is absent.
4. Hold the phone's power key continuously for five seconds.

A short press remains an ordinary input event. Releasing the key before five
seconds cancels the request. If external charger input is detected, shutdown is
refused.

A successful shutdown discards the volatile RAM session. With a microSD system
root, OpenRC stops services, flushes writes and remounts the root read-only
before power-off. If userspace shutdown cannot start, the phone remains on
instead of forcing power off with a writable root. Follow the
[system-card safety rules](../guides/MICROSD_ROOT.md).

Boot the phone normally to return to the vendor firmware.

Linux reboot is not supported. [Suspend](SUSPEND.md) preserves the RAM session
and is not a substitute for this shutdown sequence.
