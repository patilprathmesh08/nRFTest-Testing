#include "wifi_net.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(wifi_net, CONFIG_LOG_DEFAULT_LEVEL);

#include <zephyr/kernel.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi.h>
#include <zephyr/net/wifi_mgmt.h>
#include <errno.h>
#include <string.h>

#define WIFI_MGMT_EVENTS (NET_EVENT_WIFI_CONNECT_RESULT | \
			  NET_EVENT_WIFI_DISCONNECT_RESULT | \
			  NET_EVENT_WIFI_SCAN_RESULT | \
			  NET_EVENT_WIFI_SCAN_DONE)
#define IPV4_MGMT_EVENTS (NET_EVENT_IPV4_DHCP_BOUND)

/* Handed to the driver as the association attempt's own timeout via
 * struct wifi_connect_req_params::timeout. Documented as "Connect timeout
 * in seconds, SYS_FOREVER_MS for no timeout". */
#define WIFI_CONNECT_TIMEOUT_S    15

/* App-level backstop for how long we wait for a CONNECT_RESULT event -
 * without this, a stuck RPU (nRF70 radio) or an association that never
 * completes hangs forever with zero log output, indistinguishable from a
 * crash on the serial log. */
#define WIFI_CONNECT_WAIT_MAX_MS  20000

/* App-level backstop for how long we wait for a DHCP lease after Wi-Fi
 * association succeeds. 15s comfortably covers a normal DHCP exchange
 * (DISCOVER/OFFER/REQUEST/ACK) plus router latency; without a cap, a
 * router that's slow or out of leases would hang the board here forever
 * with no log output. */
#define DHCP_BOUND_WAIT_MAX_MS    15000

/* A full dual-band scan covers far more channels than a 2.4 GHz-only one,
 * and 5 GHz DFS channels must be scanned PASSIVELY - the radio has to sit
 * and listen for a beacon rather than probe for one - so this is
 * generously larger than any connect timeout. Diagnostic path only; at
 * worst it delays boot by this much.
 */
#define WIFI_SCAN_WAIT_MAX_MS     20000

#define STATUS_POLLING_MS   300

static struct net_mgmt_event_callback wifi_mgmt_cb;
static struct net_mgmt_event_callback ipv4_mgmt_cb; /* separate cb: different event mask (L3 IPv4, not L2 Wi-Fi) */

static struct {
	uint8_t connected	: 1;
	uint8_t connect_result	: 1;
	uint8_t dhcp_bound	: 1; /* set once NET_EVENT_IPV4_DHCP_BOUND fires */
} context;

/* ---------------- Pre-connect survey scan ----------------
 *
 * Exists because WIFI_STATUS_CONN_AP_NOT_FOUND (status 4) on its own does
 * not say WHY, and a 2.4 GHz-only laptop adapter physically cannot see a
 * 5 GHz network, so it can neither confirm nor deny that a 5 GHz SSID
 * exists.
 *
 * The nRF7002 is dual-band, so it is the authoritative instrument here.
 * This logs every AP it can see with band, channel, RSSI and security,
 * then states explicitly whether the configured SSID was among them -
 * turning "connection failed" into either "your SSID is not on the air"
 * or "it is there, so the problem is elsewhere". Those two have
 * completely different fixes, which is why the distinction is worth a
 * scan.
 *
 * Diagnostic only, and compiled out entirely unless CONFIG_WIFI_SURVEY_SCAN
 * is set - a scan costs seconds of boot time and a burst of UART traffic,
 * neither of which a production build should pay for.
 */
#ifdef CONFIG_WIFI_SURVEY_SCAN

static uint8_t scan_ap_count;
static bool    scan_found_configured_ssid;

/* Set in the net_mgmt callback (a different context from the waiting
 * thread) and polled below, so it must not be cached in a register. */
static volatile bool scan_done;

