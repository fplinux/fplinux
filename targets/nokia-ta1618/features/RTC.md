# Real-time clock on Nokia 3210 4G (TA-1618)

This page applies only to Nokia 3210 4G (TA-1618). The SC2720 real-time clock is
exposed as the standard read-only RTC device `/dev/rtc0`.

FPLinux can read the clock. It cannot set it and provides no RTC alarm or update
interrupt. The RTC is not used to initialize or synchronize the Linux system
clock. Its value may reset while the phone battery is absent.
