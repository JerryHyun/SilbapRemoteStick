#include "netif.h"
#include <stdio.h>
#include <string.h>
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns.h"
#include "netpolicy.h"

#define HOST_BASE "silbapstick"
#define SETUP_IP "192.168.4.1"
#define AP_LINGER_MS 30000      /* keep the setup AP this long after the STA is online */
#define TICK_MS 250
#define HEAP_LOG_MS 5000

static const char *TAG = "net";

__attribute__((weak)) void provision_start(void) {}
__attribute__((weak)) void provision_stop(void) {}
__attribute__((weak)) void provision_tick(void) {}

static settings_t *s_set;
static SemaphoreHandle_t s_mtx;
static bool s_started, s_low_power;
static volatile bool s_forced;
static volatile bool s_connected, s_ap_active;
static volatile int64_t s_fail_since_ms;        /* first disconnect/attempt without an IP; 0 = none */
static volatile int64_t s_reconnect_at_ms;      /* 0 = no reconnect scheduled */
static volatile unsigned s_attempt;
static volatile bool s_cred_dirty;
static int64_t s_ap_stop_at_ms, s_online_since_ms;
static bool s_heap_logged;
static esp_netif_t *s_sta_netif, *s_ap_netif;
static char s_ip[16], s_host[MDNS_NAME_BUF_LEN] = HOST_BASE, s_host_ret[MDNS_NAME_BUF_LEN] = HOST_BASE, s_host_local[MDNS_NAME_BUF_LEN + 8] = HOST_BASE ".local", s_ip_hosts[16];
static char s_ssid[33], s_pw[65];               /* credentials in use (mutex) */

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }
static bool have_creds(void) { return s_ssid[0] != 0; }

void net_mac_hex(char out[13]) {
    uint8_t m[6] = { 0 };
    esp_read_mac(m, ESP_MAC_WIFI_STA);
    snprintf(out, 13, "%02X%02X%02X%02X%02X%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
}

static void apply_sta_config(void) {      /* net task or net_start only */
    wifi_config_t c = { 0 };
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    strlcpy((char *)c.sta.ssid, s_ssid, sizeof c.sta.ssid);
    memcpy(c.sta.password, s_pw, strnlen(s_pw, sizeof c.sta.password));   /* up to 64 chars, no NUL needed */
    xSemaphoreGive(s_mtx);
    c.sta.threshold.authmode = c.sta.password[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    c.sta.pmf_cfg.capable = true;
    esp_wifi_set_config(WIFI_IF_STA, &c);
}

static void ap_config(wifi_config_t *c) {
    memset(c, 0, sizeof *c);
    uint8_t m[6] = { 0 };
    esp_read_mac(m, ESP_MAC_WIFI_STA);
    int n = snprintf((char *)c->ap.ssid, sizeof c->ap.ssid, "SilbapStick-Setup-%02X%02X", m[4], m[5]);
    c->ap.ssid_len = (uint8_t)n;
    strlcpy((char *)c->ap.password, s_set->ap_pw, sizeof c->ap.password);
    c->ap.channel = 1;
    c->ap.max_connection = 4;
    c->ap.authmode = WIFI_AUTH_WPA2_PSK;
}

static void enter_setup(void) {
    wifi_config_t c;
    ap_config(&c);
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &c);
    s_ap_active = true; s_ap_stop_at_ms = 0;
    ESP_LOGI(TAG, "setup AP up: %s (WPA2, %s)", (char *)c.ap.ssid, SETUP_IP);
    provision_start();
}
static void leave_setup(void) {
    provision_stop();
    esp_wifi_set_mode(WIFI_MODE_STA);
    s_ap_active = false; s_ap_stop_at_ms = 0;
    ESP_LOGI(TAG, "setup AP stopped");
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base;
    int64_t t = now_ms();
    switch (id) {
    case WIFI_EVENT_STA_START:
        if (have_creds()) {
            if (!s_fail_since_ms) s_fail_since_ms = t;
            esp_wifi_connect();
        }
        break;
    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *d = data;
        s_connected = false;
        if (!have_creds()) break;
        if (!s_fail_since_ms) s_fail_since_ms = t;
        uint32_t wait = np_backoff_ms(s_attempt++);
        s_reconnect_at_ms = t + wait;
        ESP_LOGW(TAG, "STA disconnected (reason %d), retry in %u ms", d->reason, (unsigned)wait);
        break;
    }
    default: break;
    }
}
static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base;
    if (id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        snprintf(s_ip, sizeof s_ip, IPSTR, IP2STR(&e->ip_info.ip));
        xSemaphoreGive(s_mtx);
        s_online_since_ms = now_ms();
        s_fail_since_ms = 0; s_attempt = 0; s_reconnect_at_ms = 0;
        s_connected = true;               /* last: the net task reads the fields above once this is set */
        ESP_LOGI(TAG, "online, IP %s", s_ip);
    } else if (id == IP_EVENT_STA_LOST_IP) {
        s_connected = false;
        if (!s_fail_since_ms) s_fail_since_ms = now_ms();
    }
}

