// SPDX-License-Identifier: GPL-2.0-only
#include "audio-device.h"

#include <stdio.h>
#include <stdlib.h>

int main(void)
{
	struct fplinux_audio_device device = { 0 };
	int result = fplinux_audio_device_init(&device);

	printf("init=%d rate=%u speaker=%u headphones=%u\n", result,
	       device.rate, device.speaker_id != 0 || device.fixed_speaker,
	       device.headphone_id != 0);
	if (result < 0)
		return 0;
	if (getenv("JACK_TEST_ALARM_FALLBACK")) {
		fplinux_audio_device_route(&device,
					   FPLINUX_AUDIO_ROUTE_HEADPHONES);
		result = fplinux_audio_device_alarm(&device, true);
		printf("alarm result=%d desired=%u applied=%u\n", result,
		       device.route, device.applied_route);
		fplinux_audio_device_alarm(&device, false);
		printf("restore desired=%u applied=%u\n", device.route,
		       device.applied_route);
		return 0;
	}
	while ((result = fplinux_audio_device_events(&device)) >= 0) {
		if (result > 0) {
			puts(device.jack_present ? "inserted" : "removed");
			result = fplinux_audio_device_route(&device,
							    device.route);
			if (result < 0)
				break;
		}
	}
	printf("events=%d\n", result);
	return 0;
}
