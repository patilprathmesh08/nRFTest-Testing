#include "https_conn.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(https_conn, CONFIG_LOG_DEFAULT_LEVEL);

#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/net/dns_resolve.h>
#include <errno.h>
#include <string.h>
#include <mbedtls/ssl_ciphersuites.h>

#include "epoch.h"
#include "frame.h"
#include "joystick.h"
#include "led.h"
#include "ws.h"

/* ---------------- Deployment configuration ----------------
 * The WSS server this board talks to. Independent of epoch.c's
 * NTP_SERVER (main.c) - they can point at different hosts, even though
 * this deployment happens to run both on the same PC.
 *
 * WSS_HOST is the target's mDNS name rather than an IP literal, so a
 * DHCP address change on the i.MX8MP does not need a firmware rebuild -
 * it runs Avahi and advertises itself as "verdin-imx8mp.local". The name
 * is resolved at connect time via zsock_getaddrinfo() (see
 * resolve_wss_host()); the Kconfig required for .local lookups is
 * documented in prj.conf under "mDNS (.local) name resolution".
 *
 * WSS_HOST may still be a plain IP literal - zsock_getaddrinfo() shortcuts
 * numeric strings through try_resolve_literal_addr() without emitting any
 * query - so the PC_SIMULATORBASED branch below needs no special casing. */
#ifdef PC_SIMULATORBASED
	#define WSS_HOST     "192.168.40.132"
	#define WSS_PORT     443
#else
	#define WSS_HOST     "verdin-imx8mp.local"
	#define WSS_PORT     8081
#endif
#define TLS_SEC_TAG  42

/* How often the name is re-checked while it is not resolvable, e.g. the
 * board booted before the i.MX8MP finished bringing Avahi up. Measured
 * from the START of one attempt to the start of the next, so a failed
 * attempt's own duration (up to CONFIG_NET_SOCKETS_DNS_TIMEOUT, 2000 ms)
 * is absorbed into this period rather than added on top of it - the
 * observed cadence really is 5 s, not 7 s. */
#define RESOLVE_RETRY_MS   5000

/* SNI (TLS_HOSTNAME) sent in the handshake.
 *   0 = send the resolved IP literal, exactly as this file did before
 *       WSS_HOST became a name. The server's certificate carries an
 *       IP-type SAN, so this is the value it was issued for.
 *   1 = send WSS_HOST itself, which is the conventional use of SNI.
 * Left at 0 deliberately: TLS_PEER_VERIFY is NONE here (see the security
 * note in https_conn.h), so SNI has no bearing on verification. Flip to 1
 * if the server is ever given a DNS-type SAN and made to select its
 * certificate by SNI. */
#define WSS_TLS_SNI_USE_HOSTNAME  0

static int https_sock = -1;   /* persistent HTTPS (TLS) socket, connected once */

/* client_certificate[] and client_private_key[] go here — generate with
 * gen_cert_arrays.py (reads certs/client.crt and certs/client.key) and
 * paste the two arrays from client_certs.h in below, replacing this
 * comment block. Needed for mTLS: the server requires the board to
 * present a client cert, not just verify the server's. */
#include "client_certs.h"

/* Idempotent: -EEXIST from a retried provisioning attempt is not a real
 * failure. */
int https_conn_provision_cert(void)
{
	int ret;

	/* No CA cert is provisioned — the board does not verify the
	 * server's identity (see the security note in https_conn.h). */

	/* mTLS: the board presents its own client certificate and private
	 * key under sec_tag 42. The server has CERT_REQUIRED set, so
	 * without these two credentials the handshake fails with
	 * PEER_DID_NOT_RETURN_A_CERTIFICATE. */
	ret = tls_credential_add(TLS_SEC_TAG, TLS_CREDENTIAL_PUBLIC_CERTIFICATE,
				  client_certificate, sizeof(client_certificate));
	if (ret == -EEXIST) {
		LOG_INF("Client cert already provisioned from a previous attempt — continuing");
	} else if (ret < 0) {
		LOG_ERR("Failed to add client cert: %d", ret);
		return ret;
	}

	ret = tls_credential_add(TLS_SEC_TAG, TLS_CREDENTIAL_PRIVATE_KEY,
				  client_private_key, sizeof(client_private_key));
	if (ret == -EEXIST) {
		LOG_INF("Client private key already provisioned from a previous attempt — continuing");
		return 0;
	}
	if (ret < 0) {
		LOG_ERR("Failed to add client private key: %d", ret);
	}
	return ret;
}

