#ifndef EPOCH_H_
#define EPOCH_H_

#include <stddef.h>
#include <stdint.h>

/* NTP-backed wall-clock module.
 *
 * Owns CLOCK_REALTIME's NTP synchronization: a one-shot sync callable at
 * boot, a background thread that keeps re-syncing periodically, and
 * accessors for the current time (cheap, no network) and the raw
 * timestamp from the last successful sync.
 *
 * Thread-safety: epoch_sync_now() and epoch_get_last_ntp() may be called
 * from any thread - the raw last-sync values are protected internally.
 * epoch_now_ms()/epoch_format() only read CLOCK_REALTIME via the kernel's
 * own POSIX clock calls, which are already safe to call from any thread.
 */

/* One-shot NTP sync against server_addr. Returns 0 on success, in which
 * case CLOCK_REALTIME and the raw last-sync values (see
 * epoch_get_last_ntp()) are both updated. On failure, both are left
 * unchanged - a single failed sync does not wipe out prior good data. */
int epoch_sync_now(const char *server_addr);

/* Starts a background thread that calls epoch_sync_now(server_addr) every
 * interval_s seconds. Call once, typically right after an initial
 * epoch_sync_now() at boot. priority is a Zephyr thread priority (passed
 * straight to k_thread_create) - the caller chooses it so it can be
 * coordinated against other application threads' priorities. */
void epoch_start_resync(const char *server_addr, uint32_t interval_s, int priority);

/* Current wall-clock time as unix epoch milliseconds. Cheap (no network
 * call) - safe to call frequently, from any thread. */
uint64_t epoch_now_ms(void);

/* Formats epoch_ms as "YYYY-MM-DD HH:MM:SS.mmm" (UTC) into buf. buf_len
 * must be >= 24. If NTP has never synced, CLOCK_REALTIME is still at its
 * epoch-0 default, so this prints 1970-01-01 ... */
void epoch_format(uint64_t epoch_ms, char *buf, size_t buf_len);

/* Raw timestamp (seconds + Q32 fraction, fraction / 2^32 = fractional
 * seconds) from the most recent successful NTP sync, exactly as the
 * server sent it - NOT derived from CLOCK_REALTIME/epoch_now_ms(). Stays
 * constant between syncs, then jumps right after each one. Both *seconds
 * and *fraction read 0 if NTP has never synced. */
void epoch_get_last_ntp(uint64_t *seconds, uint32_t *fraction);

#endif /* EPOCH_H_ */