static void refresh_names(void) {
    char h[MDNS_NAME_BUF_LEN] = "";
    if (mdns_hostname_get(h) == ESP_OK && h[0] && strcmp(h, s_host)) {
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        strlcpy(s_host, h, sizeof s_host);
        strlcpy(s_host_ret, h, sizeof s_host_ret);
        snprintf(s_host_local, sizeof s_host_local, "%s.local", h);
        xSemaphoreGive(s_mtx);
        ESP_LOGI(TAG, "mDNS name: %s.local", s_host);
    }
}

static void net_task(void *arg) {
    (void)arg;
    bool name_logged = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
        int64_t t = now_ms();
        if (s_cred_dirty) {
            s_cred_dirty = false;
            esp_wifi_disconnect();
            apply_sta_config();
            s_connected = false; s_attempt = 0; s_fail_since_ms = t;
            s_reconnect_at_ms = have_creds() ? t + 200 : 0;
        }
        if (s_reconnect_at_ms && t >= s_reconnect_at_ms && !s_connected) {
            s_reconnect_at_ms = 0;
            if (have_creds()) esp_wifi_connect();
        }
        np_in_t in = { .have_creds = have_creds(), .connected = s_connected,
                       .sta_fail_ms = s_fail_since_ms ? (uint32_t)(t - s_fail_since_ms) : 0, .forced_setup = s_forced };
        np_mode_t want = np_decide(&in);
        if (want == NP_SETUP_AP) {
            if (!s_ap_active) enter_setup();
            s_ap_stop_at_ms = 0;
        } else if (s_ap_active && s_connected) {
            if (!s_ap_stop_at_ms) s_ap_stop_at_ms = t + AP_LINGER_MS;
            else if (t >= s_ap_stop_at_ms) leave_setup();
        } else {
            s_ap_stop_at_ms = 0;               /* STA dropped again during the linger: keep the AP */
        }
        if (s_connected && !name_logged && t - s_online_since_ms >= 3000) { refresh_names(); name_logged = true; }
        if (!s_connected) name_logged = false;
        else if (!s_heap_logged && t - s_online_since_ms >= HEAP_LOG_MS) {
            s_heap_logged = true;
            refresh_names();
            ESP_LOGI(TAG, "heap with Wi-Fi up: free %u, largest block %u, internal free %u",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        }
        provision_tick();                  /* retries a portal route change that did not land */
    }
}

static void mdns_up(void) {
    esp_err_t e = mdns_init();
    if (e == ESP_OK) e = mdns_hostname_set(HOST_BASE);
    if (e == ESP_OK) e = mdns_instance_name_set("SilbapRemoteStick");
    if (e == ESP_OK) e = mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    if (e != ESP_OK) ESP_LOGW(TAG, "mDNS setup failed: %s", esp_err_to_name(e));
}

