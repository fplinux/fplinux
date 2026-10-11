// SPDX-License-Identifier: GPL-2.0-only

#include <linux/completion.h>
#include <linux/bitops.h>
#include <linux/build_bug.h>
#include <linux/err.h>
#include <linux/etherdevice.h>
#include <linux/ieee80211.h>
#include <linux/inetdevice.h>
#include <linux/jiffies.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>
#include <net/cfg80211.h>
#include <net/regulatory.h>

#include "marlin2-transport.h"
#include "marlin2-wlan.h"
#include "marlin2-wlan-wire.h"

#define MARLIN2_WLAN_COMMAND_TIMEOUT_MS 3000U
#define MARLIN2_WLAN_SCAN_TIMEOUT_MS 7000U
#define MARLIN2_WLAN_TX_QUEUE_MAX 64U
#define MARLIN2_WLAN_BEACON_MAX_BYTES 768U

static_assert(NL80211_SCAN_FLAG_COLOCATED_6GHZ ==
	      MARLIN2_WLAN_SCAN_COLOCATED_6GHZ_FLAG);

struct marlin2_wlan {
	struct device *dev;
	struct wiphy *wiphy;
	struct net_device *netdev;
	struct wireless_dev wdev;
	const struct marlin2_wlan_radio_ops *radio_ops;
	void *radio_context;
	struct marlin2_channel *channel;
	struct ieee80211_supported_band band;
	struct ieee80211_channel channels[11];
	/* One command may wait while the independent IRQ thread receives it. */
	struct mutex command_lock;
	/* Protects command matching, credits and scan/connection state. */
	spinlock_t state_lock;
	struct completion response;
	struct sk_buff_head tx_queue;
	struct work_struct tx_work;
	struct delayed_work scan_timeout;
	struct notifier_block ipv4_notifier;
	struct cfg80211_scan_request *scan;
	u8 *tx_packet;
	u32 token;
	u32 pending_token;
	u32 credits_shared;
	u32 credits_station;
	u8 pending_command;
	u8 mode;
	u8 response_payload[64];
	size_t response_bytes;
	int response_error;
	int io_error;
	bool pending;
	bool running;
	bool tx_enabled;
	bool port_open;
	bool connecting;
	bool connected;
	bool ap_started;
	u8 alpha2[2];
	bool ipv4_notifier_registered;
};

static struct ieee80211_rate marlin2_wlan_rates[] = {
	{ .bitrate = 10, .hw_value = 0 },   { .bitrate = 20, .hw_value = 1 },
	{ .bitrate = 55, .hw_value = 2 },   { .bitrate = 110, .hw_value = 3 },
	{ .bitrate = 60, .hw_value = 4 },   { .bitrate = 90, .hw_value = 5 },
	{ .bitrate = 120, .hw_value = 6 },  { .bitrate = 180, .hw_value = 7 },
	{ .bitrate = 240, .hw_value = 8 },  { .bitrate = 360, .hw_value = 9 },
	{ .bitrate = 480, .hw_value = 10 }, { .bitrate = 540, .hw_value = 11 },
};

static const u32 marlin2_wlan_ciphers[] = { WLAN_CIPHER_SUITE_CCMP };

static struct marlin2_wlan *wlan_from_wiphy(struct wiphy *wiphy)
{
	return *(struct marlin2_wlan **)wiphy_priv(wiphy);
}

static struct marlin2_wlan *wlan_from_netdev(struct net_device *netdev)
{
	return *(struct marlin2_wlan **)netdev_priv(netdev);
}

static void finish_scan(struct marlin2_wlan *wlan, bool aborted)
{
	struct cfg80211_scan_info info = { .aborted = aborted };
	struct cfg80211_scan_request *request;
	unsigned long flags;

	spin_lock_irqsave(&wlan->state_lock, flags);
	request = wlan->scan;
	wlan->scan = NULL;
	spin_unlock_irqrestore(&wlan->state_lock, flags);
	if (request)
		cfg80211_scan_done(request, &info);
}

static void scan_timeout_work(struct work_struct *work)
{
	struct marlin2_wlan *wlan = container_of(
		to_delayed_work(work), struct marlin2_wlan, scan_timeout);

	finish_scan(wlan, true);
}

static void receive_response(struct marlin2_wlan *wlan, const u8 *message,
			     size_t bytes)
{
	unsigned long flags;

	spin_lock_irqsave(&wlan->state_lock, flags);
	if (!wlan->pending || message[1] != wlan->pending_command ||
	    get_unaligned_le32(message + 4) != wlan->pending_token)
		goto out_unlock;
	wlan->response_error = (s8)message[8] ? -EREMOTEIO : 0;
	wlan->response_bytes = bytes - MARLIN2_WLAN_COMMAND_BYTES;
	if (wlan->response_bytes <= sizeof(wlan->response_payload))
		memcpy(wlan->response_payload,
		       message + MARLIN2_WLAN_COMMAND_BYTES,
		       wlan->response_bytes);
	else
		wlan->response_error = -EMSGSIZE;
	wlan->pending = false;
	complete(&wlan->response);
out_unlock:
	spin_unlock_irqrestore(&wlan->state_lock, flags);
}

static void receive_credits(struct marlin2_wlan *wlan, const u8 *credits)
{
	unsigned long flags;

	spin_lock_irqsave(&wlan->state_lock, flags);
	wlan->credits_shared += credits[0];
	wlan->credits_station += credits[1];
	spin_unlock_irqrestore(&wlan->state_lock, flags);
	if (READ_ONCE(wlan->running) && READ_ONCE(wlan->tx_enabled))
		schedule_work(&wlan->tx_work);
}

static bool take_ie(const u8 **cursor, size_t *remaining, const u8 **ie,
		    size_t *ie_bytes)
{
	if (*remaining < 2)
		return false;
	*ie_bytes = get_unaligned_le16(*cursor);
	*cursor += 2;
	*remaining -= 2;
	if (*ie_bytes > *remaining)
		return false;
	*ie = *cursor;
	*cursor += *ie_bytes;
	*remaining -= *ie_bytes;
	return true;
}

