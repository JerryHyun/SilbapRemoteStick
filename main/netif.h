#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "settings.h"
typedef enum { NET_OFF, NET_CONNECTING, NET_ONLINE, NET_SETUP } net_state_t;
/* Starts Wi-Fi (STA, plus the setup AP when np_decide says so), mDNS and the policy task. Never blocks on the
 * network; failures are logged and leave the USB disk untouched. s must outlive the net layer. */
void        net_start(settings_t *s, bool forced_setup);
net_state_t net_state(void);
void        net_ip(char *out, size_t sz);        /* STA IP, or "192.168.4.1" while only the setup AP is up */
int         net_rssi(void);                      /* dBm of the STA link, 0 when not connected */
const char *net_hostname(void);                  /* final mDNS name, e.g. "silbapstick" */
void        net_allowed_hosts(const char **out, size_t max);   /* NULL-terminated: IP, <hostname>.local, 192.168.4.1 in setup */
bool        net_low_power(void);                 /* brownout reset: TX power capped at 8.5 dBm */
esp_err_t   net_apply_credentials(const char *ssid, const char *pw);   /* save + reconnect, keeps the AP until online; returns the settings_save result */
void        net_mac_hex(char out[13]);           /* base MAC, 12 uppercase hex digits */
/* Provisioning hooks (Task 10 provides the real ones; weak no-ops until then). */
void provision_start(void);
void provision_stop(void);