/* Resolves WSS_HOST to a single IPv4 address.
 *
 * For the ".local" name this goes out as an mDNS query to 224.0.0.251:5353
 * (Zephyr routes any name ending in ".local" to the multicast address
 * instead of a DNS server - resolve.c:2088), which the i.MX8MP's Avahi
 * answers. For a numeric WSS_HOST it is parsed locally with no query at
 * all, so the PC_SIMULATORBASED build behaves exactly as before.
 *
 * BLOCKING: takes up to CONFIG_NET_SOCKETS_DNS_TIMEOUT (2000 ms) to fail.
 * Only ever called from https_conn_open() (main thread, during setup) or
 * from the sender thread's reconnect path - never from the sampler, so
 * the sampling cadence is untouched either way.
 *
 * On success *out receives the address AND *out_str receives its dotted-quad
 * text, which is what gets passed as SNI and printed in the connect log.
 * Returns 0 on success, -1 if the name could not be resolved. */
static int resolve_wss_host(struct in_addr *out, char *out_str, size_t out_str_len)
{
	struct zsock_addrinfo hints;
	struct zsock_addrinfo *res = NULL;
	int ret;

	memset(&hints, 0, sizeof(hints));
	/* AF_INET only: CONFIG_NET_IPV6 is =n in prj.conf, and asking for
	 * AF_UNSPEC would make getaddrinfo() issue a second, pointless AAAA
	 * query and wait out its timeout too. */
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;

	ret = zsock_getaddrinfo(WSS_HOST, NULL, &hints, &res);
	if (ret != 0 || res == NULL) {
		/* Not an error worth LOG_ERR: the expected cause is simply
		 * that the i.MX8MP is not up yet, and the caller retries. */
		LOG_WRN("Cannot resolve \"%s\" (%d, %s)", WSS_HOST, ret,
			zsock_gai_strerror(ret));
		return -1;
	}

	/* Take the first A record. CONFIG_DNS_RESOLVER_AI_MAX_ENTRIES allows
	 * more than one, but the Verdin has a single interface on this
	 * network, so a second entry would be the same host anyway. */
	*out = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
	zsock_freeaddrinfo(res);

	if (zsock_inet_ntop(AF_INET, out, out_str, out_str_len) == NULL) {
		LOG_ERR("inet_ntop() failed on resolved address (%d)", errno);
		return -1;
	}

	LOG_INF("Resolved %s -> %s", WSS_HOST, out_str);
	return 0;
}

/* Opens the HTTPS (TLS) socket and connects ONCE, then does the WebSocket
 * upgrade. The handshake happens here only — https_conn_send_frame()
 * reuses this same connected socket and never re-handshakes, except by
 * calling this again after a send failure closes it.
 *
 * Takes an ALREADY-RESOLVED address so that name resolution and connection
 * remain two separate, separately-retryable steps (see https_conn_open()).
 * addr_str is the dotted-quad form of addr, used for SNI and logging. */
