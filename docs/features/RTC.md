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

Normal one-shot alarms retain their absolute deadline when the time changes.
Moving the clock past an armed deadline delivers the event once. On targets
and profiles where RTC wake is supported, an armed alarm can wake the system from
[s2idle](SUSPEND.md), including after a time correction.

Update interrupts, alarm power-on from shutdown and deep suspend-to-RAM are not
supported. The RTC is not used to initialize or synchronize the Linux system
clock, and the system clock is not automatically copied to the RTC.
