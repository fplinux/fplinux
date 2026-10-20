// SPDX-License-Identifier: GPL-2.0-only
/* Real WLAN callbacks and codecs; registration and transport are host fakes. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <linux/unaligned.h>
#include <net/cfg80211.h>

#include "marlin2-transport.h"
#include "marlin2-wlan.h"

struct marlin2_transport {
	int unused;
};
struct marlin2_channel {
	const struct marlin2_channel_ops *ops;
	void *context;
	bool opened;
};
static struct {
	struct device dev;
	struct marlin2_transport transport;
	struct marlin2_channel channel;
	struct wiphy *wiphy;
	struct net_device *netdev;
	unsigned int writes, ie_count;
	u8 ie_payloads[3][6];
} bench;

struct wiphy *wiphy_new(const struct cfg80211_ops *ops, size_t bytes)
{
	struct wiphy *wiphy = calloc(1, sizeof(*wiphy));

	assert(wiphy && !bench.wiphy);
	wiphy->private_data = calloc(1, bytes);
	assert(wiphy->private_data);
	wiphy->ops = ops;
	bench.wiphy = wiphy;
	return wiphy;
}

int wiphy_register(struct wiphy *wiphy)
{
	assert(wiphy == bench.wiphy && wiphy->dev == &bench.dev);
	return 0;
}

void wiphy_unregister(struct wiphy *wiphy)
{
	assert(wiphy == bench.wiphy);
}

void wiphy_free(struct wiphy *wiphy)
{
	assert(wiphy == bench.wiphy);
	free(wiphy->private_data);
	free(wiphy);
	bench.wiphy = NULL;
}

struct net_device *alloc_etherdev(size_t bytes)
{
	struct net_device *netdev = calloc(1, sizeof(*netdev));

	assert(netdev && !bench.netdev);
	netdev->private_data = calloc(1, bytes);
	assert(netdev->private_data);
	bench.netdev = netdev;
	return netdev;
}

int register_netdev(struct net_device *netdev)
{
	assert(netdev == bench.netdev && netdev->dev == &bench.dev);
	return 0;
}

void unregister_netdev(struct net_device *netdev)
{
	assert(netdev == bench.netdev && !netdev->running);
}

void free_netdev(struct net_device *netdev)
{
	assert(netdev == bench.netdev);
	free(netdev->private_data);
	free(netdev);
	bench.netdev = NULL;
}

int register_inetaddr_notifier(struct notifier_block *notifier)
{
	assert(notifier->notifier_call);
	return 0;
}

int unregister_inetaddr_notifier(struct notifier_block *notifier)
{
	assert(notifier->notifier_call);
	return 0;
}

const struct ieee80211_reg_rule *freq_reg_info(struct wiphy *wiphy,
					       unsigned int khz)
{
	static const struct ieee80211_reg_rule rule = {
		.freq_range = { 2402000, 2472000, 20000 },
		.power_rule = { 0, 2000 },
	};

	assert(wiphy == bench.wiphy && khz >= 2412000 && khz <= 2462000);
	return &rule;
}

struct marlin2_channel *
marlin2_channel_register(struct marlin2_transport *transport,
			 enum marlin2_client client,
			 const struct marlin2_channel_ops *ops, void *context)
{
	assert(transport == &bench.transport && client == MARLIN2_CLIENT_WLAN);
	assert(!bench.channel.ops);
	bench.channel.ops = ops;
	bench.channel.context = context;
	return &bench.channel;
}

void marlin2_channel_unregister(struct marlin2_channel *channel)
{
	assert(channel == &bench.channel && !channel->opened);
	channel->ops = NULL;
}

int marlin2_channel_open(struct marlin2_channel *channel)
{
	assert(channel == &bench.channel && !channel->opened);
	channel->opened = true;
	return 0;
}

void marlin2_channel_close(struct marlin2_channel *channel)
{
	assert(channel == &bench.channel && channel->opened);
	channel->opened = false;
}

int marlin2_channel_write(struct marlin2_channel *channel, const u8 *packet,
			  size_t bytes)
{
	/* Reply framing is test-owned; only the request token is echoed. */
	u8 response[40] = {};
	size_t response_bytes = 12;
	size_t message_bytes;
	u8 id;

	assert(channel == &bench.channel && channel->opened);
	assert(bytes == 512 && packet[3] == 0x20);
	message_bytes = get_unaligned_le16(packet + 6);
	assert(message_bytes >= 12 && message_bytes <= bytes - 8);
	id = packet[5];
	bench.writes++;
	response[1] = id;
	memcpy(response + 4, packet + 8, 4);
	switch (id) {
	case 1:
		response_bytes = 40;
		response[28] =
			8; /* The 28-byte capability reply admits WLAN. */
		response[32] = 4;
		break;
	case 2:
	case 3:
	case 4:
	case 0x11:
		break;
	case 0x19:
		assert(message_bytes == 18 && bench.ie_count < 3);
		memcpy(bench.ie_payloads[bench.ie_count++], packet + 16, 6);
		break;
	default:
		assert(false);
	}
	response[2] = response_bytes;
	channel->ops->receive(channel->context, 0, response, response_bytes);
	return 0;
}

