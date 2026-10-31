#pragma once
#include "settings.h"
/* BOOT button (GPIO0). Rules live in button_policy.c: a short press (< 1 s) requests a forced refresh, a 5 s hold
 * clears the web password and a 15 s hold factory-resets and restarts. s is the live settings struct from app_main;
 * it must outlive the task. Start it only after boot is finished: holding GPIO0 at power-on enters the ROM download
 * mode. */
void button_start(settings_t *s);