static void receive_connect(struct marlin2_wlan *wlan, const u8 *payload,
			    size_t bytes)
{
	const u8 *request_ie = NULL;
	const u8 *response_ie = NULL;
	const u8 *beacon_ie = NULL;
	size_t request_bytes = 0;
	size_t response_bytes = 0;
	size_t beacon_bytes = 0;
	const u8 *cursor;
	size_t remaining;
	unsigned long flags;
	bool connecting;
	bool success;

	if (!bytes)
		return;
	success = payload[0] == 0;
	if (success) {
		if (bytes < 9)
			return;
		cursor = payload + 9;
		remaining = bytes - 9;
		if (!take_ie(&cursor, &remaining, &request_ie,
			     &request_bytes) ||
		    !take_ie(&cursor, &remaining, &response_ie,
			     &response_bytes) ||
		    !take_ie(&cursor, &remaining, &beacon_ie, &beacon_bytes))
			return;
	}
	spin_lock_irqsave(&wlan->state_lock, flags);
	connecting = wlan->connecting;
	if (connecting) {
		wlan->connecting = false;
		wlan->connected = success;
	}
	spin_unlock_irqrestore(&wlan->state_lock, flags);
	if (!connecting)
		return;
	cfg80211_connect_result(
		wlan->netdev, success ? payload + 1 : NULL, request_ie,
		request_bytes, response_ie, response_bytes,
		success ? WLAN_STATUS_SUCCESS : WLAN_STATUS_UNSPECIFIED_FAILURE,
		GFP_KERNEL);
	if (success)
		netif_carrier_on(wlan->netdev);
}

static void receive_disconnect(struct marlin2_wlan *wlan, u16 reason)
{
	unsigned long flags;
	bool connecting;
	bool connected;

	spin_lock_irqsave(&wlan->state_lock, flags);
	connecting = wlan->connecting;
	connected = wlan->connected;
	wlan->connecting = false;
	wlan->connected = false;
	spin_unlock_irqrestore(&wlan->state_lock, flags);
	netif_carrier_off(wlan->netdev);
	if (connecting)
		cfg80211_connect_result(wlan->netdev, NULL, NULL, 0, NULL, 0,
					WLAN_STATUS_UNSPECIFIED_FAILURE,
					GFP_KERNEL);
	else if (connected)
		cfg80211_disconnected(wlan->netdev, reason, NULL, 0, false,
				      GFP_KERNEL);
}

static void receive_management(struct marlin2_wlan *wlan, const u8 *metadata,
			       const u8 *frame, size_t available_bytes)
{
	struct ieee80211_channel *channel;
	struct cfg80211_bss *bss;
	u16 frame_bytes;
	int frequency;

	if (metadata[0] != 4 || metadata[1] < 1 || metadata[1] > 11)
		return;
	frame_bytes = get_unaligned_le16(metadata + 10);
	if (frame_bytes > available_bytes || frame_bytes < 36)
		return;
	frequency =
		ieee80211_channel_to_frequency(metadata[1], NL80211_BAND_2GHZ);
	channel = ieee80211_get_channel(wlan->wiphy, frequency);
	if (!channel)
		return;
	bss = cfg80211_inform_bss_frame(wlan->wiphy, channel,
					(struct ieee80211_mgmt *)frame,
					frame_bytes, (s8)metadata[2] * 100,
					GFP_KERNEL);
	if (bss)
		cfg80211_put_bss(wlan->wiphy, bss);
}

static void receive_station(struct marlin2_wlan *wlan, const u8 *payload,
			    size_t bytes)
{
	struct marlin2_wlan_station_event event;
	struct station_info info = {};

	if (!READ_ONCE(wlan->ap_started) ||
	    marlin2_wlan_station_decode(payload, bytes, &event))
		return;
	if (!is_valid_ether_addr(event.mac))
		return;
	if (!event.connected) {
		cfg80211_del_sta(&wlan->wdev, event.mac, GFP_KERNEL);
		return;
	}
	info.assoc_req_ies = event.ies;
	info.assoc_req_ies_len = event.ie_bytes;
	cfg80211_new_sta(&wlan->wdev, event.mac, &info, GFP_KERNEL);
}

static int receive_message(struct marlin2_wlan *wlan, const u8 *data,
			   size_t bytes)
{
	const u8 *payload;
	size_t payload_bytes;
	u8 type;

	if (!bytes)
		return -EBADMSG;
	type = data[0] & 7;
	if (type == 2) {
		struct marlin2_wlan_data_message received;
		struct sk_buff *skb;

		if (marlin2_wlan_data_decode(data, bytes, &received))
			return -EBADMSG;
		if (received.has_credits)
			receive_credits(wlan, received.credits);
		if (received.frame_type == 0x40 &&
		    received.mode == wlan->mode) {
			receive_management(wlan, received.management_metadata,
					   received.frame,
					   received.frame_bytes);
			return 0;
		}
		if (received.frame_type || received.frame_bytes < ETH_HLEN ||
		    received.mode != wlan->mode)
			return 0;
		payload = received.frame;
		payload_bytes = received.frame_bytes;
		skb = netdev_alloc_skb_ip_align(wlan->netdev, payload_bytes);
		if (!skb) {
			wlan->netdev->stats.rx_dropped++;
			return 0;
		}
		skb_put_data(skb, payload, payload_bytes);
		skb->protocol = eth_type_trans(skb, wlan->netdev);
		wlan->netdev->stats.rx_packets++;
		wlan->netdev->stats.rx_bytes += payload_bytes;
		netif_rx(skb);
		return 0;
	}
	if (bytes < MARLIN2_WLAN_COMMAND_BYTES ||
	    get_unaligned_le16(data + 2) != bytes)
		return -EBADMSG;
	if (!type) {
		receive_response(wlan, data, bytes);
		return 0;
	}
	if (!marlin2_wlan_event_for_mode(data[0], data[1], wlan->mode))
		return 0;
	payload = data + MARLIN2_WLAN_COMMAND_BYTES;
	payload_bytes = bytes - MARLIN2_WLAN_COMMAND_BYTES;
	switch (data[1]) {
	case 0x80:
		receive_connect(wlan, payload, payload_bytes);
		break;
	case 0x81:
		if (payload_bytes >= 2)
			receive_disconnect(wlan, get_unaligned_le16(payload));
		break;
	case 0x82:
		if (payload_bytes) {
			cancel_delayed_work(&wlan->scan_timeout);
			finish_scan(wlan, payload[0] != 1);
		}
		break;
	case 0x83:
		if (payload_bytes >= 12)
			receive_management(wlan, payload, payload + 12,
					   payload_bytes - 12);
		break;
	case 0xa0:
		receive_station(wlan, payload, payload_bytes);
		break;
	case 0xb3:
		if (payload_bytes >= 3)
			receive_credits(wlan, payload);
		break;
	}
	return 0;
}

