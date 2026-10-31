#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "partition_card.h"
typedef enum { MSC_NO_MEDIUM, MSC_BUSY, MSC_READY } msc_state_t;
esp_err_t usb_msc_start(void);
void      usb_msc_set_state(msc_state_t s);   /* state only: the volume is set by usb_msc_set_volume */
/* The sector window the host sees, copied out of v (valid until the next sdcard_unmount). NULL, or a volume with
 * zero sectors, means there is nothing to serve: the callbacks answer NOT READY. Call it after a successful
 * sdcard_mount() and before usb_msc_set_state(MSC_READY). */
void      usb_msc_set_volume(const card_vol_t *v);
uint32_t  usb_msc_last_read_ms(void);         /* esp_timer ms of the last READ10 */
uint32_t  usb_msc_last_write_ms(void);        /* esp_timer ms of the last WRITE10 */
/* Drop/restore the controller's VBUS view so the host sees a real unplug and re-reads the directory
 * (see usb_msc.c). Releasing D+ alone, the old tud_disconnect() approach, is debounced away by some host
 * stacks and they never re-read. The caller must wait >= RP_REPLUG_MIN_MS between off and on. */
void      usb_msc_vbus(bool on);