static void handle_wifi_scan_result(struct net_mgmt_event_callback *cb)
{
	const struct wifi_scan_result *e = (const struct wifi_scan_result *)cb->info;

	scan_ap_count++;

	/* e->ssid is NOT guaranteed NUL-terminated - ssid_length is the
	 * authority - so this prints with an explicit precision rather than
	 * %s, which would run off the end of a full 32-byte SSID.
	 */
	/* wifi_band_txt() and wifi_security_txt() are Zephyr's own helpers,
	 * used in preference to a local lookup so this cannot drift out of
	 * step when Zephyr adds a band or a security type.
	 *
	 * The cast is required and safe: wifi_scan_result::band is declared
	 * uint8_t (wifi_mgmt.h:604) while wifi_band_txt() takes
	 * enum wifi_frequency_bands - the values are the same, only the
	 * declared types differ. */
	LOG_INF("  %-32.*s  %-5s  ch%-3u  %4d dBm  %s",
		e->ssid_length, e->ssid,
		wifi_band_txt((enum wifi_frequency_bands)e->band),
		e->channel, e->rssi, wifi_security_txt(e->security));

	/* Compare on LENGTH first, then bytes. strncmp() alone would report
	 * \"TP-Link_A078\" as a match for \"TP-Link_A078_5G\", which is
	 * precisely the confusion this scan exists to resolve.
	 */
	if (e->ssid_length == strlen(CONFIG_WIFI_CREDENTIALS_STATIC_SSID) &&
	    memcmp(e->ssid, CONFIG_WIFI_CREDENTIALS_STATIC_SSID,
		   e->ssid_length) == 0) {
		scan_found_configured_ssid = true;
	}
}

static void handle_wifi_scan_done(struct net_mgmt_event_callback *cb)
{
	const struct wifi_status *status = (const struct wifi_status *)cb->info;

	if (status->status) {
		LOG_ERR("--- scan FAILED (%d) ---", status->status);
	} else {
		LOG_INF("--- scan done: %u AP(s) ---", scan_ap_count);
	}

	if (scan_found_configured_ssid) {
		LOG_INF("Configured SSID \"%s\" WAS found in the scan",
			CONFIG_WIFI_CREDENTIALS_STATIC_SSID);
	} else {
		LOG_ERR("Configured SSID \"%s\" was NOT found - check the exact "
			"name, whether that radio is enabled on the router, and "
			"that CONFIG_NRF70_REG_DOMAIN permits its channel",
			CONFIG_WIFI_CREDENTIALS_STATIC_SSID);
	}

	scan_done = true;
}

/* Runs a scan and blocks until it finishes or times out. Failure is
 * non-fatal on purpose: this is a diagnostic, and a board that cannot
 * scan should still be allowed to attempt the connection it was
 * configured for.
 */
static void wifi_survey_scan(void)
{
	struct net_if *iface = net_if_get_default();
	int64_t start;

	scan_ap_count = 0;
	scan_found_configured_ssid = false;
	scan_done = false;

	LOG_INF("--- scanning (looking for \"%s\") ---",
		CONFIG_WIFI_CREDENTIALS_STATIC_SSID);
	LOG_INF("  %-32s  %-5s  %-5s  %8s  %s",
		"SSID", "BAND", "CH", "RSSI", "SECURITY");

	/* NULL params = scan every band and channel the regulatory domain
	 * allows. Deliberate: restricting the scan to the band we intend to
	 * use would hide the very thing being looked for, namely the SSID
	 * sitting on the OTHER band.
	 */
	if (net_mgmt(NET_REQUEST_WIFI_SCAN, iface, NULL, 0)) {
		LOG_WRN("Scan request failed - continuing to connect anyway");
		return;
	}

	start = k_uptime_get();
	while (!scan_done) {
		if ((k_uptime_get() - start) > WIFI_SCAN_WAIT_MAX_MS) {
			LOG_WRN("Scan did not complete in %d ms (%u AP(s) so far) - "
				"continuing to connect anyway",
				WIFI_SCAN_WAIT_MAX_MS, scan_ap_count);
			return;
		}
		k_sleep(K_MSEC(STATUS_POLLING_MS));
	}
}

#endif /* CONFIG_WIFI_SURVEY_SCAN */

static void handle_wifi_connect_result(struct net_mgmt_event_callback *cb)
{
	const struct wifi_status *status = (const struct wifi_status *) cb->info;
	if (context.connected) return;
	if (status->status) {
		LOG_ERR("Connection failed (%d)", status->status);
	} else {
		LOG_INF("Connected");
		context.connected = 1;
	}
	context.connect_result = 1;
}