static void fail_io(struct marlin2_wlan *wlan, int error)
{
	unsigned long flags;

	spin_lock_irqsave(&wlan->state_lock, flags);
	if (!wlan->io_error)
		wlan->io_error = error;
	if (wlan->pending) {
		wlan->response_error = error;
		wlan->pending = false;
		complete(&wlan->response);
	}
	spin_unlock_irqrestore(&wlan->state_lock, flags);
	netif_stop_queue(wlan->netdev);
	netif_carrier_off(wlan->netdev);
}

static void receive_channel(void *context, u8 subtype, const u8 *data,
			    size_t bytes)
{
	struct marlin2_wlan *wlan = context;
	int ret;

	if (!READ_ONCE(wlan->running) || READ_ONCE(wlan->io_error))
		return;
	ret = receive_message(wlan, data, bytes);
	if (ret) {
		fail_io(wlan, ret);
		dev_err(wlan->dev, "wireless receive failed: %pe\n",
			ERR_PTR(ret));
	}
}

static void channel_error(void *context, int error)
{
	fail_io(context, error);
}

static const struct marlin2_channel_ops marlin2_wlan_channel_ops = {
	.receive = receive_channel,
	.error = channel_error,
};

static int command(struct marlin2_wlan *wlan, u8 id, const u8 *payload,
		   size_t payload_bytes, u8 *response, size_t *response_bytes)
{
	u8 *packet;
	size_t bytes;
	unsigned long flags;
	unsigned long waited;
	int ret;

	packet = kmalloc(MARLIN2_WLAN_TX_MAX_BYTES, GFP_KERNEL);
	if (!packet)
		return -ENOMEM;
	mutex_lock(&wlan->command_lock);
	if (!READ_ONCE(wlan->running) || READ_ONCE(wlan->io_error)) {
		ret = READ_ONCE(wlan->io_error) ?: -ENETDOWN;
		goto out_unlock;
	}
	bytes = marlin2_wlan_command_build(packet, MARLIN2_WLAN_TX_MAX_BYTES,
					   id == 1 || id == 2 ? 0 : wlan->mode,
					   id, ++wlan->token, payload,
					   payload_bytes);
	if (!bytes) {
		ret = -EMSGSIZE;
		goto out_unlock;
	}
	reinit_completion(&wlan->response);
	spin_lock_irqsave(&wlan->state_lock, flags);
	wlan->pending_token = wlan->token;
	wlan->pending_command = id;
	wlan->response_error = -ETIMEDOUT;
	wlan->response_bytes = 0;
	wlan->pending = true;
	spin_unlock_irqrestore(&wlan->state_lock, flags);
	ret = marlin2_channel_write(wlan->channel, packet, bytes);
	if (!ret) {
		waited = wait_for_completion_timeout(
			&wlan->response,
			msecs_to_jiffies(MARLIN2_WLAN_COMMAND_TIMEOUT_MS));
		ret = waited ? wlan->response_error : -ETIMEDOUT;
		if (!waited)
			dev_err(wlan->dev,
				"wireless %s 0x%02x timed out: mode=%u payload_bytes=%zu token=0x%08x\n",
				__func__, id, wlan->mode, payload_bytes,
				wlan->pending_token);
		if (!ret && response_bytes) {
			if (wlan->response_bytes > *response_bytes)
				ret = -EMSGSIZE;
			else {
				memcpy(response, wlan->response_payload,
				       wlan->response_bytes);
				*response_bytes = wlan->response_bytes;
			}
		}
	}
	spin_lock_irqsave(&wlan->state_lock, flags);
	wlan->pending = false;
	spin_unlock_irqrestore(&wlan->state_lock, flags);
	if (ret == -ETIMEDOUT || (ret && ret != -EREMOTEIO && ret != -EMSGSIZE))
		fail_io(wlan, ret);
out_unlock:
	mutex_unlock(&wlan->command_lock);
	kfree(packet);
	return ret;
}

static int set_ie(struct marlin2_wlan *wlan, u8 type, const u8 *ie,
		  size_t bytes)
{
	u8 *payload;
	int ret;

	if (!bytes)
		return 0;
	if (type > 4 || bytes > (type == 2 ? 509 : 253))
		return -EMSGSIZE;
	ret = marlin2_wlan_ies_validate(ie, bytes);
	if (ret)
		return ret;
	payload = kmalloc(3 + bytes, GFP_KERNEL);
	if (!payload)
		return -ENOMEM;
	payload[0] = type;
	put_unaligned_le16(bytes, payload + 1);
	if (bytes)
		memcpy(payload + 3, ie, bytes);
	ret = command(wlan, 0x19, payload, 3 + bytes, NULL, NULL);
	kfree(payload);
	return ret;
}

