#pragma once

typedef void (*button_cb_t)(void);

// BOOT button: pressed = push-to-talk, held 12 s = forget Wi-Fi and reboot.
void buttons_init(void);
void buttons_set_ptt(button_cb_t on_press, button_cb_t on_release);