static void wifi_mgmt_event_handler(struct net_mgmt_event_callback *cb,
				     uint64_t mgmt_event, struct net_if *iface)
{
	ARG_UNUSED(iface);

	if (mgmt_event == NET_EVENT_WIFI_CONNECT_RESULT) {
		handle_wifi_connect_result(cb);
#ifdef CONFIG_WIFI_SURVEY_SCAN
	} else if (mgmt_event == NET_EVENT_WIFI_SCAN_RESULT) {
		handle_wifi_scan_result(cb);
	} else if (mgmt_event == NET_EVENT_WIFI_SCAN_DONE) {
		handle_wifi_scan_done(cb);
#endif
	} else if (mgmt_event == NET_EVENT_WIFI_DISCONNECT_RESULT) {
		LOG_ERR("Wi-Fi disconnected!");
		context.connected = 0;
		/* Address is no longer valid once the link drops - the next
		 * connect will get a fresh lease (or renew), so don't leave
		 * a stale "bound" flag set. */
		context.dhcp_bound = 0;
	}
}

/* Logs whatever IPv4 address DHCP actually handed us. Uses the official
 * net_if_ipv4_get_global_addr() accessor rather than reaching into
 * iface->config.ip.ipv4->unicast[] directly, so this doesn't depend on
 * the exact internal layout of that struct (which has changed across
 * Zephyr/NCS versions). */
static void log_ipv4_addr(struct net_if *iface)
{
	struct in_addr *addr = net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);

	if (addr != NULL) {
		char addr_str[NET_IPV4_ADDR_LEN];

		net_addr_ntop(AF_INET, addr, addr_str, sizeof(addr_str));
		LOG_INF("DHCP bound, IP address: %s", addr_str);
	} else {
		LOG_INF("DHCP bound (address not readable yet)");
	}
}

/* Fires once the DHCP client finishes the DISCOVER/OFFER/REQUEST/ACK
 * exchange and installs an address on the interface. Before this point
 * the board has no IPv4 address at all.
 *
 * NOTE: some DHCP servers (seen in the wild with certain Windows Server
 * DHCP setups) are known to not reliably fire this event even though the
 * lease was obtained fine. wifi_net_connect_and_wait()'s wait loop below
 * therefore doesn't rely on this callback alone - it also polls
 * net_if_ipv4_get_global_addr() directly as a fallback, so a missed event
 * can't hang the board forever. */
static void ipv4_mgmt_event_handler(struct net_mgmt_event_callback *cb,
				     uint64_t mgmt_event, struct net_if *iface)
{
	ARG_UNUSED(cb);

	if (mgmt_event != NET_EVENT_IPV4_DHCP_BOUND) {
		return;
	}

	context.dhcp_bound = 1;
	log_ipv4_addr(iface);
}

static int wifi_connect(void)
{
	struct net_if *iface = net_if_get_default();
	static struct wifi_connect_req_params params;

	context.connected = 0;
	context.connect_result = 0;

	params.ssid = CONFIG_WIFI_CREDENTIALS_STATIC_SSID;
	params.ssid_length = strlen(params.ssid);
	params.security = WIFI_SECURITY_TYPE_PSK;  /* WPA2-PSK */
	params.psk = CONFIG_WIFI_CREDENTIALS_STATIC_PASSWORD;
	params.psk_length = strlen(params.psk);
	params.band = WIFI_FREQ_BAND_5_GHZ;
	params.channel = WIFI_CHANNEL_ANY;
	params.mfp = WIFI_MFP_OPTIONAL;
	params.timeout = WIFI_CONNECT_TIMEOUT_S;  /* seconds, per official docs */

	if (net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &params, sizeof(params))) {
		LOG_ERR("Connection request failed");
		return -ENOEXEC;
	}
	return 0;
}

