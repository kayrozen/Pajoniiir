/*
 * log_screen.h - on-screen verbose log viewer.
 */
#pragma once

#include <stdbool.h>

/*
 * Install the log-teeing vprintf and create the full-screen log view.
 * Call after the display/LVGL stack is up (also works before it: the label
 * is created only when LVGL is ready, lines keep buffering either way).
 */
void log_screen_start(void);
/* Re-install our log hook (call after anything else replaces the vprintf). */
void log_screen_rehook(void);

/* Call periodically from the UI loop to refresh the on-screen text. */
void log_screen_pause_render(bool pause);
void log_screen_task(void);
