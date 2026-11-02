#ifndef HTTPS_CONN_H_
#define HTTPS_CONN_H_

#include <stddef.h>
#include <stdint.h>

/* Persistent mTLS + WebSocket connection to the telemetry server.
 *
 * No CA certificate is provisioned — the board does not verify the
 * server's identity (TLS_PEER_VERIFY is set to TLS_PEER_VERIFY_NONE).
 * SECURITY NOTE: this means the board will complete a TLS handshake with
 * ANY server, not just the trusted one — it can no longer detect
 * impersonation. Client-cert auth (below) still proves the board's own
 * identity to the server, but the reverse direction is unverified.
 */

/* Provisions the mTLS client certificate/private key (see src/https_conn.c
 * for where the arrays come from). Call once, before https_conn_open().
 * Idempotent - a retried attempt after a partial failure is not an error.
 * Returns 0 on success. */
int https_conn_provision_cert(void);

/* Resolves the server's mDNS name (WSS_HOST in src/https_conn.c,
 * "verdin-imx8mp.local"), then opens the persistent TLS socket and does
 * the WebSocket upgrade handshake.
 *
 * BLOCKS UNTIL IT SUCCEEDS. If the name does not resolve - the usual
 * reason being that the i.MX8MP has not brought Avahi up yet - or the
 * connection cannot be established, it retries the whole
 * resolve-then-connect cycle every 5 s and does not return until the
 * WebSocket is up. It therefore never returns non-zero; the int return is
 * kept so the call site does not have to change and so a future bounded
 * variant can report failure.
 *
 * Call once, before the sampler/sender threads are started - blocking
 * here delays startup only.
 *
 * https_conn_send_frame() below reuses this same connection and reopens
 * it internally (re-resolving the name each time) if a send fails. */
int https_conn_open(void);

/* Sends one pre-built frame as a WebSocket text frame over the
 * persistent connection, reopening it internally (once) if the send
 * fails. Fire-and-forget - does not wait for a reply. Blocks on network
 * I/O; call only from a dedicated sender thread, never from the sampler
 * thread. */
void https_conn_send_frame(const uint8_t *frame, size_t frame_len);

#endif /* HTTPS_CONN_H_ */
