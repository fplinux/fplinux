# Hardware platforms

A platform contains SoC support shared by more than one phone: processor and
interrupt integration, reusable controller drivers, SoC DTS nodes, and the fixed
RAM-loader flow. Phone models, panel and keypad choices, board memory maps and
loader assets belong in [targets](../targets/README.md).

| Platform                       | SoC                   | Reusable capabilities                                                                                     | Targets                                                                                                                                                                               |
| ------------------------------ | --------------------- | --------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| [`ums9117`](ums9117/README.md) | Unisoc UMS9117 / T117 | CPU, GIC, timers, read-only MPLL/CPU clock reporting, USB gadget, ADI, framebuffer core and matrix keypad | [`inoi-240-modern-4g`](../targets/inoi-240-modern-4g/README.md), [`inoi-244-modern-4g`](../targets/inoi-244-modern-4g/README.md), [`nokia-ta1618`](../targets/nokia-ta1618/README.md) |

The [platform template](../docs/porting/PLATFORM.md) defines the documented
boundary between reusable SoC support and target-owned board data.