static int open_connection(const struct in_addr *addr, const char *addr_str)
{
	struct sockaddr_in dest_addr;
	sec_tag_t sec_tag_list[] = { TLS_SEC_TAG };
	int verify = TLS_PEER_VERIFY_NONE;
	/* The board does not verify the server's certificate — it will
	 * complete a TLS handshake with any server. See the security note
	 * in https_conn.h. */

	https_sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TLS_1_2);
	if (https_sock < 0) {
		LOG_ERR("socket() failed (%d)", errno);
		return -1;
	}

	int nodelay = 1;
	if (zsock_setsockopt(https_sock, IPPROTO_TCP, TCP_NODELAY,
		       &nodelay, sizeof(nodelay)) < 0) {
		LOG_WRN("TCP_NODELAY failed (%d) — continuing anyway", errno);
	}

	if (zsock_setsockopt(https_sock, SOL_TLS, TLS_SEC_TAG_LIST,
		       sec_tag_list, sizeof(sec_tag_list)) < 0) {
		LOG_ERR("TLS_SEC_TAG_LIST failed (%d)", errno);
		goto err;
	}

	/* Pin AES-128-GCM as the bulk cipher (cert is EC/SECP256R1, so ECDSA
	 * is already fixed) — fewer AES rounds per block than the AES-256
	 * mbedTLS would otherwise negotiate. */
	static const int cipher_list[] = {
		MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
	};
	if (zsock_setsockopt(https_sock, SOL_TLS, TLS_CIPHERSUITE_LIST,
		       cipher_list, sizeof(cipher_list)) < 0) {
		LOG_WRN("TLS_CIPHERSUITE_LIST failed (%d) — continuing with default "
			"cipher negotiation", errno);
	}

	if (zsock_setsockopt(https_sock, SOL_TLS, TLS_PEER_VERIFY,
		       &verify, sizeof(verify)) < 0) {
		LOG_ERR("TLS_PEER_VERIFY failed (%d)", errno);
		goto err;
	}

	/* SNI. Which string goes here is governed by WSS_TLS_SNI_USE_HOSTNAME
	 * at the top of this file - by default the resolved IP literal, which
	 * is what the server's certificate has an IP-type SAN for, and what
	 * this code sent before WSS_HOST became a name. */
#if WSS_TLS_SNI_USE_HOSTNAME
	const char *sni = WSS_HOST;
#else
	const char *sni = addr_str;
#endif
	/* optlen is ignored by Zephyr's handler - tls_opt_hostname_set()
	 * (sockets_tls.c) does ARG_UNUSED(optlen) and passes optval straight
	 * to mbedtls_ssl_set_hostname(), which expects a NUL-terminated
	 * string. strlen()+1 is passed anyway to match what the old
	 * sizeof(PC_IP) call site conveyed. */
	if (zsock_setsockopt(https_sock, SOL_TLS, TLS_HOSTNAME,
		       sni, strlen(sni) + 1) < 0) {
		LOG_ERR("TLS_HOSTNAME failed (%d)", errno);
		goto err;
	}

	memset(&dest_addr, 0, sizeof(dest_addr));
	dest_addr.sin_family = AF_INET;
	dest_addr.sin_port = htons(WSS_PORT);
	dest_addr.sin_addr = *addr;

	LOG_INF("TLS connecting to %s [%s]:%d ...", WSS_HOST, addr_str, WSS_PORT);
	if (zsock_connect(https_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) < 0) {
		LOG_ERR("TLS connect() failed (%d)", errno);
		goto err;
	}
	LOG_INF("TLS connected + authenticated (persistent)");

	struct timeval timeout = {
		.tv_sec = 2,
		.tv_usec = 0,
	};
	if (zsock_setsockopt(https_sock, SOL_SOCKET, SO_RCVTIMEO,
		       &timeout, sizeof(timeout)) < 0) {
		LOG_WRN("SO_RCVTIMEO failed (%d) — continuing anyway", errno);
	}

	/* WebSocket upgrade happens here, once, right after the TLS session
	 * is established - everything after this point on this socket is
	 * WebSocket frames (see src/ws.c), not raw HTTP. SO_RCVTIMEO is
	 * already set above, so this recv() can't hang forever if the
	 * server never responds. */
	if (ws_handshake(https_sock) != 0) {
		LOG_ERR("WebSocket handshake failed");
		goto err;
	}

	return 0;

err:
	zsock_close(https_sock);
	https_sock = -1;
	return -1;
}

