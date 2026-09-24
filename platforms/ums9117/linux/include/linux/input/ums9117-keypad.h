/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UMS9117_KEYPAD_H
#define UMS9117_KEYPAD_H

struct device_node;
struct notifier_block;

enum ums9117_keypad_power_state {
	UMS9117_KEYPAD_POWER_RELEASE,
	UMS9117_KEYPAD_POWER_PRESS,
};

struct ums9117_keypad_power_event {
	struct device_node *keypad_node;
};

int ums9117_keypad_register_power_notifier(struct notifier_block *notifier);
int ums9117_keypad_unregister_power_notifier(struct notifier_block *notifier);

#endif
