#pragma once
#include <stdbool.h>

typedef enum {
    BUTTON_EVENT_SHORT,    /* released before any hold threshold */
    BUTTON_EVENT_EDIT,     /* held for CONFIG_AO_EDIT_HOLD_MS (fires once while held) */
    BUTTON_EVENT_LONG,     /* held for CONFIG_AO_LONG_PRESS_SETUP_MS (fires once while held) */
    BUTTON_EVENT_HOLD_END, /* released after BUTTON_EVENT_EDIT and/or BUTTON_EVENT_LONG fired */
} button_event_t;

typedef void (*button_cb_t)(button_event_t ev);

void button_init(void);
bool button_is_pressed(void);           /* raw debounced level */
void button_start_events(button_cb_t cb); /* start the event task */
