/*
 * lcd_ui.h - the display task: sample the system, diff it, paint the regions.
 *
 * The task owns every pixel on the panel; nothing else may call lcd_fill_rect
 * or lcd_blit while it runs.  It never takes sdcard_lock and never touches the
 * card: it reads net_state()/net_ip()/net_rssi() through their locks, copies
 * refresh_status() and reads the settings, so a slow SD never stalls it and it
 * never stalls the SD.
 */
#pragma once

#include "lcd_view.h"

/* Creates the painter task (core 0, priority below the refresh task, 4096 byte
 * stack).  Call once, after net_start() and refresh_start(): the task samples
 * refresh_status(), which exists from then on.  lcd_init() must already have
 * run, otherwise every draw call is a silent no-op. */
void lcd_ui_start(void);

/* Pushes a complete view for the next pass, for a caller that wants to put
 * something on the glass itself (a countdown, a message).  The view is copied
 * under a critical section and used for one pass; the task then goes back to
 * sampling the system.  NULL is ignored.  Safe from any task; never blocks. */
void lcd_ui_publish(const lcd_view_t *v);