static int set_beacon_ies(struct marlin2_wlan *wlan,
			  const struct cfg80211_beacon_data *beacon)
{
	int ret;

	ret = set_ie(wlan, 0, beacon->beacon_ies, beacon->beacon_ies_len);
	if (ret)
		return ret;
	ret = set_ie(wlan, 2, beacon->proberesp_ies, beacon->proberesp_ies_len);
	if (ret)
		return ret;
	return set_ie(wlan, 4, beacon->assocresp_ies,
		      beacon->assocresp_ies_len);
}

static int set_regdomain(struct marlin2_wlan *wlan)
{
	u8 payload[8 + 11 * 28] = {};
	u32 previous_start = 0;
	unsigned int rules = 0;
	unsigned int index;
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&wlan->state_lock, flags);
	memcpy(payload + 4, wlan->alpha2, 2);
	spin_unlock_irqrestore(&wlan->state_lock, flags);
	rcu_read_lock();
	for (index = 0; index < ARRAY_SIZE(wlan->channels); index++) {
		const struct ieee80211_reg_rule *rule;
		struct ieee80211_channel *channel = &wlan->channels[index];
		u8 *wire;

		if (channel->flags & IEEE80211_CHAN_DISABLED)
			continue;
		rule = freq_reg_info(wlan->wiphy, channel->center_freq * 1000);
		if (IS_ERR(rule)) {
			ret = PTR_ERR(rule);
			break;
		}
		if (previous_start == rule->freq_range.start_freq_khz)
			continue;
		previous_start = rule->freq_range.start_freq_khz;
		wire = payload + 8 + rules++ * 28;
		/* The radio consumes seven LE32 fields, not Linux's host structure. */
		put_unaligned_le32(rule->freq_range.start_freq_khz, wire);
		put_unaligned_le32(rule->freq_range.end_freq_khz, wire + 4);
		put_unaligned_le32(rule->freq_range.max_bandwidth_khz,
				   wire + 8);
		put_unaligned_le32(rule->power_rule.max_antenna_gain,
				   wire + 12);
		put_unaligned_le32(rule->power_rule.max_eirp, wire + 16);
		put_unaligned_le32(rule->flags, wire + 20);
		put_unaligned_le32(rule->dfs_cac_ms, wire + 24);
	}
	rcu_read_unlock();
	if (ret)
		return ret;
	if (!rules)
		return -ERANGE;
	put_unaligned_le32(rules, payload);
	return command(wlan, 2, payload, 8 + rules * 28, NULL, NULL);
}

static void regulatory_changed(struct wiphy *wiphy,
			       struct regulatory_request *request)
{
	struct marlin2_wlan *wlan = wlan_from_wiphy(wiphy);
	unsigned long flags;
	int ret;

	if (request) {
		spin_lock_irqsave(&wlan->state_lock, flags);
		memcpy(wlan->alpha2, request->alpha2, 2);
		spin_unlock_irqrestore(&wlan->state_lock, flags);
	}
	if (!READ_ONCE(wlan->running))
		return;
	ret = set_regdomain(wlan);
	if (ret)
		fail_io(wlan, ret);
}

static int ipv4_changed(struct notifier_block *notifier, unsigned long event,
			void *data)
{
	struct marlin2_wlan *wlan =
		container_of(notifier, struct marlin2_wlan, ipv4_notifier);
	struct in_ifaddr *address = data;
	int ret;

	if (event != NETDEV_UP || address->ifa_dev->dev != wlan->netdev ||
	    !READ_ONCE(wlan->running) || wlan->mode != MARLIN2_WLAN_MODE_STA)
		return NOTIFY_DONE;
	ret = command(wlan, 0x1a, (u8 *)&address->ifa_address, 4, NULL, NULL);
	if (ret)
		dev_warn(wlan->dev,
			 "wireless address notification failed: %pe\n",
			 ERR_PTR(ret));
	return NOTIFY_OK;
}

static void transmit_work(struct work_struct *work)
{
	struct marlin2_wlan *wlan =
		container_of(work, struct marlin2_wlan, tx_work);
	struct sk_buff *skb;

	while (READ_ONCE(wlan->running) && READ_ONCE(wlan->tx_enabled) &&
	       !READ_ONCE(wlan->io_error)) {
		unsigned long flags;
		bool shared;
		size_t bytes;
		int ret;

		spin_lock_irqsave(&wlan->state_lock, flags);
		shared = wlan->credits_shared != 0;
		if (!shared && !wlan->credits_station) {
			spin_unlock_irqrestore(&wlan->state_lock, flags);
			break;
		}
		skb = skb_dequeue(&wlan->tx_queue);
		if (!skb) {
			spin_unlock_irqrestore(&wlan->state_lock, flags);
			break;
		}
		if (shared)
			wlan->credits_shared--;
		else
			wlan->credits_station--;
		spin_unlock_irqrestore(&wlan->state_lock, flags);
		bytes = marlin2_wlan_data_build(wlan->tx_packet,
						MARLIN2_WLAN_TX_MAX_BYTES,
						wlan->mode, shared, skb->data,
						skb->len);
		if (!bytes) {
			wlan->netdev->stats.tx_dropped++;
			dev_kfree_skb(skb);
			continue;
		}
		ret = marlin2_channel_write(wlan->channel, wlan->tx_packet,
					    bytes);
		if (ret) {
			wlan->netdev->stats.tx_errors++;
			dev_kfree_skb(skb);
			fail_io(wlan, ret);
			break;
		}
		wlan->netdev->stats.tx_packets++;
		wlan->netdev->stats.tx_bytes += skb->len;
		dev_consume_skb_any(skb);
		if (skb_queue_len(&wlan->tx_queue) <
		    MARLIN2_WLAN_TX_QUEUE_MAX / 2)
			netif_wake_queue(wlan->netdev);
	}
}

