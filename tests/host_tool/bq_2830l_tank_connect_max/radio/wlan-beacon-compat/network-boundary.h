/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_WLAN_BEACON_HOST_NETWORK_BOUNDARY_H
#define FPLINUX_WLAN_BEACON_HOST_NETWORK_BOUNDARY_H

#include <string.h>
#include <linux/bitops.h>
#include <linux/completion.h>
#include <linux/spinlock.h>

/* Only registration, AP configuration and command replies are exercised. */
enum nl80211_iftype {
	NL80211_IFTYPE_STATION = 2,
	NL80211_IFTYPE_AP = 3,
};
#define NL80211_BAND_2GHZ 0
#define NL80211_CHAN_WIDTH_20_NOHT 0
#define NL80211_HIDDEN_SSID_ZERO_CONTENTS 2
#define NL80211_WPA_VERSION_2 2
#define NL80211_AUTHTYPE_OPEN_SYSTEM 0
#define NL80211_AUTHTYPE_AUTOMATIC 10
#define NL80211_MFP_NO 0
#define NL80211_SCAN_FLAG_COLOCATED_6GHZ 0x4000U
#define IEEE80211_CHAN_DISABLED BIT(0)
#define IEEE80211_CHAN_NO_IR BIT(1)
#define WLAN_CIPHER_SUITE_CCMP 0x000fac04U
#define WLAN_AKM_SUITE_PSK 0x000fac02U
#define WLAN_EID_SSID 0
#define WLAN_STATUS_SUCCESS 0
#define WLAN_STATUS_UNSPECIFIED_FAILURE 1
#define WLAN_REASON_DEAUTH_LEAVING 3
#define WIPHY_FLAG_HAVE_AP_SME BIT(17)
#define WIPHY_FLAG_PS_ON_BY_DEFAULT BIT(4)
#define CFG80211_SIGNAL_TYPE_MBM 1
#define ETH_ALEN 6
#define ETH_HLEN 14
#define ETH_DATA_LEN 1500
#define IFNAMSIZ 16
#define NET_ADDR_PERM 0
#define NETDEV_UP 1
#define NETDEV_TX_OK 0
#define NOTIFY_DONE 0
#define NOTIFY_OK 1
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define container_of(pointer, type, member) \
	((type *)((char *)(pointer) - offsetof(type, member)))
#define static_assert _Static_assert
#define dev_warn dev_err

