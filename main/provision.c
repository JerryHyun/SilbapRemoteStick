/* Setup portal: captive DNS on UDP 53 plus the setup-only HTTP routes (/setup, the captive-probe redirects,
 * /api/scan, /api/setup). Started/stopped by netif with the setup AP, so nothing here is reachable on a normal
 * running device.
 *
 * Captive DNS adapted from robbellie/embroidery-stick app_provision.c (MIT License; see THIRD_PARTY_NOTICES.md);
 * the packet builder itself lives in main/dns_reply.c and is host-tested. */
#include "provision.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "api_list_core.h"
#include "dns_reply.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "netif.h"
#include "ssid_list.h"
#include "web.h"
#include "webcore.h"

static const char *TAG = "provision";

/* 192.168.4.1 as the four RDATA octets C0 A8 04 01, big-endian (see dns_reply.h: NOT the inet_addr form). */
#define SETUP_IPV4_BE   0xC0A80401u
#define SETUP_IP_TEXT   "192.168.4.1"
#define SETUP_PAGE      "http://" SETUP_IP_TEXT "/setup"

#define DNS_PORT        53
#define DNS_STACK       4096            /* lwIP sockets + the reply builder */
#define DNS_PRIO        3               /* below the net task (4) and httpd, above idle */
#define DNS_POLL_MS     200             /* select() timeout: lets the task notice the stop flag */
#define DNS_PKT_MAX     512
#define DNS_STOP_WAIT_MS 1500           /* ~7 poll intervals; the task never blocks longer than this */

/* Scan: return at most SCAN_LIST_MAX networks. Worst case per entry, from the JSON writer's escaping: an SSID of 32
 * control bytes becomes 32 x "\u00XX" = 192 bytes (the other extreme, 32 invalid bytes, gives 32 U+FFFD = 96 bytes
 * passed through), plus the two quotes, "rssi":-128 (11), "ch":255 (8), "auth":"enterprise" (19), the four field
 * names (31) and the braces/comma (3): under 260 bytes, so SCAN_BUF below is 12 x 260 plus the two brackets. */
#define SCAN_LIST_MAX   12
#define SCAN_GET_MAX    32              /* records fetched from the driver at most */
#define SCAN_ENT_BUDGET 260             /* worst-case JSON bytes for one entry (see above) */
#define SCAN_BUF        (SCAN_LIST_MAX * SCAN_ENT_BUDGET + 2)

/* The scan routine keeps one list slot per driver record (SCAN_GET_MAX), dedups there, then truncates to
 * SCAN_LIST_MAX: duplicates must not consume a capped slot. The dedup input is capped at the same 32 records the
 * driver is asked for, so it cannot grow without bound; only the capped output is written to the JSON buffer. */
_Static_assert(SCAN_GET_MAX >= SCAN_LIST_MAX, "the merged list must be able to hold the capped output");

/* main/ssid_list.c mirrors the wifi_auth_mode_t numbers so it stays ESP-IDF-free; these checks fail the build if a
 * future IDF reorders the enum instead of mislabelling networks at runtime. */