static netdev_tx_t start_xmit(struct sk_buff *skb, struct net_device *netdev)
{
	struct marlin2_wlan *wlan = wlan_from_netdev(netdev);

	if (!READ_ONCE(wlan->running) || !READ_ONCE(wlan->tx_enabled) ||
	    READ_ONCE(wlan->io_error) || skb_linearize(skb)) {
		netdev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}
	skb_queue_tail(&wlan->tx_queue, skb);
	if (skb_queue_len(&wlan->tx_queue) >= MARLIN2_WLAN_TX_QUEUE_MAX)
		netif_stop_queue(netdev);
	schedule_work(&wlan->tx_work);
	return NETDEV_TX_OK;
}

static void stop_runtime(struct marlin2_wlan *wlan)
{
	WRITE_ONCE(wlan->running, false);
	WRITE_ONCE(wlan->tx_enabled, false);
	cancel_work_sync(&wlan->tx_work);
	cancel_delayed_work_sync(&wlan->scan_timeout);
	marlin2_channel_close(wlan->channel);
	finish_scan(wlan, true);
	receive_disconnect(wlan, WLAN_REASON_DEAUTH_LEAVING);
	wlan->ap_started = false;
	wlan->port_open = false;
	skb_queue_purge(&wlan->tx_queue);
}

static int open_netdev(struct net_device *netdev)
{
	struct marlin2_wlan *wlan = wlan_from_netdev(netdev);
	u8 payload[8] = {};
	u8 info[28];
	size_t info_bytes = sizeof(info);
	u8 mac[ETH_ALEN];
	int ret;

	ret = wlan->radio_ops->get_mac(wlan->radio_context, mac);
	if (ret)
		return ret;
	if (!is_valid_ether_addr(mac))
		return -EPROTONOSUPPORT;
	eth_hw_addr_set(netdev, mac);
	ether_addr_copy(netdev->perm_addr, mac);
	ether_addr_copy(wlan->wiphy->perm_addr, mac);
	netdev->addr_assign_type = NET_ADDR_PERM;
	wlan->mode = wlan->wdev.iftype == NL80211_IFTYPE_AP ?
			     MARLIN2_WLAN_MODE_AP :
			     MARLIN2_WLAN_MODE_STA;
	wlan->io_error = 0;
	wlan->credits_shared = 0;
	wlan->credits_station = 0;
	WRITE_ONCE(wlan->running, true);
	ret = marlin2_channel_open(wlan->channel);
	if (ret)
		goto out_stop;
	ret = command(wlan, 1, NULL, 0, info, &info_bytes);
	if (ret)
		goto out_stop;
	if (info_bytes != sizeof(info) ||
	    !(get_unaligned_le32(info + 16) & BIT(3))) {
		ret = -EPROTONOSUPPORT;
		goto out_stop;
	}
	wlan->wiphy->max_ap_assoc_sta = info[20];
	ret = set_regdomain(wlan);
	if (ret)
		goto out_stop;
	payload[0] = wlan->mode;
	ether_addr_copy(payload + 2, mac);
	ret = command(wlan, 3, payload, sizeof(payload), NULL, NULL);
	if (ret)
		goto out_stop;
	wlan->port_open = true;
	if (wlan->mode == MARLIN2_WLAN_MODE_STA) {
		u8 stack[3] = { MARLIN2_WLAN_MODE_STA, 1, 1 };

		ret = command(wlan, 0x43, stack, sizeof(stack), NULL, NULL);
		if (ret)
			goto out_stop;
	}
	netif_carrier_off(netdev);
	WRITE_ONCE(wlan->tx_enabled, true);
	netif_start_queue(netdev);
	return 0;

out_stop:
	stop_runtime(wlan);
	return ret;
}

static int stop_netdev(struct net_device *netdev)
{
	struct marlin2_wlan *wlan = wlan_from_netdev(netdev);
	u8 mode = wlan->mode;

	netif_stop_queue(netdev);
	netif_carrier_off(netdev);
	WRITE_ONCE(wlan->tx_enabled, false);
	cancel_work_sync(&wlan->tx_work);
	skb_queue_purge(&wlan->tx_queue);
	if (READ_ONCE(wlan->running) && wlan->port_open &&
	    !READ_ONCE(wlan->io_error))
		command(wlan, 4, &mode, sizeof(mode), NULL, NULL);
	stop_runtime(wlan);
	return 0;
}

static int change_interface(struct wiphy *wiphy, struct net_device *netdev,
			    enum nl80211_iftype type, struct vif_params *params)
{
	struct marlin2_wlan *wlan = wlan_from_wiphy(wiphy);

	if (type != NL80211_IFTYPE_STATION && type != NL80211_IFTYPE_AP)
		return -EOPNOTSUPP;
	if (netif_running(netdev))
		return -EBUSY;
	wlan->wdev.iftype = type;
	return 0;
}

