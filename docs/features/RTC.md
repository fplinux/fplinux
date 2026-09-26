# Real-time clock

On targets with RTC support, the SC2720 real-time clock is exposed as the
standard RTC device `/dev/rtc0`. It supports reading and setting UTC time from
1980-01-01 through 2099-12-31. The selected target's documentation states its
supported time, alarm and wake behavior; a standalone archive describes that
status in `README.txt`.

Invalid clock contents cause time reads to fail, but the device remains
available for correction through the standard Linux RTC API. FPLinux does not
substitute a default date. Time may be lost while the phone battery is absent;
check the clock before relying on it.

When `./fplinux run` or a standalone archive's runner opens a new SSH session,
it sets the phone's system clock to the host's current UTC time. It writes that
time to the RTC only when the RTC time cannot be read, then reads the RTC again
to confirm it. A readable RTC keeps its time; to replace it, run
`hwclock -u -w -f /dev/rtc0` on the phone. The runner prints the result, or a
warning when the clock could not be set, and the session remains usable.
Reconnecting to a running session does not set the clock. The phone has no time
zone, and the RTC holds UTC.

Normal one-shot alarms retain their absolute deadline when the time changes.
Moving the clock past an armed deadline delivers the event once. On targets
and profiles where RTC wake is supported, an armed alarm can wake the system from
[s2idle](SUSPEND.md), including after a time correction.

Update interrupts, alarm power-on from shutdown and deep suspend-to-RAM are not
supported. The Linux system clock is not initialized from the RTC.