_Static_assert((unsigned)SSID_WIFI_AUTH_OPEN == (unsigned)WIFI_AUTH_OPEN, "auth enum drift");
_Static_assert((unsigned)SSID_WIFI_AUTH_WEP == (unsigned)WIFI_AUTH_WEP, "auth enum drift");
_Static_assert((unsigned)SSID_WIFI_AUTH_WPA_PSK == (unsigned)WIFI_AUTH_WPA_PSK, "auth enum drift");
_Static_assert((unsigned)SSID_WIFI_AUTH_WPA2_PSK == (unsigned)WIFI_AUTH_WPA2_PSK, "auth enum drift");
_Static_assert((unsigned)SSID_WIFI_AUTH_WPA_WPA2_PSK == (unsigned)WIFI_AUTH_WPA_WPA2_PSK, "auth enum drift");
_Static_assert((unsigned)SSID_WIFI_AUTH_ENTERPRISE == (unsigned)WIFI_AUTH_ENTERPRISE, "auth enum drift");
_Static_assert((unsigned)SSID_WIFI_AUTH_WPA3_PSK == (unsigned)WIFI_AUTH_WPA3_PSK, "auth enum drift");
_Static_assert((unsigned)SSID_WIFI_AUTH_WPA2_WPA3_PSK == (unsigned)WIFI_AUTH_WPA2_WPA3_PSK, "auth enum drift");
_Static_assert((unsigned)SSID_WIFI_AUTH_WAPI_PSK == (unsigned)WIFI_AUTH_WAPI_PSK, "auth enum drift");
_Static_assert((unsigned)SSID_WIFI_AUTH_OWE == (unsigned)WIFI_AUTH_OWE, "auth enum drift");
_Static_assert((unsigned)SSID_WIFI_AUTH_WPA3_ENT_192 == (unsigned)WIFI_AUTH_WPA3_ENT_192, "auth enum drift");
_Static_assert((unsigned)SSID_WIFI_AUTH_WPA3_EXT_PSK == (unsigned)WIFI_AUTH_WPA3_EXT_PSK, "auth enum drift");
_Static_assert((unsigned)SSID_WIFI_AUTH_WPA3_EXT_PSK_MIXED_MODE == (unsigned)WIFI_AUTH_WPA3_EXT_PSK_MIXED_MODE, "auth enum drift");
_Static_assert((unsigned)SSID_WIFI_AUTH_DPP == (unsigned)WIFI_AUTH_DPP, "auth enum drift");
_Static_assert((unsigned)SSID_WIFI_AUTH_WPA3_ENTERPRISE == (unsigned)WIFI_AUTH_WPA3_ENTERPRISE, "auth enum drift");
_Static_assert((unsigned)SSID_WIFI_AUTH_WPA2_WPA3_ENTERPRISE == (unsigned)WIFI_AUTH_WPA2_WPA3_ENTERPRISE, "auth enum drift");
_Static_assert((unsigned)SSID_WIFI_AUTH_WPA_ENTERPRISE == (unsigned)WIFI_AUTH_WPA_ENTERPRISE, "auth enum drift");

/* ---- setup-only routes ---- */
static esp_err_t setup_page_get(httpd_req_t *req) { return web_send_asset(req, "/assets/setup.html"); }

static esp_err_t captive_redirect(httpd_req_t *req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", SETUP_PAGE);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t scan_get(httpd_req_t *req) {
    static ssid_ent_t list[SCAN_GET_MAX];   /* one slot per driver record: dedup runs before the cap */
    static wifi_ap_record_t recs[SCAN_GET_MAX];
    static char out[SCAN_BUF];

    /* Active scan, 2.4 GHz channels 1..13: sc.channel = 0 means "every channel of the current country", and netif.c
     * sets the country to 01 / schan 1 / nchan 13 (WIFI_COUNTRY_POLICY_MANUAL), so the scan never leaves 2.4 GHz.
     * The httpd task blocks for the whole scan: ~1 s typical, at most ~2 s (13 channels x 150 ms). The plan accepts a
     * blocking scan on a LAN device; the AP stays up meanwhile but is off-channel while scanning (see the report). */
    wifi_scan_config_t sc = { 0 };
    sc.show_hidden = false;             /* hidden networks are typed in by hand on the page */
    sc.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    sc.scan_time.active.min = 60;
    sc.scan_time.active.max = 150;
    esp_err_t e = esp_wifi_scan_start(&sc, true);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(e));
        return web_error(req, 503, "err.io");
    }
    uint16_t want = SCAN_GET_MAX, got = 0;
    e = esp_wifi_scan_get_ap_records(&want, recs);   /* also frees the driver's scan list */
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "scan results: %s", esp_err_to_name(e));
        esp_wifi_clear_ap_list();
        return web_error(req, 503, "err.io");
    }
    got = want > SCAN_GET_MAX ? SCAN_GET_MAX : want;

    size_t n = 0;
    for (uint16_t i = 0; i < got; i++) {
        /* SSID is up to 32 raw bytes, not necessarily UTF-8 or NUL-terminated: ssid[33] has one spare byte, so
         * stop one short of the array size and never read that spare. */
        size_t len = strnlen((const char *)recs[i].ssid, sizeof recs[i].ssid - 1);
        n = ssid_merge(list, n, SCAN_GET_MAX, recs[i].ssid, len, recs[i].rssi, recs[i].primary,
                       ssid_auth_from_code(recs[i].authmode));
    }
    n = ssid_list_finish(list, n, SCAN_LIST_MAX);   /* sort, then truncate: duplicates never took a capped slot */

    wc_json_t w;
    wc_json_init(&w, out, sizeof out);
    wc_json_arr_open(&w, NULL);
    for (size_t i = 0; i < n; i++) {
        wc_json_obj_open(&w);
        wc_json_str(&w, "ssid", list[i].ssid);
        wc_json_num(&w, "rssi", list[i].rssi);
        wc_json_num(&w, "ch", list[i].channel);
        wc_json_str(&w, "auth", ssid_auth_name(list[i].auth));
        wc_json_obj_close(&w);
    }
    wc_json_arr_close(&w);
    if (w.overflow) return web_error(req, 500, "err.internal");
    ESP_LOGI(TAG, "scan: %u networks", (unsigned)n);
    return web_json(req, 200, out);
}

