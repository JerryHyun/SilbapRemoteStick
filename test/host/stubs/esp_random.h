#pragma once
/* host-test stub: main/settings.c only needs the two fill calls (see nvs_stub.c) */
#include <stddef.h>
#include <stdint.h>
uint32_t esp_random(void);
void     esp_fill_random(void *buf, size_t len);
