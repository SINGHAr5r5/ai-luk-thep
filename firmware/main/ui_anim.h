#pragma once
#include "lvgl.h"
#include "app_state.h"
#include "ui.h"

// All of these must be called with the LVGL lock held.
void ui_anim_create(void);                       // build the screen (Orb, title, QR, caption)
void ui_anim_set_state(app_state_t s);           // re-colour / switch the scene for a state
void ui_anim_show_qr(const char *payload, ui_caption_t cap);
void ui_anim_set_caption(ui_caption_t cap);
void ui_anim_set_status(const char *text);
void ui_anim_set_ota_progress(int pct);