struct wiphy;
struct net_device;
struct regulatory_request {
	char alpha2[2];
};
struct ieee80211_rate {
	int bitrate, hw_value;
};
struct ieee80211_channel {
	unsigned int band, hw_value, center_freq, max_power, flags;
};
struct ieee80211_supported_band {
	struct ieee80211_channel *channels;
	struct ieee80211_rate *bitrates;
	unsigned int n_channels, n_bitrates;
};
struct wireless_dev {
	struct wiphy *wiphy;
	struct net_device *netdev;
	enum nl80211_iftype iftype;
};
struct cfg80211_beacon_data {
	const u8 *head, *tail, *beacon_ies, *proberesp_ies, *assocresp_ies;
	size_t head_len, tail_len, beacon_ies_len, proberesp_ies_len;
	size_t assocresp_ies_len;
};
struct cfg80211_ap_update {
	struct cfg80211_beacon_data beacon;
};
struct cfg80211_crypto_settings {
	u32 wpa_versions, cipher_group;
	int n_ciphers_pairwise, n_akm_suites;
	u32 ciphers_pairwise[5], akm_suites[5];
};
struct cfg80211_ap_settings {
	struct {
		struct ieee80211_channel *chan;
		int width;
	} chandef;
	struct cfg80211_beacon_data beacon;
	struct cfg80211_crypto_settings crypto;
	const u8 *ssid;
	size_t ssid_len;
	u8 hidden_ssid;
};
struct cfg80211_scan_info {
	bool aborted;
};
struct cfg80211_ssid {
	u8 ssid[32];
	size_t ssid_len;
};
struct cfg80211_scan_request {
	unsigned int n_ssids, n_channels;
	u32 flags;
	struct cfg80211_ssid *ssids;
	struct ieee80211_channel **channels;
	const u8 *ie;
	size_t ie_len;
};
struct cfg80211_connect_params {
	struct ieee80211_channel *channel, *channel_hint;
	const u8 *bssid, *bssid_hint, *ssid, *ie;
	size_t ssid_len, key_len, ie_len;
	int auth_type, mfp;
	struct cfg80211_crypto_settings crypto;
};
struct key_params {
	u32 cipher;
	int key_len, seq_len;
	const u8 *key, *seq;
};
struct vif_params {
	int unused;
};
struct station_parameters {
	int unused;
};
struct station_del_parameters {
	const u8 *mac;
	u16 reason_code;
};
struct station_info {
	const u8 *assoc_req_ies;
	size_t assoc_req_ies_len;
};
struct cfg80211_bss;
struct ieee80211_mgmt;
struct cfg80211_ops {
	int (*change_virtual_intf)(struct wiphy *, struct net_device *,
				   enum nl80211_iftype, struct vif_params *);
	int (*scan)(struct wiphy *, struct cfg80211_scan_request *);
	int (*connect)(struct wiphy *, struct net_device *,
		       struct cfg80211_connect_params *);
	int (*disconnect)(struct wiphy *, struct net_device *, u16);
	int (*add_key)(struct wiphy *, struct wireless_dev *, int, u8, bool,
		       const u8 *, struct key_params *);
	int (*del_key)(struct wiphy *, struct wireless_dev *, int, u8, bool,
		       const u8 *);
	int (*set_default_key)(struct wiphy *, struct net_device *, int, u8,
			       bool, bool);
	int (*start_ap)(struct wiphy *, struct net_device *,
			struct cfg80211_ap_settings *);
	int (*change_beacon)(struct wiphy *, struct net_device *,
			     struct cfg80211_ap_update *);
	int (*stop_ap)(struct wiphy *, struct net_device *, unsigned int);
	int (*change_station)(struct wiphy *, struct wireless_dev *, const u8 *,
			      struct station_parameters *);
	int (*del_station)(struct wiphy *, struct wireless_dev *,
			   struct station_del_parameters *);
};
struct wiphy {
	const struct cfg80211_ops *ops;
	void *private_data;
	struct device *dev;
	struct ieee80211_supported_band *bands[1];
	unsigned long interface_modes, flags;
	void (*reg_notifier)(struct wiphy *, struct regulatory_request *);
	unsigned int signal_type, max_scan_ssids, max_scan_ie_len;
	const u32 *cipher_suites;
	unsigned int n_cipher_suites;
	const u8 *extended_capabilities, *extended_capabilities_mask;
	size_t extended_capabilities_len;
	unsigned int max_ap_assoc_sta;
	u8 perm_addr[6];
};
struct sk_buff {
	size_t len;
	u8 *data;
	u16 protocol;
};
struct sk_buff_head {
	unsigned int length;
};
typedef int netdev_tx_t;
struct net_device_ops {
	int (*ndo_open)(struct net_device *);
	int (*ndo_stop)(struct net_device *);
	netdev_tx_t (*ndo_start_xmit)(struct sk_buff *, struct net_device *);
	int (*ndo_validate_addr)(struct net_device *);
};
struct net_device {
	void *private_data;
	struct device *dev;
	struct wireless_dev *ieee80211_ptr;
	const struct net_device_ops *netdev_ops;
	unsigned int max_mtu, addr_assign_type;
	u8 dev_addr[6], perm_addr[6];
	char name[16];
	bool running, carrier, queue_started;
	struct {
		unsigned long rx_dropped, rx_packets, rx_bytes;
		unsigned long tx_dropped, tx_packets, tx_bytes, tx_errors;
	} stats;
};
struct notifier_block {
	int (*notifier_call)(struct notifier_block *, unsigned long, void *);
};
struct in_device {
	struct net_device *dev;
};
struct in_ifaddr {
	struct in_device *ifa_dev;
	u32 ifa_address;
};
struct ieee80211_reg_rule {
	struct {
		u32 start_freq_khz, end_freq_khz, max_bandwidth_khz;
	} freq_range;
	struct {
		u32 max_antenna_gain, max_eirp;
	} power_rule;
	u32 flags, dfs_cac_ms;
};
struct work_struct {
	void (*function)(struct work_struct *);
};
struct delayed_work {
	struct work_struct work;
};
#define INIT_WORK(item, callback) ((item)->function = (callback))
#define INIT_DELAYED_WORK(item, callback) INIT_WORK(&(item)->work, callback)
#define to_delayed_work(pointer) \
	container_of(pointer, struct delayed_work, work)

struct wiphy *wiphy_new(const struct cfg80211_ops *ops, size_t bytes);
int wiphy_register(struct wiphy *wiphy);
void wiphy_unregister(struct wiphy *wiphy);
void wiphy_free(struct wiphy *wiphy);
struct net_device *alloc_etherdev(size_t bytes);
int register_netdev(struct net_device *netdev);
void unregister_netdev(struct net_device *netdev);
void free_netdev(struct net_device *netdev);
const struct ieee80211_reg_rule *freq_reg_info(struct wiphy *wiphy,
					       unsigned int khz);
