# CPU clock reporting

FPLinux observes the UMS9117 clock state inherited from boot firmware. It does
not select a parent clock, change a divider, program the MPLL, install a
governor or otherwise control CPU frequency.

The built-in observer exposes the current Cortex-A7 and MPLL rates to Linux
only when it can decode a stable, supported register snapshot. It returns no
invented rate for an unstable or unsupported clock state.

`fplinux-cpuclock` is a separate phone-side measurement helper. It runs a
dependent integer-addition chain, times it with the monotonic clock and prints
each round plus the best result. The loop overhead is deliberately excluded, so
the reported result is a lower bound rather than an optimistic frequency claim.

Run it through the active session:

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
target document states whether the observer and helper have been exercised on
that exact phone.
