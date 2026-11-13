#pragma once
#include <stdbool.h>

typedef enum {
    UI_MODE_BOOT,        /* splash + "hold button for setup" countdown */
    UI_MODE_SETUP_AP,    /* provisioning access point instructions */
    UI_MODE_NORMAL,      /* agent monitor (shows "connecting" while Wi-Fi is down) */
    UI_MODE_MESSAGE,     /* full-screen message (e.g. "Saved, rebooting") */
} ui_mode_t;

void ui_start(void);
void ui_set_mode(ui_mode_t mode);
/* Boot screen: progress of the setup window and of the button hold, 0..100. */
void ui_set_boot_progress(int window_pct, int hold_pct);
void ui_set_message(const char *title, const char *body);