/* One full attempt: resolve WSS_HOST, then (only if that worked) open the
 * TLS + WebSocket connection to the address it gave. Returns 0 once the
 * WebSocket is up. Used by both the initial open below and the sender
 * thread's reconnect path, so a reconnect always re-resolves rather than
 * reusing a cached address - that is what lets the board follow the
 * i.MX8MP to a new DHCP lease without a reboot. */
static int resolve_and_connect(void)
{
	struct in_addr addr;
	char addr_str[INET_ADDRSTRLEN];

	if (resolve_wss_host(&addr, addr_str, sizeof(addr_str)) != 0) {
		return -1;
	}

	return open_connection(&addr, addr_str);
}

/* Blocks until the connection is up, retrying every RESOLVE_RETRY_MS.
 *
 * Retries rather than halting because failure here is routine and
 * temporary - the i.MX8MP may simply not have finished booting Avahi
 * yet. Called from main() before the sampler/sender threads start, so
 * blocking only delays startup; nothing is sampling or sending yet. */
int https_conn_open(void)
{
	while (1) {
		/* Timed from the START of the attempt so that the attempt's
		 * own cost (a failed mDNS query can burn up to
		 * CONFIG_NET_SOCKETS_DNS_TIMEOUT = 2000 ms, a failed TCP/TLS
		 * connect more) comes out of the 5 s period instead of being
		 * added to it. If an attempt overruns the period the sleep
		 * below is skipped and the next one starts immediately. */
		int64_t attempt_start = k_uptime_get();
		int64_t remaining;

		if (resolve_and_connect() == 0) {
			return 0;
		}

		remaining = RESOLVE_RETRY_MS - (k_uptime_get() - attempt_start);
		if (remaining > 0) {
			LOG_INF("Retrying \"%s\" in %lld ms", WSS_HOST,
				(long long)remaining);
			k_sleep(K_MSEC(remaining));
		}
	}
}

/* Takes a pre-built frame (built by the sampler thread) and sends it as a
 * WebSocket text frame over the persistent WSS connection. FIRE-AND-FORGET
 * - does NOT wait for a reply after sending, because the target server is
 * a third-party server that doesn't ack every message; waiting for a
 * reply that never comes causes a connect->send->2s-timeout->reconnect
 * loop that never gets past the first frame. If you switch to a server
 * that DOES ack every frame and want RTT measurement again, add a
 * ws_recv_frame() call back in here.
 *
 * t_start/t_end bracket only ws_send_frame() itself - this is send
 * duration (how long the blocking send call took), NOT round-trip time,
 * since there's no reply to wait for. Still useful for spotting
 * slow/stalled sends even without a full RTT number.
 *
 * This function is called ONLY by the sender thread - it blocks on
 * network I/O, which is fine because the sender thread has nothing else
 * to do and its blocking doesn't affect the sampler's timing. */
