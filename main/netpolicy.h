#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef enum { NP_STA_ONLY, NP_SETUP_AP } np_mode_t;
typedef struct { bool have_creds; bool connected; uint32_t sta_fail_ms; bool forced_setup; } np_in_t;
#define NP_SETUP_AFTER_MS 20000u
/* AP when !have_creds || forced_setup || (!connected && sta_fail_ms >= 20000) */
np_mode_t np_decide(const np_in_t *in);
/* STA reconnect delay after the n-th failed attempt in a row (0-based): 2 s, 5 s, 10 s, then 30 s */
uint32_t  np_backoff_ms(unsigned attempt);
