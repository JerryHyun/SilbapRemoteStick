#pragma once
#include <stdint.h>
/* sdlog_event only enqueues (never blocks, drops when the queue is full); a low-priority task writes the files. */
void sdlog_init(void);
/* Appends one line to SILBAP_DIR/<file>. The same (file,key) is written at most once per 60 s. */
void sdlog_event(const char *file, uint32_t key, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
uint32_t sdlog_dropped(void);    /* events dropped because the queue was full, since boot */
uint32_t sdlog_stack_hwm(void);   /* unused stack bytes at the deepest point of the sdlog task */
