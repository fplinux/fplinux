# CPU clock and frequency selection

FPLinux starts the UMS9117 Cortex-A7 at the 1 GHz clock inherited from boot
firmware. The default `performance` governor keeps that frequency. Manual
selection through standard Linux cpufreq supports only 768 MHz and 1 GHz. It
switches the CPU clock source while retaining the inherited voltage, MPLL and
divider settings. Automatic frequency scaling and undervolting are not provided.

To select a frequency from a shell on the phone, change to the `userspace`
governor and write the desired rate in kHz. Restore the default governor when
finished:

```sh
cd /sys/devices/system/cpu/cpu0/cpufreq
echo userspace > scaling_governor
echo 768000 > scaling_setspeed
cat scaling_cur_freq
echo 1000000 > scaling_setspeed
echo performance > scaling_governor
```

The clock observer reports the current Cortex-A7 and MPLL rates to Linux when
it can decode a stable, supported register snapshot. It returns no invented
rate for an unstable or unsupported clock state. The selected target document
states whether frequency switching has been tested on that phone.

`fplinux-cpuclock` is a separate phone-side measurement helper. It runs a
dependent integer-addition chain, times it with the monotonic clock and prints
each round plus the best result. The loop overhead is deliberately excluded, so
the reported result is a lower bound rather than an optimistic frequency claim.

Run it through the active session, adding the session's `--profile` to the
source-checkout command as described in [SSH sessions](SSH.md):

```sh
# Source checkout
./fplinux console <target> --exec fplinux-cpuclock

# Standalone archive
./runner/run.py --reconnect --exec fplinux-cpuclock
```

Optional arguments select the number of loop iterations and measurement rounds:

```sh
fplinux-cpuclock [iterations] [rounds]
```

The defaults are 2000000 iterations and 5 rounds. Each supplied value must be a
complete positive decimal integer from 1 through 4294967295. Extra positional
arguments are rejected. Use `fplinux-cpuclock -h` or
`fplinux-cpuclock --help` to show the syntax without running the workload.

This is a diagnostic workload, not a performance guarantee, thermal test or
clock-control interface. Its result describes the machine on which that
workload ran; a host run is not evidence of the phone's clock. The selected
target document states the support boundary for that exact phone.
