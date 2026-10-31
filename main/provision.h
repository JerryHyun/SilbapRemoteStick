#pragma once
/* Setup portal (captive DNS + the Wi-Fi setup page). netif calls provision_start() when it brings the setup AP up
 * and provision_stop() when it takes it down, so every route registered here exists only during setup.
 *
 * While the portal is up:
 *   GET /setup, /generate_204, /hotspot-detect.html, /ncsi.txt, /connecttest.txt   (all PUBLIC)
 *   GET /api/scan, POST /api/setup                                                  (all PUBLIC)
 * and UDP :53 answers every A query with the setup AP address (see dns_reply.h for the packet shape).
 *
 * Both calls are idempotent and may be repeated (start, stop, start, ...); the second provision_start() gets a fresh
 * DNS task, socket and route set. Captive DNS is adapted from robbellie/embroidery-stick app_provision.c (MIT); the
 * reply builder now lives in main/dns_reply.c. */

void provision_start(void);
void provision_stop(void);

/* Called at the end of web_start(): the setup AP usually comes up before the HTTP server, so the portal registers
 * its routes here once the server exists. */
void provision_web_ready(void);

/* Called from the net task on every tick: retries a route change (add or remove) that could not be queued or applied
 * within its bounded wait, so a failed unregister is never silently final. Cheap no-op when nothing is pending. */
void provision_tick(void);