static int scan(struct wiphy *wiphy, struct cfg80211_scan_request *request)
{
	struct marlin2_wlan *wlan = wlan_from_wiphy(wiphy);
	struct marlin2_wlan_ssid ssids[4];
	unsigned long flags;
	u32 channels = 0;
	u8 payload[10 + 4 * 33] = {};
	size_t bytes;
	unsigned int index;
	int ret;

	if (!READ_ONCE(wlan->running) || wlan->mode != MARLIN2_WLAN_MODE_STA)
		return -ENETDOWN;
	if (request->n_ssids > 4 ||
	    !marlin2_wlan_scan_flags_supported(request->flags))
		return -EOPNOTSUPP;
	for (index = 0; index < request->n_channels; index++) {
		unsigned int channel = request->channels[index]->hw_value;

		if (channel < 1 || channel > 11 ||
		    request->channels[index]->flags & IEEE80211_CHAN_DISABLED)
			return -EINVAL;
		channels |= BIT(channel - 1);
	}
	for (index = 0; index < request->n_ssids; index++) {
		if (request->ssids[index].ssid_len > 32)
			return -EINVAL;
		ssids[index].data = request->ssids[index].ssid;
		ssids[index].bytes = request->ssids[index].ssid_len;
	}
	bytes = marlin2_wlan_scan_build(payload, sizeof(payload), channels,
					ssids, request->n_ssids);
	if (!bytes)
		return -EINVAL;
	ret = set_ie(wlan, 1, request->ie, request->ie_len);
	if (ret)
		return ret;
	spin_lock_irqsave(&wlan->state_lock, flags);
	if (wlan->scan) {
		spin_unlock_irqrestore(&wlan->state_lock, flags);
		return -EBUSY;
	}
	wlan->scan = request;
	spin_unlock_irqrestore(&wlan->state_lock, flags);
	ret = command(wlan, 0x0b, payload, bytes, NULL, NULL);
	if (ret) {
		spin_lock_irqsave(&wlan->state_lock, flags);
		if (wlan->scan == request)
			wlan->scan = NULL;
		spin_unlock_irqrestore(&wlan->state_lock, flags);
		return ret;
	}
	mod_delayed_work(system_percpu_wq, &wlan->scan_timeout,
			 msecs_to_jiffies(MARLIN2_WLAN_SCAN_TIMEOUT_MS));
	return 0;
}

static bool is_crypto_supported(const struct cfg80211_crypto_settings *crypto)
{
	return !crypto->wpa_versions ||
	       (crypto->wpa_versions == NL80211_WPA_VERSION_2 &&
		crypto->n_ciphers_pairwise == 1 &&
		crypto->ciphers_pairwise[0] == WLAN_CIPHER_SUITE_CCMP &&
		crypto->cipher_group == WLAN_CIPHER_SUITE_CCMP &&
		crypto->n_akm_suites == 1 &&
		crypto->akm_suites[0] == WLAN_AKM_SUITE_PSK);
}

static int connect(struct wiphy *wiphy, struct net_device *netdev,
		   struct cfg80211_connect_params *params)
{
	struct marlin2_wlan *wlan = wlan_from_wiphy(wiphy);
	struct ieee80211_channel *channel = params->channel ?:
						    params->channel_hint;
	const u8 *bssid = params->bssid ?: params->bssid_hint;
	u8 payload[82] = {};
	unsigned long flags;
	int ret;

	if (!READ_ONCE(wlan->running) || wlan->mode != MARLIN2_WLAN_MODE_STA)
		return -ENETDOWN;
	if (!params->ssid_len || params->ssid_len > 32 || params->key_len ||
	    (params->auth_type != NL80211_AUTHTYPE_OPEN_SYSTEM &&
	     params->auth_type != NL80211_AUTHTYPE_AUTOMATIC) ||
	    params->mfp != NL80211_MFP_NO)
		return -EOPNOTSUPP;
	if (!is_crypto_supported(&params->crypto))
		return -EOPNOTSUPP;
	if (channel) {
		if (channel->hw_value < 1 || channel->hw_value > 11 ||
		    channel->flags & IEEE80211_CHAN_DISABLED)
			return -EINVAL;
	}
	marlin2_wlan_connect_build(payload, params->crypto.wpa_versions != 0,
				   bssid, channel ? channel->hw_value : 0,
				   params->ssid, params->ssid_len);
	ret = set_ie(wlan, 3, params->ie, params->ie_len);
	if (ret)
		return ret;
	spin_lock_irqsave(&wlan->state_lock, flags);
	if (wlan->connecting || wlan->connected) {
		spin_unlock_irqrestore(&wlan->state_lock, flags);
		return -EBUSY;
	}
	wlan->connecting = true;
	spin_unlock_irqrestore(&wlan->state_lock, flags);
	ret = command(wlan, 0x0a, payload, sizeof(payload), NULL, NULL);
	if (ret) {
		spin_lock_irqsave(&wlan->state_lock, flags);
		wlan->connecting = false;
		spin_unlock_irqrestore(&wlan->state_lock, flags);
	}
	return ret;
}

static int disconnect(struct wiphy *wiphy, struct net_device *netdev,
		      u16 reason)
{
	struct marlin2_wlan *wlan = wlan_from_wiphy(wiphy);
	u8 payload[2];

	put_unaligned_le16(reason, payload);
	return command(wlan, 0x0d, payload, sizeof(payload), NULL, NULL);
}

static int add_key(struct wiphy *wiphy, struct wireless_dev *wdev, int link_id,
		   u8 index, bool pairwise, const u8 *mac,
		   struct key_params *params)
{
	struct marlin2_wlan *wlan = wlan_from_wiphy(wiphy);
	u8 payload[19 + 16] = {};
	int ret;

	if (link_id >= 0 || index > 3 ||
	    params->cipher != WLAN_CIPHER_SUITE_CCMP || params->key_len != 16 ||
	    params->seq_len > 8 || params->seq_len < 0 || (pairwise && !mac))
		return -EOPNOTSUPP;
	ret = marlin2_wlan_key_build(payload, index, pairwise, mac, params->seq,
				     params->seq_len, params->key);
	if (ret)
		return ret;
	return command(wlan, 0x0e, payload, sizeof(payload), NULL, NULL);
}

static int delete_key(struct wiphy *wiphy, struct wireless_dev *wdev,
		      int link_id, u8 index, bool pairwise, const u8 *mac)
{
	struct marlin2_wlan *wlan = wlan_from_wiphy(wiphy);
	u8 payload[9] = { 4, index, pairwise };

	if (link_id >= 0 || index > 3 || (pairwise && !mac))
		return -EOPNOTSUPP;
	if (mac)
		ether_addr_copy(payload + 3, mac);
	return command(wlan, 0x0e, payload, sizeof(payload), NULL, NULL);
}