int register_inetaddr_notifier(struct notifier_block *notifier);
int unregister_inetaddr_notifier(struct notifier_block *notifier);
static inline void *wiphy_priv(struct wiphy *wiphy)
{
	return wiphy->private_data;
}
static inline void *netdev_priv(struct net_device *netdev)
{
	return netdev->private_data;
}
static inline void set_wiphy_dev(struct wiphy *wiphy, struct device *dev)
{
	wiphy->dev = dev;
}
#define SET_NETDEV_DEV(netdev, device) ((netdev)->dev = (device))
static inline bool netif_running(struct net_device *netdev)
{
	return netdev->running;
}
static inline void netif_carrier_on(struct net_device *netdev)
{
	netdev->carrier = true;
}
static inline void netif_carrier_off(struct net_device *netdev)
{
	netdev->carrier = false;
}
static inline void netif_start_queue(struct net_device *netdev)
{
	netdev->queue_started = true;
}
static inline void netif_stop_queue(struct net_device *netdev)
{
	netdev->queue_started = false;
}
static inline void netif_wake_queue(struct net_device *netdev)
{
	netif_start_queue(netdev);
}
static inline void ether_addr_copy(u8 *dest, const u8 *source)
{
	memcpy(dest, source, 6);
}
static inline void eth_broadcast_addr(u8 *dest)
{
	memset(dest, 0xff, 6);
}
static inline bool is_valid_ether_addr(const u8 *address)
{
	static const u8 zero[6];
	return !(address[0] & 1) && memcmp(address, zero, 6);
}
static inline void eth_hw_addr_set(struct net_device *netdev, const u8 *address)
{
	ether_addr_copy(netdev->dev_addr, address);
}
static inline void eth_hw_addr_random(struct net_device *netdev)
{
	const u8 address[6] = { 2, 0, 0, 0, 0, 1 };
	eth_hw_addr_set(netdev, address);
}
static inline int eth_validate_addr(struct net_device *netdev)
{
	return is_valid_ether_addr(netdev->dev_addr) ? 0 : -EADDRNOTAVAIL;
}
static inline void strscpy(char *dest, const char *source, size_t bytes)
{
	assert(bytes && strlen(source) < bytes);
	memcpy(dest, source, strlen(source) + 1);
}
static inline void skb_queue_head_init(struct sk_buff_head *queue)
{
	queue->length = 0;
}
static inline void skb_queue_purge(struct sk_buff_head *queue)
{
	assert(!queue->length);
}
static inline bool cancel_work_sync(struct work_struct *work)
{
	return false;
}
static inline bool cancel_delayed_work_sync(struct delayed_work *work)
{
	return false;
}
static inline void rcu_read_lock(void)
{
}
static inline void rcu_read_unlock(void)
{
}

/* Unexpected data, scan, connection or scheduling activity must fail visibly. */
static inline struct sk_buff *netdev_alloc_skb_ip_align(struct net_device *dev,
							size_t bytes)
{
	abort();
}
static inline void skb_put_data(struct sk_buff *skb, const void *data,
				size_t bytes)
{
	abort();
}
static inline u16 eth_type_trans(struct sk_buff *skb, struct net_device *dev)
{
	abort();
}
static inline int netif_rx(struct sk_buff *skb)
{
	abort();
}
static inline struct sk_buff *skb_dequeue(struct sk_buff_head *queue)
{
	abort();
}
static inline void skb_queue_tail(struct sk_buff_head *queue,
				  struct sk_buff *skb)
{
	abort();
}
static inline unsigned int skb_queue_len(const struct sk_buff_head *queue)
{
	abort();
}
static inline int skb_linearize(struct sk_buff *skb)
{
	abort();
}
static inline void dev_kfree_skb(struct sk_buff *skb)
{
	abort();
}
static inline void dev_kfree_skb_any(struct sk_buff *skb)
{
	abort();
}
static inline void dev_consume_skb_any(struct sk_buff *skb)
{
	abort();
}
static inline bool schedule_work(struct work_struct *work)
{
	abort();
}
static inline bool cancel_delayed_work(struct delayed_work *work)
{
	abort();
}
static inline bool mod_delayed_work(void *queue, struct delayed_work *work,
				    unsigned long delay)
{
	abort();
}
#define system_percpu_wq NULL
static inline void cfg80211_scan_done(struct cfg80211_scan_request *request,
				      struct cfg80211_scan_info *info)
{
	abort();
}
static inline void cfg80211_connect_result(struct net_device *netdev,
					   const u8 *bssid, const u8 *request,
					   size_t request_bytes,
					   const u8 *response,
					   size_t response_bytes, u16 status,
					   int flags)
{
	abort();
}
static inline void cfg80211_disconnected(struct net_device *netdev, u16 reason,
					 const u8 *ie, size_t bytes, bool local,
					 int flags)
{
	abort();
}
static inline int ieee80211_channel_to_frequency(int channel, int band)
{
	abort();
}
static inline struct ieee80211_channel *
ieee80211_get_channel(struct wiphy *wiphy, int frequency)
{
	abort();
}
static inline struct cfg80211_bss *cfg80211_inform_bss_frame(
	struct wiphy *wiphy, struct ieee80211_channel *channel,
	struct ieee80211_mgmt *frame, size_t bytes, int signal, int flags)
{
	abort();
}
static inline void cfg80211_put_bss(struct wiphy *wiphy,
				    struct cfg80211_bss *bss)
{
	abort();
}
static inline void cfg80211_del_sta(struct wireless_dev *wdev, const u8 *mac,
				    int flags)
{
	abort();
}
static inline void cfg80211_new_sta(struct wireless_dev *wdev, const u8 *mac,
				    struct station_info *info, int flags)
{
	abort();
}

#endif
