#pragma once
/* Test controls for the in-RAM NVS stub. */
#include <stdint.h>

void nvs_stub_reset(void);                       /* empty NVS: the namespace does not exist yet */
void nvs_stub_set_u8(const char *key, uint8_t v);/* as if a previous boot had saved it */
