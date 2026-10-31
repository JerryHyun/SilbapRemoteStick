#pragma once
#include <stdbool.h>
#include <stdint.h>
/* Pure timing policy of the refresh task (no ESP-IDF dependencies, unit-tested on the host).
 * All times are milliseconds on one monotonic clock; last_read == 0 means "no SCSI read yet". */
#define RP_DEBOUNCE_MS          1500u   /* a request must be this old; newer requests restart it */
#define RP_IDLE_MS              3000u   /* no READ10 for this long before an unforced rebuild */
/* A forced request also prefers a quiet bus: a rebuild takes the controller off the bus for 500 ms, so starting one
 * in the middle of a READ10 makes the host lose the disk instead of getting a retryable NOT READY. The wait is
 * bounded so a machine that reads continuously cannot starve a button press or a settings change. */
#define RP_FORCED_MAX_WAIT_MS  10000u   /* forced rebuild runs unconditionally this long after the request */
/* How long the USB device must stay away for a host to treat the gap as a real unplug rather than a glitch.
 * robbellie/embroidery-stick measured this on hardware (main/app_main.c, do_usb_refresh): 50 ms was debounced away
 * by some host stacks, which then kept showing a stale directory listing until the user replugged by hand, and
 * 500 ms was enough. Serializable here so the clamp can be tested without a USB host. */
#define RP_REPLUG_HOLD_MS       500u
#define RP_REPLUG_MIN_MS        500u   /* below this the gap is not reliably seen as a disconnect */
#define RP_REPLUG_MAX_MS       2000u   /* above this the machine is away long enough to notice */
/* Clamp a requested hold into [RP_REPLUG_MIN_MS, RP_REPLUG_MAX_MS]. */
uint32_t rp_replug_hold_ms(uint32_t requested);
#define RP_BOOT_VERIFY_MS       5000u   /* earliest boot verify */
#define RP_BOOT_VERIFY_CAP_MS  15000u   /* latest boot verify, even while the PC keeps reading */
typedef enum { RP_WAIT = 0, RP_RUN = 1 } rp_decision_t;
/* Pending change request: RUN once debounced and (forced ? idle || cap : auto_refresh && idle). auto_refresh off: only forced runs. */
rp_decision_t rp_change(uint32_t now, uint32_t last_request, uint32_t last_read, bool forced, bool auto_refresh);
/* An automatic replug is never closer than this to the previous one, the same floor the re-mount keeps
 * (REMOUNT_MIN_MS): the machine has just re-enumerated and is reading its directory again, and a write of its own
 * during that read must not take the disk away twice inside one quiet window. */
#define RP_REPLUG_FLOOR_MS      3000u
/* Should the controller be taken off the bus and put back (so the host re-reads the directory) now?
 * last_request: the stamp of what asked for the cycle, 0 = nothing pending (never a cycle on an unchanged disk).
 *   Forced (button, POST /api/refresh): rp_change's forced rule, so the debounce still applies and the bus is
 *   preferred idle, but RP_FORCED_MAX_WAIT_MS after the request runs regardless of reads. Ignores the floor and
 *   auto_refresh: the user asked.
 *   Automatic (web mutation, machine write): only with auto_refresh, and only after the debounce and RP_IDLE_MS
 *   without a READ10, and never within RP_REPLUG_FLOOR_MS of the previous replug.
 * last_replug: 0 = never. */
rp_decision_t rp_replug(uint32_t now, uint32_t last_request, uint32_t last_read, bool forced, bool auto_refresh,
                        uint32_t last_replug);
#define RP_BACKOFF_MIN_MS      30000u   /* after the first failed rebuild */
#define RP_BACKOFF_MAX_MS     600000u   /* doubling stops at 10 min */
/* Wait before the next rebuild after fails consecutive failed ones: 0, then 30 s doubling up to 10 min. */
uint32_t rp_backoff_ms(uint32_t fails);
/* Boot verify: wait until 5 s after boot; then run when idle, or unconditionally from 15 s after boot. */
rp_decision_t rp_boot_verify(uint32_t now, uint32_t boot, uint32_t last_read);
