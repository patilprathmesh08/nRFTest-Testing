#include "ws.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(ws, CONFIG_LOG_DEFAULT_LEVEL);

#include <zephyr/net/socket.h>
#include <zephyr/random/random.h>
#include <errno.h>
#include <string.h>

#include "frame.h"

static int send_all(int sock, const char *buf, size_t len)
{
	size_t sent = 0;

	while (sent < len) {
		int ret = zsock_send(sock, buf + sent, len - sent, 0);

		if (ret < 0) {
			if (errno == EAGAIN) {
				continue;
			}
			LOG_ERR("send() failed (%d)", errno);
			return -1;
		}
		if (ret == 0) {
			LOG_ERR("send() returned 0");
			return -1;
		}
		sent += (size_t)ret;
	}
	return 0;
}

/* Standard base64 encode, used only for the 16-byte Sec-WebSocket-Key.
 * out must have room for 4*ceil(in_len/3)+1 bytes. */
static void ws_base64_encode(const uint8_t *in, size_t in_len, char *out)
{
	static const char b64_table[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t i, j = 0;

	for (i = 0; i < in_len; i += 3) {
		uint32_t v = (uint32_t)in[i] << 16;

		if (i + 1 < in_len) {
			v |= (uint32_t)in[i + 1] << 8;
		}
		if (i + 2 < in_len) {
			v |= (uint32_t)in[i + 2];
		}

		out[j++] = b64_table[(v >> 18) & 0x3F];
		out[j++] = b64_table[(v >> 12) & 0x3F];
		out[j++] = (i + 1 < in_len) ? b64_table[(v >> 6) & 0x3F] : '=';
		out[j++] = (i + 2 < in_len) ? b64_table[v & 0x3F] : '=';
	}
	out[j] = '\0';
}

#define WS_KEY_RAW_LEN 16
#define WS_KEY_B64_LEN 25  /* 24 base64 chars + NUL */

int ws_handshake(int sock)
{
	uint8_t key_raw[WS_KEY_RAW_LEN];
	char key_b64[WS_KEY_B64_LEN];
	char req[224];
	char resp[512];
	int req_len;
	int ret;

	sys_rand_get(key_raw, sizeof(key_raw));
	ws_base64_encode(key_raw, sizeof(key_raw), key_b64);

	req_len = snprintf(req, sizeof(req),
			    "GET /ws HTTP/1.1\r\n"
			    "Host: h\r\n"
			    "Upgrade: websocket\r\n"
			    "Connection: Upgrade\r\n"
			    "Sec-WebSocket-Key: %s\r\n"
			    "Sec-WebSocket-Version: 13\r\n"
			    "\r\n",
			    key_b64);

	if (send_all(sock, req, (size_t)req_len) != 0) {
		LOG_ERR("WS handshake request send failed");
		return -1;
	}

	ret = zsock_recv(sock, resp, sizeof(resp) - 1, 0);
	if (ret <= 0) {
		LOG_ERR("WS handshake response recv failed (%d)", errno);
		return -1;
	}
	resp[ret] = '\0';

	if (strncmp(resp, "HTTP/1.1 101", 12) != 0) {
		LOG_ERR("WS handshake rejected by server: %.60s", resp);
		return -1;
	}

	LOG_INF("WS handshake OK (101 Switching Protocols)");
	return 0;
}

int ws_send_frame(int sock, const uint8_t *payload, size_t len)
{
	/* header: 1 (FIN+opcode) + 1-3 (mask bit + length) + 4 (mask key) */
	uint8_t buf[8 + MAX_FRAME_SIZE];
	uint8_t mask[4];
	size_t hlen = 0;
	size_t i;

	buf[hlen++] = 0x81; /* FIN=1, opcode=1 (text) */

	if (len <= 125) {
		buf[hlen++] = 0x80 | (uint8_t)len;
	} else {
		/* Not expected given MAX_FRAME_SIZE, but handled correctly
		 * regardless (16-bit extended length, big-endian). */
		buf[hlen++] = 0x80 | 126;
		buf[hlen++] = (uint8_t)((len >> 8) & 0xFF);
		buf[hlen++] = (uint8_t)(len & 0xFF);
	}

	sys_rand_get(mask, sizeof(mask));
	memcpy(&buf[hlen], mask, sizeof(mask));
	hlen += sizeof(mask);

	for (i = 0; i < len; i++) {
		buf[hlen + i] = payload[i] ^ mask[i % 4];
	}

	return send_all(sock, (const char *)buf, hlen + len);
}

int ws_recv_frame(int sock, uint8_t *out, size_t out_cap, size_t *out_len)
{
	uint8_t hdr[2];
	uint8_t opcode;
	int masked;
	uint64_t plen;
	uint8_t mask[4] = {0};
	size_t received;
	int ret;

	ret = zsock_recv(sock, hdr, sizeof(hdr), 0);
	if (ret != (int)sizeof(hdr)) {
		return -1;
	}

	opcode = hdr[0] & 0x0F;
	masked = hdr[1] & 0x80;
	plen = hdr[1] & 0x7F;

	if (plen == 126) {
		uint8_t ext[2];

		if (zsock_recv(sock, ext, sizeof(ext), 0) != (int)sizeof(ext)) {
			return -1;
		}
		plen = ((uint64_t)ext[0] << 8) | ext[1];
	} else if (plen == 127) {
		uint8_t ext[8];
		int k;

		if (zsock_recv(sock, ext, sizeof(ext), 0) != (int)sizeof(ext)) {
			return -1;
		}
		plen = 0;
		for (k = 0; k < 8; k++) {
			plen = (plen << 8) | ext[k];
		}
	}

	if (masked) {
		if (zsock_recv(sock, mask, sizeof(mask), 0) != (int)sizeof(mask)) {
			return -1;
		}
	}

	if (plen > out_cap) {
		LOG_ERR("WS frame too large for buffer (%llu > %zu)",
			(unsigned long long)plen, out_cap);
		return -1;
	}

	received = 0;
	while (received < plen) {
		ret = zsock_recv(sock, out + received, (size_t)plen - received, 0);
		if (ret <= 0) {
			return -1;
		}
		received += (size_t)ret;
	}

	if (masked) {
		size_t i;

		for (i = 0; i < plen; i++) {
			out[i] ^= mask[i % 4];
		}
	}

	*out_len = (size_t)plen;
	return (int)opcode;
}