static esp_err_t setup_post(httpd_req_t *req) {
    static char body[1024];              /* same cap as /api/wifi: escaped SSID + password still fits */
    static char ssid[34], pw[66];
    esp_err_t rc;
    if (!web_read_body(req, body, sizeof body, "err.bad_request", &rc)) return rc;
    api_get_t a = api_json_get_str(body, "ssid", ssid, sizeof ssid);
    api_get_t b = api_json_get_str(body, "password", pw, sizeof pw);
    if (b == API_ABSENT) { pw[0] = 0; b = API_OK; }
    size_t sl = a == API_OK ? strlen(ssid) : 0, pl = b == API_OK ? strlen(pw) : 0;
    if (a != API_OK || b != API_OK || sl == 0 || sl > 32 || pl > 64 || (pl > 0 && pl < 8)) {
        memset(pw, 0, sizeof pw);
        memset(body, 0, sizeof body);
        return web_error(req, 400, "err.bad_wifi");
    }
    esp_err_t e = net_apply_credentials(ssid, pw);
    memset(pw, 0, sizeof pw);
    memset(body, 0, sizeof body);
    if (e != ESP_OK) return web_error(req, 500, "err.io");
    ESP_LOGI(TAG, "credentials for \"%s\" saved, reconnecting", ssid);   /* never the password */
    return web_json(req, 200, "{\"ok\":true}");
}

typedef struct { const char *uri; httpd_method_t method; web_handler_t h; } proute_t;
static const proute_t ROUTES[] = {
    { "/setup",               HTTP_GET,  setup_page_get },
    { "/generate_204",        HTTP_GET,  captive_redirect },   /* Android */
    { "/hotspot-detect.html", HTTP_GET,  captive_redirect },   /* Apple */
    { "/ncsi.txt",            HTTP_GET,  captive_redirect },   /* Windows */
    { "/connecttest.txt",     HTTP_GET,  captive_redirect },   /* Windows */
    { "/api/scan",            HTTP_GET,  scan_get },
    { "/api/setup",           HTTP_POST, setup_post },
};

/* Host names the captive probes put in the Host header (the middleware would answer 403 otherwise). Only while the
 * portal is up. String literals: web_allow_host does not copy. Keep the count <= MAX_EXTRA_HOSTS in main/web.c. */
static const char *const PROBE_HOSTS[] = {
    "connectivitycheck.gstatic.com",   /* Android /generate_204 */
    "connectivitycheck.android.com",
    "www.google.com",                  /* older Android builds probe /generate_204 here */
    "clients3.google.com",
    "captive.apple.com",               /* Apple /hotspot-detect.html */
    "www.msftconnecttest.com",         /* Windows /connecttest.txt */
    "www.msftncsi.com",                /* Windows /ncsi.txt */
    "detectportal.firefox.com",
};