void https_conn_send_frame(const uint8_t *frame, size_t frame_len)
{
	int64_t t_start;
	int64_t t_end;
	int retried = 0;

	if (https_sock < 0) {
		/* Single attempt only - deliberately NOT the blocking retry
		 * loop of https_conn_open(). The sampler builds a fresh frame
		 * every SEND_PERIOD_MS (1 s) regardless, so returning here
		 * gives a natural ~1 s reconnect cadence and keeps this
		 * thread responsive, instead of parking it in a 5 s sleep. */
		if (resolve_and_connect() != 0) {
			return;
		}
	}

retry:
	t_start = k_uptime_get();

	/* LED turns ON right before the send and OFF right after it
	 * completes, so its on-time is a real, physical representation of
	 * how long ws_send_frame() actually took - a genuine flash, not a
	 * state that stays lit until the NEXT send. */
	led_on();

	if (ws_send_frame(https_sock, frame, frame_len) != 0) {
		led_off();
		zsock_close(https_sock);
		https_sock = -1;
		if (retried) { return; }
		retried = 1;
		/* Re-resolves as well as reconnects, so if the send failed
		 * because the i.MX8MP came back on a different IP, this
		 * picks the new one up. */
		if (resolve_and_connect() != 0) { return; }
		goto retry;
	}

	led_off();

	t_end = k_uptime_get();

	int64_t send_time = t_end - t_start;

	/* COMMENTED OUT - redundant, and it was costing half the console
	 * bandwidth for nothing.
	 *
	 * This was a short heartbeat printed immediately after every
	 * successful send. Every value in it - the sequence number and the
	 * send duration - already appears in the detailed per-frame line
	 * further down ("seq=... send=... ms"), so it duplicated roughly
	 * 70 bytes per frame against a 115200 console.
	 *
	 * Re-enable it only if you need to see that a send completed at the
	 * moment it completes, rather than after the epoch/X/Y line is
	 * formatted - the two are emitted a few milliseconds apart.
	 */
	/* LOG_INF("Frame sent (seq=%u, %lld ms)", frame_get_seq() - 1, (long long)send_time); */

	/* Publish this frame's send duration for the NEXT frame (built by
	 * the sampler thread) to include as transmission_time_ms. See
	 * src/frame.h for why this is necessarily one frame behind. */
	frame_note_send_duration_ms(send_time);

	/* Logs the epoch time on every frame - at SEND_PERIOD_MS's current
	 * 1Hz rate, one line per frame is a readable once-per-second
	 * heartbeat.
	 *
	 * Both forms of the SAME timestamp are printed, from one single read
	 * of CLOCK_REALTIME:
	 *   - "epoch=" as numeric seconds.millis, matching the units of the
	 *     JSON "timestamp" field
	 *   - "utc=" as the human-readable string
	 *
	 * Both come from the NTP-corrected CLOCK_REALTIME, the same clock the
	 * frame uses. Do NOT source "epoch=" from epoch_get_last_ntp(): that
	 * raw last-reply value stands still for a whole resync window while
	 * "utc=" beside it advances, putting two contradictory-looking fields
	 * on one line.
	 *
	 * Note this read happens AFTER the send, whereas the frame's
	 * "timestamp" was read before it, so the log value runs ahead of the
	 * frame's by roughly send_time milliseconds. Same clock, different
	 * sample instant - not a discrepancy. */
	{
		char ts_str[48];
		uint64_t now_ms;
		uint16_t x_raw;
		uint16_t y_raw;

		now_ms = epoch_now_ms();
		epoch_format(now_ms, ts_str, sizeof(ts_str));

		/* frame_get_last_raw(), NOT joystick_get_raw(): the latter
		 * re-reads the LIVE ADC rather than reporting the frame that
		 * was just sent. That looks harmless in a normal build, but in
		 * a CONFIG_DEBUG_TFLITE replay build the two are unrelated
		 * numbers and the log would claim the replay is broken when the
		 * payload is in fact correct. */
		frame_get_last_raw(&x_raw, &y_raw);

#ifdef CONFIG_DEBUG_TFLITE
		/* Replay builds also print the recorded row the frame came
		 * from, so a line here can be matched straight against
		 * controller_data_log.csv, and the live ADC alongside it - the
		 * ADC is still sampled every cycle, it is just not what gets
		 * sent, and keeping it visible means a replay build can still
		 * be used to watch the joystick wiring problem. */
		{
			uint16_t live_x;
			uint16_t live_y;

			joystick_get_raw(&live_x, &live_y);
			LOG_INF("seq=%u epoch=%llu.%03u utc=[%s] send=%lld ms "
				"X=%u Y=%u (replay row %u; live ADC %u/%u)",
				frame_get_seq() - 1,
				(unsigned long long)(now_ms / 1000ULL),
				(unsigned int)(now_ms % 1000ULL),
				ts_str, (long long)send_time,
				x_raw, y_raw,
				frame_get_last_replay_row(), live_x, live_y);
		}
#else
		LOG_INF("seq=%u epoch=%llu.%03u utc=[%s] send=%lld ms X=%u Y=%u",
			frame_get_seq() - 1,
			(unsigned long long)(now_ms / 1000ULL),
			(unsigned int)(now_ms % 1000ULL),
			ts_str, (long long)send_time,
			x_raw, y_raw);
#endif
	}
}