static int default_key(struct wiphy *wiphy, struct net_device *netdev,
		       int link_id, u8 index, bool unicast, bool multicast)
{
	struct marlin2_wlan *wlan = wlan_from_wiphy(wiphy);
	u8 payload[2] = { 2, index };

	if (link_id >= 0 || index > 3)
		return -EOPNOTSUPP;
	return command(wlan, 0x0e, payload, sizeof(payload), NULL, NULL);
}

static int change_station(struct wiphy *wiphy, struct wireless_dev *wdev,
			  const u8 *mac, struct station_parameters *params)
{
	/* The firmware completes access-point station management. */
	return 0;
}

static int delete_station(struct wiphy *wiphy, struct wireless_dev *wdev,
			  struct station_del_parameters *params)
{
	struct marlin2_wlan *wlan = wlan_from_wiphy(wiphy);
	u8 payload[8];

	if (!params->mac && !READ_ONCE(wlan->ap_started))
		return 0;
	if (!params->mac)
		eth_broadcast_addr(payload);
	else
		ether_addr_copy(payload, params->mac);
	put_unaligned_le16(params->reason_code, payload + 6);
	return command(wlan, 0x12, payload, sizeof(payload), NULL, NULL);
}

static int start_ap(struct wiphy *wiphy, struct net_device *netdev,
		    struct cfg80211_ap_settings *settings)
{
	struct marlin2_wlan *wlan = wlan_from_wiphy(wiphy);
	const struct cfg80211_beacon_data *beacon = &settings->beacon;
	u8 *payload;
	size_t bytes;
	int ret;

	if (!READ_ONCE(wlan->running) || wlan->mode != MARLIN2_WLAN_MODE_AP)
		return -ENETDOWN;
	if (!wlan->port_open) {
		u8 open[8] = { MARLIN2_WLAN_MODE_AP };

		ether_addr_copy(open + 2, netdev->dev_addr);
		ret = command(wlan, 3, open, sizeof(open), NULL, NULL);
		if (ret)
			return ret;
		wlan->port_open = true;
	}
	if (settings->chandef.chan->hw_value < 1 ||
	    settings->chandef.chan->hw_value > 11 ||
	    settings->chandef.chan->flags &
		    (IEEE80211_CHAN_DISABLED | IEEE80211_CHAN_NO_IR) ||
	    settings->chandef.width != NL80211_CHAN_WIDTH_20_NOHT ||
	    !settings->ssid_len || settings->ssid_len > 32 ||
	    settings->hidden_ssid > NL80211_HIDDEN_SSID_ZERO_CONTENTS ||
	    beacon->head_len < 38 || beacon->head[36] != WLAN_EID_SSID)
		return -EOPNOTSUPP;
	if (!is_crypto_supported(&settings->crypto))
		return -EOPNOTSUPP;
	payload = kmalloc(MARLIN2_WLAN_BEACON_MAX_BYTES + 2, GFP_KERNEL);
	if (!payload)
		return -ENOMEM;
	bytes = marlin2_wlan_beacon_build(
		payload, MARLIN2_WLAN_BEACON_MAX_BYTES + 2, beacon->head,
		beacon->head_len, beacon->tail, beacon->tail_len,
		settings->ssid, settings->ssid_len, settings->hidden_ssid);
	if (!bytes) {
		kfree(payload);
		return -EINVAL;
	}
	ret = set_beacon_ies(wlan, beacon);
	if (!ret)
		ret = command(wlan, 0x11, payload, bytes, NULL, NULL);
	kfree(payload);
	if (!ret) {
		WRITE_ONCE(wlan->ap_started, true);
		WRITE_ONCE(wlan->tx_enabled, true);
		netif_carrier_on(netdev);
		netif_wake_queue(netdev);
	}
	return ret;
}

static int stop_ap(struct wiphy *wiphy, struct net_device *netdev,
		   unsigned int link_id)
{
	struct marlin2_wlan *wlan = wlan_from_wiphy(wiphy);
	u8 mode = wlan->mode;
	int ret;

	if (link_id)
		return -EOPNOTSUPP;
	netif_stop_queue(netdev);
	WRITE_ONCE(wlan->tx_enabled, false);
	cancel_work_sync(&wlan->tx_work);
	skb_queue_purge(&wlan->tx_queue);
	ret = command(wlan, 4, &mode, 1, NULL, NULL);
	if (ret)
		return ret;
	WRITE_ONCE(wlan->ap_started, false);
	wlan->port_open = false;
	netif_carrier_off(netdev);
	return 0;
}

static int change_beacon(struct wiphy *wiphy, struct net_device *netdev,
			 struct cfg80211_ap_update *update)
{
	struct marlin2_wlan *wlan = wlan_from_wiphy(wiphy);
	struct cfg80211_beacon_data *beacon = &update->beacon;

	if (!READ_ONCE(wlan->ap_started))
		return -ENETDOWN;
	return set_beacon_ies(wlan, beacon);
}

static const struct cfg80211_ops marlin2_wlan_cfg_ops = {
	.change_virtual_intf = change_interface,
	.scan = scan,
	.connect = connect,
	.disconnect = disconnect,
	.add_key = add_key,
	.del_key = delete_key,
	.set_default_key = default_key,
	.start_ap = start_ap,
	.change_beacon = change_beacon,
	.stop_ap = stop_ap,
	.change_station = change_station,
	.del_station = delete_station,
};

static const struct net_device_ops marlin2_wlan_netdev_ops = {
	.ndo_open = open_netdev,
	.ndo_stop = stop_netdev,
	.ndo_start_xmit = start_xmit,
	.ndo_validate_addr = eth_validate_addr,
};