static volatile bool s_want;          /* portal should be up (set/cleared by start/stop) */
static bool s_routes_on;              /* routes currently registered: touched only in the httpd task */
static TaskHandle_t s_dns_task;
static SemaphoreHandle_t s_dns_done;
static volatile int s_dns_fd = -1;    /* socket before it is handed to the task */
static SemaphoreHandle_t s_sync_done; /* portal_work has applied the newest requested generation */
static volatile unsigned s_gen;       /* bumped by start and stop: a task runs while its generation matches */
static bool s_dns_orphan;             /* a stop timed out; the task is still on its way out */

/* Route-change bookkeeping. Each queued item carries its own generation, so a give from an older item can never
 * satisfy a newer wait: portal_work stores what it applied, and only the waiter's own generation counts. */
static volatile unsigned s_req_gen;       /* generation of the newest portal_sync request */
static volatile unsigned s_applied_gen;   /* generation the httpd task has finished deciding */
static volatile bool s_applied_want;      /* s_routes_on after that decision (written before s_applied_gen) */
static volatile bool s_sync_retry;        /* a request was not confirmed: the net task must retry it */

/* The job travels in the void *arg itself (httpd_queue_work only passes it through), so two queued items never
 * share a struct and a stale item cannot pick up a newer request. gen is masked to 31 bits and shifted left for the
 * want bit; wrapping is harmless because a waiter compares its own generation for equality and httpd runs items in
 * queue order, so the newest item always writes the applied generation last. */
#define PORTAL_GEN_MASK 0x7FFFFFFFu
static void *portal_job(unsigned gen, bool want) {
    return (void *)(uintptr_t)(((gen & PORTAL_GEN_MASK) << 1) | (want ? 1u : 0u));
}

static bool portal_applied(unsigned gen, bool want) {
    unsigned a = __atomic_load_n(&s_applied_gen, __ATOMIC_ACQUIRE);
    if ((int32_t)(a - gen) < 0) return false;   /* no item from this request (or a newer one) has finished */
    return __atomic_load_n(&s_applied_want, __ATOMIC_ACQUIRE) == want;
}

/* Register or unregister the whole setup route set. Runs in the httpd task (see portal_sync) so that httpd's handler
 * array and this file's route records are never changed while a request is being dispatched. */
static void portal_work(void *arg) {
    uintptr_t v = (uintptr_t)arg;
    unsigned gen = (unsigned)(v >> 1);
    bool on = (v & 1u) != 0;
    if (on == s_routes_on) goto done;
    if (on && !web_started()) goto done;   /* web_start() will call provision_web_ready() */
    for (size_t i = 0; i < sizeof ROUTES / sizeof ROUTES[0]; i++) {
        if (on) web_route(ROUTES[i].uri, ROUTES[i].method, WEB_PUBLIC, ROUTES[i].h);
        else web_unroute(ROUTES[i].uri, ROUTES[i].method);
    }
    if (on) {
        for (size_t i = 0; i < sizeof PROBE_HOSTS / sizeof PROBE_HOSTS[0]; i++) web_allow_host(PROBE_HOSTS[i]);
    } else {
        web_clear_extra_hosts();
    }
    s_routes_on = on;
    ESP_LOGI(TAG, "setup routes %s", on ? "registered" : "removed");
done:
    __atomic_store_n(&s_applied_want, s_routes_on, __ATOMIC_RELAXED);
    /* Never move the applied generation backwards: an item that found nothing to do still confirms its request, but a
     * late older item must not overwrite a newer confirmation. */
    unsigned prev = __atomic_load_n(&s_applied_gen, __ATOMIC_RELAXED);
    if ((int32_t)(gen - prev) > 0) __atomic_store_n(&s_applied_gen, gen, __ATOMIC_RELEASE);
    /* Only the newest request gets the give. An older item that finishes late leaves the semaphore alone, so it can
     * never stand in for work that has not run yet. */
    if (s_sync_done && gen == (__atomic_load_n(&s_req_gen, __ATOMIC_ACQUIRE) & PORTAL_GEN_MASK))
        xSemaphoreGive(s_sync_done);
}

