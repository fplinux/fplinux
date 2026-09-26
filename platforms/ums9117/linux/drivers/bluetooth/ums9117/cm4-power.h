/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UMS9117_CM4_POWER_H
#define UMS9117_CM4_POWER_H

struct device;

enum ums9117_cm4_user {
	UMS9117_CM4_BLUETOOTH,
	UMS9117_CM4_FM,
};

/* Sleepable owner transitions; never call with the transport lock held. */
int ums9117_cm4_get(struct device *dev, enum ums9117_cm4_user user);
void ums9117_cm4_put(struct device *dev, enum ums9117_cm4_user user);

#endif
