#ifndef WIFI_NET_H_
#define WIFI_NET_H_

/* Wi-Fi + DHCP bring-up module.
 *
 * Owns Wi-Fi association and the DHCP lease wait: registers the Wi-Fi
 * (L2) and IPv4 (L3/DHCP) net_mgmt event callbacks, requests a
 * connection, blocks until both a connect result and a DHCP lease are in
 * (or their respective timeouts elapse), and disables Wi-Fi power-save
 * once connected.
 *
 * Uses CONFIG_WIFI_CREDENTIALS_STATIC_SSID / _PASSWORD (from prj.conf)
 * for the network credentials - not parameters, since this app only ever
 * connects to one configured network.
 */

/* Call once, early in main(), before anything that needs the network
 * (TLS, NTP, etc.). Returns 0 once Wi-Fi is associated and a DHCP lease
 * is bound. Returns a negative error if the connect request itself
 * failed to submit, if no connect result arrived in time, if the
 * connection failed, or if no DHCP lease was obtained in time - each
 * case is already logged with the specific reason before returning. */
int wifi_net_connect_and_wait(void);

#endif /* WIFI_NET_H_ */