void net_start(settings_t *s, bool forced_setup) {
    if (s_started) return;
    s_set = s; s_forced = forced_setup;
    s_mtx = xSemaphoreCreateMutex();
    strlcpy(s_ssid, s->wifi_ssid, sizeof s_ssid);
    strlcpy(s_pw, s->wifi_pw, sizeof s_pw);
    s_low_power = esp_reset_reason() == ESP_RST_BROWNOUT;
    if (s_low_power) ESP_LOGW(TAG, "brownout reset: Wi-Fi TX power capped at 8.5 dBm");

    esp_err_t e = esp_netif_init();
    if (e == ESP_OK) e = esp_event_loop_create_default();
    if (e == ESP_ERR_INVALID_STATE) e = ESP_OK;
    if (e != ESP_OK) { ESP_LOGE(TAG, "netif/event init failed: %s", esp_err_to_name(e)); return; }
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (!s_sta_netif || !s_ap_netif) { ESP_LOGE(TAG, "netif create failed"); return; }
    esp_netif_set_hostname(s_sta_netif, HOST_BASE);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if ((e = esp_wifi_init(&cfg)) != ESP_OK) { ESP_LOGE(TAG, "wifi init failed: %s", esp_err_to_name(e)); return; }
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL);
    esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, on_ip, NULL);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    wifi_country_t cc = { .cc = "01", .schan = 1, .nchan = 13, .max_tx_power = 20, .policy = WIFI_COUNTRY_POLICY_MANUAL };
    esp_wifi_set_country(&cc);

    np_in_t in = { .have_creds = have_creds(), .forced_setup = forced_setup };
    if (np_decide(&in) == NP_SETUP_AP) enter_setup(); else esp_wifi_set_mode(WIFI_MODE_STA);
    apply_sta_config();
    if ((e = esp_wifi_start()) != ESP_OK) { ESP_LOGE(TAG, "wifi start failed: %s", esp_err_to_name(e)); return; }
    if (s_low_power) esp_wifi_set_max_tx_power(34);          /* 8.5 dBm in 0.25 dBm units */
    mdns_up();
    s_started = true;
    if (xTaskCreate(net_task, "net", 4096, NULL, 4, NULL) != pdPASS) ESP_LOGE(TAG, "net task failed");
}

net_state_t net_state(void) {
    if (!s_started) return NET_OFF;
    if (s_connected) return NET_ONLINE;
    if (s_ap_active) return NET_SETUP;
    return have_creds() ? NET_CONNECTING : NET_OFF;
}

void net_ip(char *out, size_t sz) {
    if (!sz) return;
    if (s_connected) {
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        strlcpy(out, s_ip, sz);
        xSemaphoreGive(s_mtx);
    } else if (s_ap_active) strlcpy(out, SETUP_IP, sz);
    else out[0] = 0;
}

int net_rssi(void) {
    wifi_ap_record_t r;
    if (!s_started || !s_connected || esp_wifi_sta_get_ap_info(&r) != ESP_OK) return 0;
    return r.rssi;
}

const char *net_hostname(void) {
    if (s_mtx) xSemaphoreTake(s_mtx, portMAX_DELAY);
    strlcpy(s_host_ret, s_host, sizeof s_host_ret);      /* stable buffer for the caller, updated under the lock */
    if (s_mtx) xSemaphoreGive(s_mtx);
    return s_host_ret;
}

void net_allowed_hosts(const char **out, size_t max) {
    if (!max) return;
    size_t n = 0;
    out[0] = NULL;
    if (!s_mtx) return;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (s_connected && n + 1 < max) { strlcpy(s_ip_hosts, s_ip, sizeof s_ip_hosts); out[n++] = s_ip_hosts; }
    if (n + 1 < max) out[n++] = s_host_local;
    xSemaphoreGive(s_mtx);
    if (s_ap_active && n + 1 < max) out[n++] = SETUP_IP;
    out[n] = NULL;
}

bool net_low_power(void) { return s_low_power; }

esp_err_t net_apply_credentials(const char *ssid, const char *pw) {
    if (!s_started) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    strlcpy(s_ssid, ssid, sizeof s_ssid);
    strlcpy(s_pw, pw, sizeof s_pw);
    xSemaphoreGive(s_mtx);
    strlcpy(s_set->wifi_ssid, s_ssid, sizeof s_set->wifi_ssid);
    strlcpy(s_set->wifi_pw, s_pw, sizeof s_set->wifi_pw);
    esp_err_t e = settings_save(s_set);
    if (e != ESP_OK) ESP_LOGW(TAG, "saving Wi-Fi credentials failed: %s", esp_err_to_name(e));
    s_forced = false;                   /* provisioning done: the AP now follows the normal 20 s rule */
    s_cred_dirty = true;
    return e;
}