struct marlin2_wlan *
marlin2_wlan_register(struct device *dev,
		      const struct marlin2_wlan_radio_ops *radio_ops,
		      void *radio_context, struct marlin2_transport *transport)
{
	struct marlin2_wlan *wlan;
	unsigned int index;
	int ret;

	if (!radio_ops || !radio_ops->get_mac || !transport)
		return ERR_PTR(-EINVAL);
	wlan = kzalloc_obj(*wlan, GFP_KERNEL);
	if (!wlan)
		return ERR_PTR(-ENOMEM);
	wlan->dev = dev;
	wlan->radio_ops = radio_ops;
	wlan->radio_context = radio_context;
	wlan->alpha2[0] = '0';
	wlan->alpha2[1] = '0';
	wlan->tx_packet = kmalloc(MARLIN2_WLAN_TX_MAX_BYTES, GFP_KERNEL);
	wlan->netdev = alloc_etherdev(sizeof(struct marlin2_wlan *));
	wlan->wiphy =
		wiphy_new(&marlin2_wlan_cfg_ops, sizeof(struct marlin2_wlan *));
	if (!wlan->tx_packet || !wlan->netdev || !wlan->wiphy) {
		ret = -ENOMEM;
		goto out_free;
	}
	*(struct marlin2_wlan **)netdev_priv(wlan->netdev) = wlan;
	*(struct marlin2_wlan **)wiphy_priv(wlan->wiphy) = wlan;
	mutex_init(&wlan->command_lock);
	spin_lock_init(&wlan->state_lock);
	init_completion(&wlan->response);
	skb_queue_head_init(&wlan->tx_queue);
	INIT_WORK(&wlan->tx_work, transmit_work);
	INIT_DELAYED_WORK(&wlan->scan_timeout, scan_timeout_work);
	for (index = 0; index < ARRAY_SIZE(wlan->channels); index++) {
		wlan->channels[index].band = NL80211_BAND_2GHZ;
		wlan->channels[index].hw_value = index + 1;
		wlan->channels[index].center_freq = 2412 + index * 5;
		wlan->channels[index].max_power = 20;
	}
	wlan->band.channels = wlan->channels;
	wlan->band.n_channels = ARRAY_SIZE(wlan->channels);
	wlan->band.bitrates = marlin2_wlan_rates;
	wlan->band.n_bitrates = ARRAY_SIZE(marlin2_wlan_rates);
	wlan->wiphy->bands[NL80211_BAND_2GHZ] = &wlan->band;
	wlan->wiphy->interface_modes = BIT(NL80211_IFTYPE_STATION) |
				       BIT(NL80211_IFTYPE_AP);
	wlan->wiphy->flags |= WIPHY_FLAG_HAVE_AP_SME;
	wlan->wiphy->flags &= ~WIPHY_FLAG_PS_ON_BY_DEFAULT;
	wlan->wiphy->reg_notifier = regulatory_changed;
	wlan->wiphy->signal_type = CFG80211_SIGNAL_TYPE_MBM;
	wlan->wiphy->max_scan_ssids = 4;
	wlan->wiphy->max_scan_ie_len = 253;
	wlan->wiphy->cipher_suites = marlin2_wlan_ciphers;
	wlan->wiphy->n_cipher_suites = ARRAY_SIZE(marlin2_wlan_ciphers);
	wlan->wiphy->extended_capabilities = marlin2_wlan_extended_capabilities;
	wlan->wiphy->extended_capabilities_mask =
		marlin2_wlan_extended_capabilities_mask;
	wlan->wiphy->extended_capabilities_len =
		sizeof(marlin2_wlan_extended_capabilities);
	set_wiphy_dev(wlan->wiphy, dev);
	wlan->wdev.wiphy = wlan->wiphy;
	wlan->wdev.netdev = wlan->netdev;
	wlan->wdev.iftype = NL80211_IFTYPE_STATION;
	wlan->netdev->ieee80211_ptr = &wlan->wdev;
	wlan->netdev->netdev_ops = &marlin2_wlan_netdev_ops;
	wlan->netdev->max_mtu = ETH_DATA_LEN;
	strscpy(wlan->netdev->name, "wlan%d", IFNAMSIZ);
	SET_NETDEV_DEV(wlan->netdev, dev);
	/* The admitted device address replaces this before any radio transmission. */
	eth_hw_addr_random(wlan->netdev);
	wlan->channel = marlin2_channel_register(transport, MARLIN2_CLIENT_WLAN,
						 &marlin2_wlan_channel_ops,
						 wlan);
	if (IS_ERR(wlan->channel)) {
		ret = PTR_ERR(wlan->channel);
		goto out_free;
	}
	ret = wiphy_register(wlan->wiphy);
	if (ret)
		goto out_channel;
	ret = register_netdev(wlan->netdev);
	if (ret)
		goto out_wiphy;
	wlan->ipv4_notifier.notifier_call = ipv4_changed;
	ret = register_inetaddr_notifier(&wlan->ipv4_notifier);
	if (ret) {
		unregister_netdev(wlan->netdev);
		goto out_wiphy;
	}
	wlan->ipv4_notifier_registered = true;
	return wlan;

out_wiphy:
	wiphy_unregister(wlan->wiphy);
out_channel:
	marlin2_channel_unregister(wlan->channel);
out_free:
	if (wlan->wiphy)
		wiphy_free(wlan->wiphy);
	if (wlan->netdev)
		free_netdev(wlan->netdev);
	kfree(wlan->tx_packet);
	kfree(wlan);
	return ERR_PTR(ret);
}

void marlin2_wlan_unregister(struct marlin2_wlan *wlan)
{
	if (!wlan)
		return;
	if (wlan->ipv4_notifier_registered)
		unregister_inetaddr_notifier(&wlan->ipv4_notifier);
	unregister_netdev(wlan->netdev);
	wiphy_unregister(wlan->wiphy);
	marlin2_channel_unregister(wlan->channel);
	wiphy_free(wlan->wiphy);
	free_netdev(wlan->netdev);
	kfree(wlan->tx_packet);
	kfree(wlan);
}
