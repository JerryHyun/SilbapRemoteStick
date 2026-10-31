#include "timekeep.h"
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "timekeep_core.h"

static const char *TAG = "timekeep";
static tk_state_t s_st;
static SemaphoreHandle_t s_mtx;
static bool s_sntp_init;

static void apply_tz(int tz_min) {
    char tz[24];
    tk_tz_string(tz_min, tz, sizeof tz);
    setenv("TZ", tz, 1);
    tzset();
}
static void set_rtc(int64_t sec) {
    struct timeval tv = { .tv_sec = (time_t)sec, .tv_usec = 0 };
    settimeofday(&tv, NULL);
}

static void maybe_save(void) {
    int64_t now = timekeep_now();
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    bool save = tk_should_save(&s_st, now);
    xSemaphoreGive(s_mtx);
    if (!save) return;
    if (settings_save_last_time(now) == ESP_OK) {
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        s_st.last_saved = now;
        xSemaphoreGive(s_mtx);
    } else ESP_LOGW(TAG, "saving last_time failed");
}
static void on_timer(void *arg) {
    (void)arg;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    tk_check_uptime(&s_st, esp_timer_get_time() / 1000000);   /* 24 h on browser time alone: persist it */
    xSemaphoreGive(s_mtx);
    maybe_save();
}

static void on_sntp(struct timeval *tv) {
    (void)tv;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    tk_on_sntp(&s_st, (int64_t)time(NULL));
    xSemaphoreGive(s_mtx);
    ESP_LOGI(TAG, "SNTP synced");
    maybe_save();
}
static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base; (void)id; (void)data;
    esp_err_t e;
    if (!s_sntp_init) {
        esp_sntp_config_t c = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        c.wait_for_sync = false; c.sync_cb = on_sntp;
        e = esp_netif_sntp_init(&c);
        if (e == ESP_OK) s_sntp_init = true;
    } else e = esp_netif_sntp_start();      /* restart: new network, new lease */
    if (e != ESP_OK) ESP_LOGW(TAG, "SNTP start failed: %s", esp_err_to_name(e));
}

void timekeep_start(settings_t *s) {
    if (s_mtx) return;
    s_mtx = xSemaphoreCreateMutex();
    int64_t last = 0;
    settings_load_last_time(&last);
    s_st = (tk_state_t){ .last_saved = last, .floor = last, .synced = false };
    apply_tz(s->tz_min >= -14 * 60 && s->tz_min <= 14 * 60 ? s->tz_min : 0);
    struct timeval tv;
    gettimeofday(&tv, NULL);
    if (tv.tv_sec < last) { set_rtc(last); ESP_LOGW(TAG, "RTC below saved time, raised to %lld", (long long)last); }

    esp_err_t e = esp_netif_init();
    if (e == ESP_OK) e = esp_event_loop_create_default();
    if (e == ESP_ERR_INVALID_STATE) e = ESP_OK;
    if (e == ESP_OK) e = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip, NULL);
    if (e != ESP_OK) ESP_LOGE(TAG, "SNTP hook failed: %s", esp_err_to_name(e));

    /* the guard checks every 10 min; tk_should_save keeps NVS writes to at most one per hour */
    const esp_timer_create_args_t a = { .callback = on_timer, .name = "tk_save" };
    esp_timer_handle_t t;
    if (esp_timer_create(&a, &t) == ESP_OK) esp_timer_start_periodic(t, 600ULL * 1000000ULL);
}

int64_t timekeep_now(void) {
    if (!s_mtx) return (int64_t)time(NULL);
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    int64_t v = tk_effective(&s_st, (int64_t)time(NULL));
    xSemaphoreGive(s_mtx);
    return v;
}
bool timekeep_synced(void) {
    if (!s_mtx) return false;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    bool v = s_st.synced;
    xSemaphoreGive(s_mtx);
    return v;
}
bool timekeep_client_hint(int64_t client_ms, int tz_min) {
    if (!s_mtx) return false;
    if (tz_min >= -14 * 60 && tz_min <= 14 * 60) apply_tz(tz_min);
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    bool ok = tk_accept_client(&s_st, (int64_t)time(NULL), client_ms);
    if (ok) { set_rtc(client_ms / 1000); tk_on_client(&s_st); }
    xSemaphoreGive(s_mtx);
    if (ok) { ESP_LOGI(TAG, "clock set from browser (RAM only until SNTP or 24 h uptime)"); }
    return ok;
}
