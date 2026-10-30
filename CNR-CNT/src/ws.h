#ifndef WS_H_
#define WS_H_

#include <stddef.h>
#include <stdint.h>

/* ---------------- WebSocket (RFC 6455) over an existing TLS socket ------
 * One HTTP Upgrade handshake at connect time, then lightweight
 * binary-framed messages for every send (2-8 bytes of framing overhead
 * instead of ~150 bytes of HTTP headers per frame with plain HTTP POST).
 *
 * TLS/mTLS setup is the caller's concern (src/https_conn.c) - this module
 * only handles the WebSocket-layer framing on whatever already-connected
 * socket it's given.
 *
 * SCOPE / LIMITATIONS (documented on purpose, not hidden):
 *   - No message fragmentation support (FIN is always 1) — fine here,
 *     every frame we send is well under any reasonable single-frame size.
 *   - No ping/pong keepalive handling — this connection is already kept
 *     alive by a real data frame every send cycle, so an idle-timeout
 *     keepalive mechanism isn't needed the way it would be for a
 *     mostly-idle client.
 *   - The handshake response's Sec-WebSocket-Accept is NOT cryptographically
 *     verified against the key we sent (that needs SHA-1 + base64 decode
 *     on-device). We only check the response starts with "HTTP/1.1 101".
 *     This is a deliberate simplification: the channel is already
 *     authenticated by mTLS underneath (client cert), so a peer that
 *     could spoof this response would already have had to pass that
 *     check first — the risk this verification step would catch is
 *     already covered one layer down.
 */

/* Sends the HTTP Upgrade request and reads/checks the "101" response.
 * Called once per connection, right after the TLS handshake completes,
 * before any data frames are sent. Returns 0 on success. */
int ws_handshake(int sock);

/* Sends one WebSocket text frame. Client-to-server frames MUST be masked
 * per RFC 6455 (server frames must NOT be) - the mask key is fresh random
 * bytes every frame. Returns 0 on success. */
int ws_send_frame(int sock, const uint8_t *payload, size_t len);

/* Reads one WebSocket frame from the server. Server-to-client frames are
 * NOT masked per spec, but this still handles the masked case correctly
 * (harmless if a server ever does mask). Only single-frame (FIN=1)
 * messages are supported - see the scope note above. Returns the opcode
 * on success (>=0), or -1 on error. *out_len receives the payload length. */
int ws_recv_frame(int sock, uint8_t *out, size_t out_cap, size_t *out_len);

#endif /* WS_H_ */