int wifi_net_connect_and_wait(void)
{
	net_mgmt_init_event_callback(&wifi_mgmt_cb, wifi_mgmt_event_handler,
				      WIFI_MGMT_EVENTS);
	net_mgmt_add_event_callback(&wifi_mgmt_cb);

	/* Separate callback for IPv4 (L3) events - different mask/layer than
	 * the Wi-Fi (L2) events above, so per Zephyr's net_mgmt docs this
	 * needs its own net_mgmt_event_callback struct rather than reusing
	 * wifi_mgmt_cb. */
	net_mgmt_init_event_callback(&ipv4_mgmt_cb, ipv4_mgmt_event_handler,
				      IPV4_MGMT_EVENTS);
	net_mgmt_add_event_callback(&ipv4_mgmt_cb);

#ifdef CONFIG_WIFI_SURVEY_SCAN
	/* Runs BEFORE the connect attempt so the survey is in the log even
	 * when the connection then fails - which is the case it exists for.
	 */
	wifi_survey_scan();
#endif

	if (wifi_connect() != 0) {
		LOG_ERR("Wi-Fi connect request failed, halting");
		return -ENOEXEC;
	}

	{
		int64_t wait_start = k_uptime_get();
		int poll_count = 0;

		/* WIFI_CONNECT_WAIT_MAX_MS caps how long we'll sit here. Without
		 * this, a stuck RPU (nRF70 radio) or an association that never
		 * completes just hangs forever with zero log output, which is
		 * indistinguishable from a crash on the serial log. */
		while (!context.connect_result) {
			if ((k_uptime_get() - wait_start) > WIFI_CONNECT_WAIT_MAX_MS) {
				LOG_ERR("No Wi-Fi connect result after %d ms - driver/RF "
					"issue likely (check SSID/password/band match "
					"your AP; consider enabling the "
					"wpa-supplicant-debug build snippet for more "
					"detail), halting",
					WIFI_CONNECT_WAIT_MAX_MS);
				return -ETIMEDOUT;
			}

			k_sleep(K_MSEC(STATUS_POLLING_MS));
			poll_count++;

			/* Visible heartbeat roughly every 5s instead of silence,
			 * so a stall is obvious in the serial log rather than
			 * looking identical to a crash or a hung board. */
			if (poll_count % (5000 / STATUS_POLLING_MS) == 0) {
				LOG_INF("Still waiting for Wi-Fi connect result... "
					"(%d ms elapsed)",
					poll_count * STATUS_POLLING_MS);
			}
		}
	}

	if (!context.connected) {
		LOG_ERR("Wi-Fi connect failed, halting");
		return -ENOTCONN;
	}

	/* Wi-Fi association succeeding only means we're on the network - it
	 * does NOT mean we have an IP address yet; a real DHCP exchange has
	 * to complete first. So wait for it here rather than letting
	 * whatever the caller does next fail confusingly with "network
	 * unreachable"/ENOMEM-looking errors that are actually just "no IP
	 * yet". */
	{
		struct net_if *dhcp_iface = net_if_get_default();
		int64_t wait_start = k_uptime_get();
		int poll_count = 0;

		while (!context.dhcp_bound) {
			/* Fallback for the known case (see ipv4_mgmt_event_handler's
			 * comment) where some DHCP servers don't reliably fire
			 * NET_EVENT_IPV4_DHCP_BOUND even though the lease is fine -
			 * check the interface directly so a missed event doesn't
			 * hang the board forever. */
			if (net_if_ipv4_get_global_addr(dhcp_iface, NET_ADDR_PREFERRED) != NULL) {
				LOG_WRN("DHCP address present but DHCP_BOUND event "
					"never fired (known issue with some DHCP "
					"servers) - proceeding anyway");
				context.dhcp_bound = 1;
				break;
			}

			if ((k_uptime_get() - wait_start) > DHCP_BOUND_WAIT_MAX_MS) {
				LOG_ERR("No DHCP lease after %d ms - check the router "
					"has DHCP enabled and isn't out of leases, "
					"halting",
					DHCP_BOUND_WAIT_MAX_MS);
				return -ETIMEDOUT;
			}

			k_sleep(K_MSEC(STATUS_POLLING_MS));
			poll_count++;

			if (poll_count % (5000 / STATUS_POLLING_MS) == 0) {
				LOG_INF("Still waiting for DHCP lease... (%d ms elapsed)",
					poll_count * STATUS_POLLING_MS);
			}
		}
	}

	{
		struct wifi_ps_params ps_params = { .enabled = WIFI_PS_DISABLED };
		struct net_if *ps_iface = net_if_get_default();

		if (net_mgmt(NET_REQUEST_WIFI_PS, ps_iface, &ps_params, sizeof(ps_params))) {
			LOG_ERR("Failed to disable Wi-Fi power save");
		}
	}

	return 0;
}
