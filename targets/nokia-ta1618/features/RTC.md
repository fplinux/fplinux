# Real-time clock on Nokia 3210 4G (TA-1618)

This page applies only to Nokia 3210 4G (TA-1618). The SC2720 real-time clock is
exposed as the standard RTC device `/dev/rtc0`. Its time is read-only.

FPLinux can read the clock and use normal one-shot alarms through the standard
Linux RTC API. An armed alarm can wake the default RAM-boot system from
[s2idle](SUSPEND.md).

FPLinux cannot set the RTC and does not provide update interrupts. The RTC is
not used to initialize or synchronize the Linux system clock. Alarm power-on
from shutdown and deep suspend-to-RAM are not supported. The clock value may
reset while the phone battery is absent.