unsigned long wait_for_completion_timeout(struct completion *completion,
					  unsigned long timeout)
{
	assert(timeout == 3000);
	return completion->done ? 1 : 0;
}

static int get_mac(void *context, u8 address[6])
{
	const u8 mac[6] = { 2, 3, 4, 5, 6, 7 };

	assert(context == &bench);
	memcpy(address, mac, sizeof(mac));
	return 0;
}

static void check_update(const char *scenario)
{
	const struct marlin2_wlan_radio_ops radio_ops = { .get_mac = get_mac };
	const u8 head[40] = {
		[0] = 0x80, [36] = 0, [37] = 2, [38] = 'a', [39] = 'p',
	};
	const u8 tail[] = { 3, 1, 1 };
	const u8 beacon_ie[] = { 0xdd, 1, 0x11 };
	const u8 probe_ie[] = { 0xdd, 1, 0x22 };
	const u8 assoc_ie[] = { 0xdd, 1, 0x33 };
	const u8 expected_ies[3][6] = {
		{ 0, 3, 0, 0xdd, 1, 0x11 },
		{ 2, 3, 0, 0xdd, 1, 0x22 },
		{ 4, 3, 0, 0xdd, 1, 0x33 },
	};
	struct cfg80211_ap_settings settings = {
		.beacon = { .head = head, .head_len = sizeof(head) },
		.ssid = (const u8 *)"ap",
		.ssid_len = 2,
	};
	struct cfg80211_ap_update update = {};
	struct marlin2_wlan *wlan;
	const struct cfg80211_ops *ops;
	bool extra_ies = !strcmp(scenario, "extra-ies");
	bool started = strcmp(scenario, "not-started") != 0;
	unsigned int writes;
	int ret;

	wlan = marlin2_wlan_register(&bench.dev, &radio_ops, &bench,
				     &bench.transport);
	assert(!IS_ERR(wlan));
	ops = bench.wiphy->ops;
	assert(!ops->change_virtual_intf(bench.wiphy, bench.netdev,
					 NL80211_IFTYPE_AP, NULL));
	assert(!bench.netdev->netdev_ops->ndo_open(bench.netdev));
	bench.netdev->running = true;
	settings.chandef.chan = &bench.wiphy->bands[0]->channels[0];
	settings.chandef.width = NL80211_CHAN_WIDTH_20_NOHT;
	if (started)
		assert(!ops->start_ap(bench.wiphy, bench.netdev, &settings));

	if (!strcmp(scenario, "head") || !strcmp(scenario, "mixed") ||
	    !started) {
		update.beacon.head = head;
		update.beacon.head_len = sizeof(head);
	} else if (!strcmp(scenario, "tail")) {
		update.beacon.tail = tail;
		update.beacon.tail_len = sizeof(tail);
	} else if (!strcmp(scenario, "empty-tail")) {
		update.beacon.tail = tail;
	} else {
		assert(extra_ies);
	}
	if (extra_ies || !strcmp(scenario, "mixed")) {
		update.beacon.beacon_ies = beacon_ie;
		update.beacon.beacon_ies_len = sizeof(beacon_ie);
		update.beacon.proberesp_ies = probe_ie;
		update.beacon.proberesp_ies_len = sizeof(probe_ie);
		update.beacon.assocresp_ies = assoc_ie;
		update.beacon.assocresp_ies_len = sizeof(assoc_ie);
	}
	writes = bench.writes;
	ret = ops->change_beacon(bench.wiphy, bench.netdev, &update);
	if (extra_ies) {
		assert(!ret && bench.writes == writes + 3 &&
		       bench.ie_count == 3);
		assert(!memcmp(bench.ie_payloads, expected_ies,
			       sizeof(expected_ies)));
	} else {
		assert(ret == (started ? -EOPNOTSUPP : -ENETDOWN));
		assert(bench.writes == writes && !bench.ie_count);
	}
	if (started)
		assert(!ops->stop_ap(bench.wiphy, bench.netdev, 0));
	assert(!bench.netdev->netdev_ops->ndo_stop(bench.netdev));
	bench.netdev->running = false;
	marlin2_wlan_unregister(wlan);
	assert(!bench.wiphy && !bench.netdev && !bench.channel.ops);
}

int main(int argc, char **argv)
{
	assert(argc == 2);
	check_update(argv[1]);
	puts("Marlin2 beacon callback host case passed");
	return 0;
}