/* Applies the wanted state. The table is changed from the httpd task (so httpd's handler array and this file's route
 * records are never modified under a running dispatch). Removing waits for the change (provision_stop must have
 * unregistered its routes when it returns; the wait is bounded because the httpd task may be busy with a scan);
 * adding does not, since the phone needs seconds to reach the portal anyway. Returns true when the wanted state is
 * confirmed (wait) or at least queued for the httpd task (no wait); false means the caller must retry. */
static bool portal_sync(bool wait) {
    bool want = s_want;
    unsigned gen = __atomic_add_fetch(&s_req_gen, 1, __ATOMIC_SEQ_CST) & PORTAL_GEN_MASK;
    if (!web_started()) {             /* no dispatcher yet: nothing can race with the table change */
        portal_work(portal_job(gen, want));  /* an add without a server is a no-op; web_start re-runs this */
        s_sync_retry = false;         /* nothing is left pending: web_start() calls provision_web_ready() */
        return true;
    }
    if (!s_sync_done) s_sync_done = xSemaphoreCreateBinary();
    if (!s_sync_done) {
        ESP_LOGE(TAG, "sync semaphore failed");
        s_sync_retry = true;
        return false;
    }
    while (xSemaphoreTake(s_sync_done, 0) == pdTRUE) {}
    esp_err_t e = ESP_FAIL;
    for (int i = 0; i < 3 && e != ESP_OK; i++) {          /* the ctrl socket may be briefly full */
        if (i) vTaskDelay(pdMS_TO_TICKS(20));
        e = web_run_in_server(portal_work, portal_job(gen, want));
    }
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "route change could not be queued: %s", esp_err_to_name(e));
        s_sync_retry = true;          /* the net task's next tick queues it again */
        return false;
    }
    if (!wait) {                      /* the item is in httpd's queue: a pending action exists */
        s_sync_retry = false;
        return true;
    }
    TickType_t start = xTaskGetTickCount();
    while (!portal_applied(gen, want)) {
        TickType_t spent = xTaskGetTickCount() - start;
        if (spent >= pdMS_TO_TICKS(3000)) break;
        /* Short poll, not "sleep until the give": a newer request clears this item's give on purpose, and then
         * portal_applied() is the only reliable answer. 50 ms is far below the 3 s bound. */
        xSemaphoreTake(s_sync_done, pdMS_TO_TICKS(50));
    }
    bool ok = portal_applied(gen, want);
    if (!ok) ESP_LOGE(TAG, "route change not applied in 3000 ms (wanted %s); the net task will retry",
                      want ? "on" : "off");
    s_sync_retry = !ok;
    return ok;
}

/* Called by web_start() once the server runs (the setup AP usually comes up before it). Queued, not waited for: boot
 * must not sit in app_main for the HTTP task. The portal is reachable as soon as the httpd task gets to the item. */
void provision_web_ready(void) {
    if (s_want) portal_sync(false);
}

/* Called from the net task every tick. A route change that could not be queued, or was not applied within the
 * bounded wait, is retried here, so after provision_stop() returns the routes are either gone or on their way out.
 * Throttled to one attempt a second so a wedged httpd cannot turn this into a busy loop or a log flood; the flag
 * stays set until a queue attempt succeeds, so the retry is never dropped. */
void provision_tick(void) {
    static TickType_t last;
    if (!s_sync_retry) return;
    TickType_t now = xTaskGetTickCount();
    if (last && now - last < pdMS_TO_TICKS(1000)) return;
    last = now;
    if (portal_sync(false))
        ESP_LOGW(TAG, "route change re-queued, wanted state: %s", s_want ? "on" : "off");
}

/* The setup AP netif owns 192.168.4.1. Bind to it so the socket only answers on the AP link; fall back to any
 * address if the netif cannot be queried (the portal still works, the reply address is fixed). */
static void ap_bind_addr(struct sockaddr_in *sa) {
    memset(sa, 0, sizeof *sa);
    sa->sin_family = AF_INET;
    sa->sin_port = htons(DNS_PORT);
    sa->sin_addr.s_addr = htonl(INADDR_ANY);
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_ip_info_t ip;
    if (ap && esp_netif_get_ip_info(ap, &ip) == ESP_OK && ip.ip.addr != 0) sa->sin_addr.s_addr = ip.ip.addr;
}

