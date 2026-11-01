/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "fplinux-ui-softkey-layout.h"

#include <stdbool.h>

void fplinux_softkey_layout(int bar_width, int margin, int gap,
			    const int widths[3],
			    struct fplinux_softkey_box boxes[3])
{
	int allotted[3] = { 0 };
	bool placed[3] = { 0 };
	int count = 0;
	int available;
	int ideal;
	int lowest;
	int highest;
	int centre;

	for (int i = 0; i < 3; ++i)
		count += widths[i] > 0;
	available =
		bar_width - 2 * margin - (count > 1 ? (count - 1) * gap : 0);
	/* The narrowest legend keeps its width when it fits an equal share. */
	for (int remaining = count; remaining > 0; --remaining) {
		int narrowest = -1;
		int share = available / remaining;

		for (int i = 0; i < 3; ++i)
			if (widths[i] > 0 && !placed[i] &&
			    (narrowest < 0 || widths[i] < widths[narrowest]))
				narrowest = i;
		if (narrowest < 0)
			break;
		allotted[narrowest] =
			widths[narrowest] < share ? widths[narrowest] : share;
		available -= allotted[narrowest];
		placed[narrowest] = true;
	}
	ideal = (bar_width - allotted[1]) / 2;
	lowest = margin;
	if (allotted[0])
		lowest += allotted[0] + gap;
	highest = bar_width - margin - allotted[1];
	if (allotted[2])
		highest -= allotted[2] + gap;
	centre = ideal;
	if (allotted[1])
		centre = ideal < lowest	 ? lowest :
			 ideal > highest ? highest :
					   ideal;
	boxes[0] = (struct fplinux_softkey_box){ margin, allotted[0] };
	boxes[1] = (struct fplinux_softkey_box){ centre, allotted[1] };
	boxes[2] =
		(struct fplinux_softkey_box){ bar_width - margin - allotted[2],
					      allotted[2] };
}
