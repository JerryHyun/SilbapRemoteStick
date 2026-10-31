#include "sdlog.h"
#include "board.h"
#include "sdcard.h"
#include "esp_timer.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define SLOTS 16
#define LOG_MAX_BYTES (256 * 1024)
static struct { uint32_t h; int64_t t; bool used; } s_rl[SLOTS];
static QueueHandle_t s_q;
static volatile uint32_t s_dropped;
static TaskHandle_t s_task;

typedef struct { uint32_t key; int64_t t; char file[24]; char msg[100]; } rec_t;

static uint32_t fnv(const char *s, uint32_t key) {
    uint32_t h = 2166136261u ^ key;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

/* Runs in the sdlog task only: rate limit, card check and file write. */
static void write_rec(const rec_t *r) {
    uint32_t h = fnv(r->file, r->key);
    int slot = -1, oldest = 0;
    for (int i = 0; i < SLOTS; i++) {
        if (s_rl[i].used && s_rl[i].h == h) { slot = i; break; }
        if (!s_rl[i].used) { if (slot < 0) slot = i; }
        else if (s_rl[i].t < s_rl[oldest].t) oldest = i;
    }
    if (slot >= 0 && s_rl[slot].used && r->t - s_rl[slot].t < 60 * 1000000LL) return;
    if (slot < 0) slot = oldest;
    s_rl[slot].used = true; s_rl[slot].h = h; s_rl[slot].t = r->t;

    char path[96], line[160];
    snprintf(path, sizeof path, "%s/%s", SILBAP_DIR, r->file);
    snprintf(line, sizeof line, "[%llu ms] %s\n", (unsigned long long)(r->t / 1000), r->msg);
    sdcard_lock();
    if (sdcard_present()) {
        struct stat st;
        if (stat(path, &st) != 0 || st.st_size < LOG_MAX_BYTES) {
            FILE *f = fopen(path, "a");
            if (f) { fputs(line, f); fclose(f); }
        }
    }
    sdcard_unlock();
}

static void sdlog_task(void *arg) {
    (void)arg;
    rec_t r;
    for (;;) {
        if (xQueueReceive(s_q, &r, portMAX_DELAY) == pdTRUE) write_rec(&r);
    }
}

void sdlog_init(void) {
    if (s_q) return;
    s_q = xQueueCreate(8, sizeof(rec_t));
    if (s_q) xTaskCreate(sdlog_task, "sdlog", 4096, NULL, 2, &s_task);
}

/* Callable from any task including TinyUSB callbacks: enqueue only, never blocks, never touches the SD. */
void sdlog_event(const char *file, uint32_t key, const char *fmt, ...) {
    if (!s_q) return;
    rec_t r;
    r.key = key; r.t = esp_timer_get_time();
    strlcpy(r.file, file, sizeof r.file);
    va_list ap; va_start(ap, fmt);
    vsnprintf(r.msg, sizeof r.msg, fmt, ap);
    va_end(ap);
    if (xQueueSend(s_q, &r, 0) != pdTRUE) s_dropped++;
}

uint32_t sdlog_dropped(void) { return s_dropped; }
uint32_t sdlog_stack_hwm(void) { return s_task ? (uint32_t)uxTaskGetStackHighWaterMark(s_task) * sizeof(StackType_t) : 0; }