static void dns_task(void *arg) {
    unsigned gen = (unsigned)(uintptr_t)arg;
    int fd = s_dns_fd;
    static uint8_t q[DNS_PKT_MAX], r[DNS_PKT_MAX];
    while (s_gen == gen) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        struct timeval tv = { .tv_sec = 0, .tv_usec = DNS_POLL_MS * 1000 };
        int ready = select(fd + 1, &fds, NULL, NULL, &tv);
        if (ready < 0) {
            if (errno == EINTR) continue;
            ESP_LOGW(TAG, "DNS select: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(DNS_POLL_MS));   /* keep the task alive: stop() ends it, not a transient error */
            continue;
        }
        if (ready == 0 || !FD_ISSET(fd, &fds)) continue;
        struct sockaddr_storage from;
        socklen_t fl = sizeof from;
        int n = recvfrom(fd, q, sizeof q, 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) continue;
        size_t rn = dns_build_reply(q, (size_t)n, r, sizeof r, SETUP_IPV4_BE);
        if (rn) sendto(fd, r, rn, 0, (struct sockaddr *)&from, fl);
    }
    close(fd);                        /* the task owns the socket from here on */
    s_dns_fd = -1;
    xSemaphoreGive(s_dns_done);       /* socket closed: provision_stop may finish */
    vTaskDelete(NULL);
}

/* One semaphore for the life of the process: the task gives it on exit, start drains it before creating the task. */
static bool dns_sem_init(void) {
    if (s_dns_done) return true;
    s_dns_done = xSemaphoreCreateBinary();
    if (!s_dns_done) ESP_LOGE(TAG, "DNS semaphore failed");
    return s_dns_done != NULL;
}

/* Opens and binds UDP :53 on the AP address. Returns the fd, or -1 (logged) so the caller starts no task. */
static int dns_open(void) {
    if (s_dns_orphan) {
        /* A previous stop timed out. Its task still polls every DNS_POLL_MS and closes its own socket; binding port
         * 53 again before it is gone would fail for nothing. */
        if (xSemaphoreTake(s_dns_done, pdMS_TO_TICKS(DNS_STOP_WAIT_MS)) != pdTRUE) {
            ESP_LOGW(TAG, "previous DNS task still exiting; not starting another");
            return -1;
        }
        s_dns_orphan = false;
    }
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) { ESP_LOGE(TAG, "DNS socket: errno %d", errno); return -1; }
    struct sockaddr_in sa;
    ap_bind_addr(&sa);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        ESP_LOGE(TAG, "DNS bind :%d: errno %d", DNS_PORT, errno);
        close(fd);
        return -1;
    }
    ESP_LOGI(TAG, "captive DNS on UDP %d", DNS_PORT);
    return fd;
}

void provision_start(void) {
    s_want = true;
    portal_sync(false);
    if (s_dns_task) return;           /* already running */
    if (!dns_sem_init()) return;
    int fd = dns_open();
    if (fd < 0) return;               /* logged; no task, no socket */
    s_dns_fd = fd;
    s_gen++;
    if (xTaskCreate(dns_task, "dns53", DNS_STACK, (void *)(uintptr_t)s_gen, DNS_PRIO, &s_dns_task) != pdPASS) {
        ESP_LOGE(TAG, "DNS task failed");
        s_dns_task = NULL;
        close(fd);                    /* the task never ran: close what it would have owned */
        s_dns_fd = -1;
    }
}

void provision_stop(void) {
    s_want = false;
    portal_sync(true);
    if (!s_dns_task) return;
    s_gen++;                          /* the task closes the socket as soon as it notices */
    /* Normal case: the select() poll is 200 ms, so this returns well inside the timeout. On a timeout the task is
     * left alone: it still sees the new generation, closes its own socket and frees its stack, so nothing leaks; the
     * next start waits for it instead of binding port 53 twice. */
    if (xSemaphoreTake(s_dns_done, pdMS_TO_TICKS(DNS_STOP_WAIT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "DNS task did not stop in %d ms; it will exit on its own", DNS_STOP_WAIT_MS);
        s_dns_orphan = true;
    }
    s_dns_task = NULL;
}
