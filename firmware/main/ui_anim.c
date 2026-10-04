// Screen scenes (240x240). One top bar (status dot + state word, battery, clock) is always there, under it either
//   - the eyes: two emoji-style eyes whose iris colour, gaze and lids follow the state    (idle, listening, ...)
//   - the conversation: two rounded cards, "what was heard" and "the reply"      (text from the bridge)
//   - the Wi-Fi setup QR code, or the OTA progress ring.
// apply_scene() is the only place that decides what is visible, so states, text, QR and OTA cannot fight.
#include "ui_anim.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "img_text.h"
#include "img_eyes.h"
#include <math.h>
#include "esp_random.h"
#include "audio_out.h"
#include "src/misc/cache/instance/lv_image_cache.h"   // lv_image_cache_drop: not in the public lvgl.h

#define COL_BG        0x05070F
#define COL_CARD      0x121A2E
#define COL_TEXT_DIM  0x9AA3B8
#define COL_DETAIL    0x4B5470
#define COL_HEARD     0x7FE7FF
#define COL_REPLY     0xFFE9A8

#define EYE_CY 116
#define TEXT_HOLD_MS 12000
static const int EYE_X[2] = {26, 126};          // left edge of each 88 px wide eye
#define GAZE_AX 13.0f                           // how far the iris may travel inside the eye (px)
#define GAZE_AY 40.0f

// ---- top bar
static lv_obj_t *s_dot_glow, *s_dot, *s_state_lbl, *s_time_lbl, *s_batt_lbl;
static lv_anim_t s_dot_pulse;
// ---- eye scene
static lv_obj_t *s_sclera[2], *s_iris[2];
static lv_obj_t *s_detail;     // dim line under the eyes: IP address + version
static lv_timer_t *s_eye_timer;
typedef enum { M_IDLE, M_LISTEN, M_THINK, M_SPEAK, M_ERROR, M_SCAN, M_BOOT } mood_t;
static mood_t s_mood = M_BOOT;
static float s_gx, s_gy, s_tx, s_ty, s_smooth = 0.28f, s_base_open = 1.0f;
static uint32_t s_next_move, s_next_blink, s_blink_t0, s_shake_t0;
static int s_blink_phase;      // 0 open, 1 closing, 2 closed, 3 opening
static bool s_was_double, s_side;
// ---- other scenes
static lv_obj_t *s_qr;
static bool s_qr_ready;
static lv_obj_t *s_caption;
static bool s_caption_on;
static lv_obj_t *s_arc, *s_pct;
// ---- conversation: [0] = what was heard, [1] = the reply. Two descriptors per role so that a new bitmap
// always has a new address (LVGL caches images by source pointer).
static lv_obj_t *s_card[2];
static lv_obj_t *s_txt_img[2];
static lv_image_dsc_t s_txt_dsc[2][2];
static uint8_t *s_txt_buf[2];
static int s_txt_flip[2];
static bool s_text_visible;
static lv_timer_t *s_text_timer;

static app_state_t s_state = ST_BOOT;

static lv_color_t state_color(app_state_t s)
{
    switch (s) {
    case ST_BOOT:                return lv_color_hex(0xDDE3F5);
    case ST_WIFI_PROVISIONING:   return lv_color_hex(0x3AA0FF);
    case ST_WIFI_CONNECTING:
    case ST_BRIDGE_CONNECTING:   return lv_color_hex(0x7FD0FF);
    case ST_IDLE:                return lv_color_hex(0x6B6BFF);  // blue-violet
    case ST_LISTENING:           return lv_color_hex(0x2EE6A8);  // mint
    case ST_THINKING:            return lv_color_hex(0xB45CFF);  // violet
    case ST_SPEAKING:            return lv_color_hex(0xFFA630);  // gold
    case ST_OTA_UPDATING:        return lv_color_hex(0xFFD93A);  // yellow
    case ST_ERROR:               return lv_color_hex(0xFF4B4B);  // red
    }
    return lv_color_hex(0x6B6BFF);
}

static const char *state_text(app_state_t s)
{
    switch (s) {
    case ST_BOOT:                return "starting";
    case ST_WIFI_CONNECTING:     return "wi-fi";
    case ST_WIFI_PROVISIONING:   return "setup";
    case ST_BRIDGE_CONNECTING:   return "linking";
    case ST_IDLE:                return "ready";
    case ST_LISTENING:           return "listening";
    case ST_THINKING:            return "thinking";
    case ST_SPEAKING:            return "speaking";
    case ST_OTA_UPDATING:        return "update";
    case ST_ERROR:               return "error";
    }
    return "";
}

static const lv_image_dsc_t *caption_img(ui_caption_t c, uint32_t *color)
{
    switch (c) {
    case UI_CAP_SCAN_AP:    *color = 0xFFD93A; return &img_scan_ap;
    case UI_CAP_SCAN_SETUP: *color = 0x2EE6A8; return &img_scan_setup;
    case UI_CAP_CONNECTING: *color = 0x7FD0FF; return &img_connecting;
    case UI_CAP_CONNECTED:  *color = 0x2EE6A8; return &img_connected;
    case UI_CAP_FAILED:     *color = 0xFF5A5A; return &img_failed;
    case UI_CAP_TESTING:    *color = 0x7FD0FF; return &img_testing;
    case UI_CAP_RESET:      *color = 0xFFD93A; return &img_reset;
    case UI_CAP_WIFI_LOST:  *color = 0xFFA630; return &img_wifi_lost;
    case UI_CAP_UPDATING:   *color = 0xFFD93A; return &img_updating;
    case UI_CAP_UPDATE_DONE:*color = 0x2EE6A8; return &img_update_done;
    default:                *color = 0xFFFFFF; return NULL;
    }
}

// ------------------------------------------------------------------ small helpers
static lv_obj_t *circle(lv_color_t c, lv_opa_t opa)
{
    lv_obj_t *o = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(o);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, c, 0);
    lv_obj_set_style_bg_opa(o, opa, 0);
    lv_obj_set_clickable(o, false);
    lv_obj_set_scrollable(o, false);
    return o;
}

static const lv_image_dsc_t *iris_for(app_state_t st)
{
    switch (st) {
    case ST_LISTENING:         return &img_iris_listening;
    case ST_THINKING:          return &img_iris_thinking;
    case ST_SPEAKING:          return &img_iris_speaking;
    case ST_WIFI_CONNECTING:
    case ST_BRIDGE_CONNECTING: return &img_iris_connecting;
    case ST_ERROR:             return &img_iris_error;
    case ST_BOOT:              return &img_iris_boot;
    default:                   return &img_iris_idle;      // warm brown, like the reference picture
    }
}

static float frand(float lo, float hi) { return lo + (hi - lo) * ((esp_random() & 0xFFFF) / 65535.0f); }
static uint32_t rrange(uint32_t lo, uint32_t hi) { return lo + esp_random() % (hi - lo + 1); }

static uint32_t blink_interval(void)
{
    switch (s_mood) {
    case M_LISTEN: return rrange(4000, 8000);   // attentive: blinks less
    case M_ERROR:  return rrange(4500, 7000);
    default:       return rrange(2200, 5800);
    }
}

static void pick_target(uint32_t now)
{
    switch (s_mood) {
    case M_IDLE:
        if (frand(0, 1) < 0.5f) { s_tx = frand(-0.3f, 0.3f); s_ty = frand(-0.3f, 0.3f); }   // near the middle
        else { float a = frand(0, 6.2832f), r = 0.45f + 0.5f * frand(0, 1); s_tx = r * cosf(a); s_ty = r * sinf(a); }
        s_next_move = now + rrange(1100, 3200);
        break;
    case M_LISTEN:
        s_tx = frand(-0.06f, 0.06f); s_ty = frand(-0.06f, 0.06f);
        s_next_move = now + 700;
        break;
    case M_THINK:                                // look up and to the side, then the other side
        s_tx = s_side ? 0.75f : -0.75f; s_ty = -0.85f; s_side = !s_side;
        s_next_move = now + 1500;
        break;
    case M_SPEAK:
        s_tx = frand(-0.15f, 0.15f); s_ty = frand(-0.1f, 0.2f);
        s_next_move = now + rrange(600, 1200);
        break;
    case M_ERROR:
        s_tx = 0; s_ty = 0.35f;
        s_next_move = now + 1000;
        break;
    case M_SCAN:                                 // looking around while it connects
        s_tx = s_side ? 0.9f : -0.9f; s_ty = 0; s_side = !s_side;
        s_next_move = now + 800;
        break;
    default:
        s_tx = 0; s_ty = 0;
        s_next_move = now + 1000;
    }
}

static void eye_apply(float dx, float open, float iris_k)
{
    float gx = s_gx, gy = s_gy, n = sqrtf(gx * gx + gy * gy);
    if (n > 1.0f) { gx /= n; gy /= n; }
    if (open < 0.06f) open = 0.06f;
    if (open > 1.12f) open = 1.12f;
    for (int i = 0; i < 2; i++) {
        lv_image_set_scale_y(s_sclera[i], (int)(256 * open));
        float cx = EYE_X[i] + EYE_W / 2 + dx + gx * GAZE_AX;
        float cy = EYE_CY + gy * GAZE_AY * open;
        lv_obj_set_pos(s_iris[i], (int)(cx - IRIS_D / 2), (int)(cy - IRIS_D / 2));
        lv_image_set_scale_x(s_iris[i], (int)(256 * iris_k));
        lv_image_set_scale_y(s_iris[i], (int)(256 * open * iris_k));
    }
}

static void eye_tick(lv_timer_t *t)
{
    (void)t;
    uint32_t now = lv_tick_get();
    if (now >= s_next_move) pick_target(now);
    s_gx += (s_tx - s_gx) * s_smooth;
    s_gy += (s_ty - s_gy) * s_smooth;

    float bl = 1.0f;                              // 1 = lids open
    switch (s_blink_phase) {
    case 0:
        if (now >= s_next_blink) { s_blink_phase = 1; s_blink_t0 = now; }
        break;
    case 1: {
        uint32_t d = now - s_blink_t0;
        if (d >= 70) { s_blink_phase = 2; s_blink_t0 = now; bl = 0; } else bl = 1.0f - d / 70.0f;
        break; }
    case 2:
        bl = 0;
        if (now - s_blink_t0 >= 40) { s_blink_phase = 3; s_blink_t0 = now; }
        break;
    case 3: {
        uint32_t d = now - s_blink_t0;
        if (d >= 120) {
            s_blink_phase = 0;
            if (!s_was_double && frand(0, 1) < 0.18f) { s_next_blink = now + 140; s_was_double = true; }   // double blink
            else { s_next_blink = now + blink_interval(); s_was_double = false; }
        } else bl = d / 120.0f;
        break; }
    }

    float open = s_base_open * (0.06f + 0.94f * bl);
    float level = audio_out_level();               // talking makes the irises "breathe" with the voice
    float k = 1.0f + (level * 0.45f > 0.12f ? 0.12f : level * 0.45f);
    float dx = 0;
    if (s_mood == M_ERROR && now - s_shake_t0 < 500)
        dx = sinf((now - s_shake_t0) * 0.06f) * 5.0f * (1.0f - (now - s_shake_t0) / 500.0f);
    eye_apply(dx, open, k);
}

static void dot_pulse_cb(void *var, int32_t v)  // v: 30..130
{
    (void)var;
    lv_obj_set_style_bg_opa(s_dot_glow, (lv_opa_t)v, 0);
}

static void clock_cb(lv_timer_t *t)
{
    (void)t;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_year + 1900 < 2024) lv_label_set_text(s_time_lbl, "--:--");       // not synced yet
    else lv_label_set_text_fmt(s_time_lbl, "%02d:%02d", tm.tm_hour, tm.tm_min);
}

// ------------------------------------------------------------------ conversation cards
static void text_relayout(void)
{
    int y = 36;
    for (int i = 0; i < 2; i++) {
        if (!s_txt_buf[i]) continue;
        int h = s_txt_dsc[i][s_txt_flip[i]].header.h + 8;
        lv_obj_set_size(s_card[i], 232, h);
        lv_obj_align(s_card[i], LV_ALIGN_TOP_MID, 0, y);
        lv_obj_align(s_txt_img[i], LV_ALIGN_TOP_LEFT, 3, 3);
        y += h + 6;
    }
}

// One place decides what is visible.
static void apply_scene(void)
{
    bool setup = (s_state == ST_WIFI_PROVISIONING);
    bool ota = (s_state == ST_OTA_UPDATING);
    bool text = s_text_visible && !setup && !ota;
    bool eyes = !setup && !ota && !text;

    for (int i = 0; i < 2; i++) {
        lv_obj_set_hidden(s_sclera[i], !eyes);
        lv_obj_set_hidden(s_iris[i], !eyes);
        lv_image_set_src(s_iris[i], iris_for(s_state));
        // a faint red wash over the whites when something is wrong
        lv_obj_set_style_image_recolor(s_sclera[i], lv_color_hex(0xFF4040), 0);
        lv_obj_set_style_image_recolor_opa(s_sclera[i], s_state == ST_ERROR ? LV_OPA_30 : LV_OPA_TRANSP, 0);
    }
    if (eyes) lv_timer_resume(s_eye_timer); else lv_timer_pause(s_eye_timer);   // no animation cost while hidden
    lv_obj_set_hidden(s_detail, !(eyes && !s_caption_on));
    lv_obj_set_hidden(s_qr, !(setup && s_qr_ready));
    lv_obj_set_hidden(s_arc, !ota);
    lv_obj_set_hidden(s_pct, !ota);
    for (int i = 0; i < 2; i++) lv_obj_set_hidden(s_card[i], !(text && s_txt_buf[i]));
    lv_obj_set_style_border_color(s_card[1], state_color(s_state), 0);

    lv_color_t c = state_color(s_state);
    lv_obj_set_style_bg_color(s_dot, c, 0);
    lv_obj_set_style_bg_color(s_dot_glow, c, 0);
    lv_label_set_text(s_state_lbl, state_text(s_state));
}

static void text_free(int i)
{
    if (!s_txt_buf[i]) return;
    lv_obj_set_hidden(s_card[i], true);
    lv_image_cache_drop(&s_txt_dsc[i][s_txt_flip[i]]);
    free(s_txt_buf[i]);
    s_txt_buf[i] = NULL;
}

void ui_anim_clear_text(void)
{
    text_free(0);
    text_free(1);
    s_text_visible = false;
    lv_timer_pause(s_text_timer);
    apply_scene();
}

static void text_timeout(lv_timer_t *t)
{
    (void)t;
    ui_anim_clear_text();
}

void ui_anim_show_text(const char *role, int w, int h, uint8_t *a8)
{
    int i = (role && role[0] == 's') ? 0 : 1;
    if (i == 0 && s_txt_buf[1]) text_free(1);            // a new "heard" line starts a new turn
    uint8_t *old = s_txt_buf[i];
    int old_flip = s_txt_flip[i];
    s_txt_flip[i] ^= 1;
    lv_image_dsc_t *d = &s_txt_dsc[i][s_txt_flip[i]];
    memset(d, 0, sizeof(*d));
    d->header.magic = LV_IMAGE_HEADER_MAGIC;
    d->header.cf = LV_COLOR_FORMAT_A8;
    d->header.w = w;
    d->header.h = h;
    d->header.stride = w;
    d->data_size = (uint32_t)w * h;
    d->data = a8;
    s_txt_buf[i] = a8;
    lv_image_set_src(s_txt_img[i], d);
    if (old) {
        lv_image_cache_drop(&s_txt_dsc[i][old_flip]);
        free(old);
    }
    s_text_visible = true;
    lv_timer_pause(s_text_timer);                         // stays up while the turn is running
    text_relayout();
    apply_scene();
}

// ------------------------------------------------------------------ build
void ui_anim_create(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // eyes first so that everything else is drawn above them
    for (int i = 0; i < 2; i++) {
        s_sclera[i] = lv_image_create(scr);
        lv_image_set_src(s_sclera[i], &img_eye_sclera);
        lv_image_set_pivot(s_sclera[i], EYE_W / 2, EYE_H / 2);
        lv_obj_set_pos(s_sclera[i], EYE_X[i], EYE_CY - EYE_H / 2);
        s_iris[i] = lv_image_create(scr);
        lv_image_set_src(s_iris[i], &img_iris_boot);
        lv_image_set_pivot(s_iris[i], IRIS_D / 2, IRIS_D / 2);
    }
    eye_apply(0, 0.06f, 1.0f);
    s_eye_timer = lv_timer_create(eye_tick, 33, NULL);

    s_detail = lv_label_create(scr);
    lv_obj_set_style_text_font(s_detail, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_detail, lv_color_hex(COL_DETAIL), 0);
    lv_label_set_text(s_detail, "");
    lv_obj_align(s_detail, LV_ALIGN_BOTTOM_MID, 0, -5);

    // QR scene
    s_qr = lv_qrcode_create(scr);
    lv_qrcode_set_size(s_qr, 150);
    lv_qrcode_set_dark_color(s_qr, lv_color_black());
    lv_qrcode_set_light_color(s_qr, lv_color_white());
    lv_qrcode_set_quiet_zone(s_qr, true);
    lv_obj_set_style_radius(s_qr, 10, 0);
    lv_obj_align(s_qr, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_hidden(s_qr, true);

    // OTA ring
    s_arc = lv_arc_create(scr);
    lv_obj_set_size(s_arc, 140, 140);
    lv_obj_align(s_arc, LV_ALIGN_TOP_MID, 0, 46);
    lv_arc_set_rotation(s_arc, 270);
    lv_arc_set_bg_angles(s_arc, 0, 360);
    lv_arc_set_range(s_arc, 0, 100);
    lv_arc_set_value(s_arc, 0);
    lv_obj_remove_style(s_arc, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(s_arc, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 12, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_arc, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(COL_CARD), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(0xFFD93A), LV_PART_INDICATOR);
    lv_obj_set_clickable(s_arc, false);
    lv_obj_set_hidden(s_arc, true);
    s_pct = lv_label_create(scr);
    lv_obj_set_style_text_font(s_pct, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_pct, lv_color_hex(0xFFD93A), 0);
    lv_label_set_text(s_pct, "0%");
    lv_obj_align(s_pct, LV_ALIGN_TOP_MID, 0, 46 + 70 - 12);
    lv_obj_set_hidden(s_pct, true);

    // conversation cards
    static const uint32_t text_colors[2] = {COL_HEARD, COL_REPLY};
    static const uint32_t border_colors[2] = {0x3C7F92, 0xFFA630};
    for (int i = 0; i < 2; i++) {
        s_card[i] = lv_obj_create(scr);
        lv_obj_remove_style_all(s_card[i]);
        lv_obj_set_style_radius(s_card[i], 12, 0);
        lv_obj_set_style_bg_color(s_card[i], lv_color_hex(COL_CARD), 0);
        lv_obj_set_style_bg_opa(s_card[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_card[i], 1, 0);
        lv_obj_set_style_border_color(s_card[i], lv_color_hex(border_colors[i]), 0);
        lv_obj_set_style_border_opa(s_card[i], LV_OPA_60, 0);
        lv_obj_set_clickable(s_card[i], false);
        lv_obj_set_scrollable(s_card[i], false);
        lv_obj_set_hidden(s_card[i], true);
        s_txt_img[i] = lv_image_create(s_card[i]);
        lv_obj_set_style_image_recolor(s_txt_img[i], lv_color_hex(text_colors[i]), 0);
        lv_obj_set_style_image_recolor_opa(s_txt_img[i], LV_OPA_COVER, 0);
    }
    s_text_timer = lv_timer_create(text_timeout, TEXT_HOLD_MS, NULL);
    lv_timer_pause(s_text_timer);

    // caption (Thai messages as images) sits at the bottom of every scene
    s_caption = lv_image_create(scr);
    lv_obj_set_style_image_recolor_opa(s_caption, LV_OPA_COVER, 0);
    lv_obj_align(s_caption, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_hidden(s_caption, true);

    // top bar
    s_dot_glow = circle(lv_color_hex(0x6B6BFF), 60);
    lv_obj_set_size(s_dot_glow, 18, 18);
    lv_obj_align(s_dot_glow, LV_ALIGN_TOP_LEFT, 7, 10);
    s_dot = circle(lv_color_hex(0x6B6BFF), 255);
    lv_obj_set_size(s_dot, 10, 10);
    lv_obj_align(s_dot, LV_ALIGN_TOP_LEFT, 11, 14);
    s_state_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_state_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_state_lbl, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_align(s_dot, LV_ALIGN_TOP_LEFT, 11, 14);
    lv_obj_align(s_state_lbl, LV_ALIGN_TOP_LEFT, 30, 11);
    s_time_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_time_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_time_lbl, lv_color_hex(0xE8ECF8), 0);
    lv_label_set_text(s_time_lbl, "--:--");
    lv_obj_align(s_time_lbl, LV_ALIGN_TOP_RIGHT, -14, 7);
    lv_timer_create(clock_cb, 1000, NULL);
    s_batt_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_batt_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_batt_lbl, lv_color_hex(COL_TEXT_DIM), 0);
    lv_label_set_text(s_batt_lbl, "");
    lv_obj_align_to(s_batt_lbl, s_time_lbl, LV_ALIGN_OUT_LEFT_MID, -8, 0);

    lv_anim_init(&s_dot_pulse);                       // the status dot's halo pulses, so the board looks alive
    lv_anim_set_exec_cb(&s_dot_pulse, dot_pulse_cb);
    lv_anim_set_values(&s_dot_pulse, 30, 130);
    lv_anim_set_duration(&s_dot_pulse, 1100);
    lv_anim_set_reverse_duration(&s_dot_pulse, 1100);
    lv_anim_set_path_cb(&s_dot_pulse, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&s_dot_pulse, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&s_dot_pulse);

    ui_anim_set_state(ST_BOOT);
}

// ------------------------------------------------------------------ public
void ui_anim_set_caption(ui_caption_t cap)
{
    uint32_t color;
    const lv_image_dsc_t *img = caption_img(cap, &color);
    if (img) {
        lv_image_set_src(s_caption, img);
        lv_obj_set_style_image_recolor(s_caption, lv_color_hex(color), 0);
        lv_obj_align(s_caption, LV_ALIGN_BOTTOM_MID, 0, -4);
    }
    s_caption_on = (img != NULL);
    lv_obj_set_hidden(s_caption, img == NULL);
    apply_scene();
}

void ui_anim_set_status(const char *text)
{
    lv_label_set_text(s_detail, text);
    lv_obj_align(s_detail, LV_ALIGN_BOTTOM_MID, 0, -5);
}

void ui_anim_show_qr(const char *payload, ui_caption_t cap)
{
    lv_qrcode_update(s_qr, payload, strlen(payload));
    lv_obj_align(s_qr, LV_ALIGN_TOP_MID, 0, 40);
    s_qr_ready = true;
    ui_anim_set_caption(cap);     // also re-applies the scene
}

void ui_anim_set_ota_progress(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    lv_arc_set_value(s_arc, pct);
    lv_label_set_text_fmt(s_pct, "%d%%", pct);
    lv_obj_align(s_pct, LV_ALIGN_TOP_MID, 0, 46 + 70 - 12);
}

void ui_anim_set_state(app_state_t s)
{
    s_state = s;
    if (s != ST_WIFI_PROVISIONING) s_qr_ready = false;
    if (s == ST_LISTENING) ui_anim_clear_text();          // a new turn: forget the last one
    if (s == ST_IDLE && s_text_visible) {                 // keep the text readable for a while, then back to the Orb
        lv_timer_reset(s_text_timer);
        lv_timer_resume(s_text_timer);
    } else if (s != ST_IDLE) {
        lv_timer_pause(s_text_timer);
    }

    // mood: where the eyes look and how wide they are
    uint32_t now = lv_tick_get();
    switch (s) {
    case ST_BOOT:                s_mood = M_BOOT;   s_base_open = 1.0f;  break;
    case ST_WIFI_CONNECTING:
    case ST_BRIDGE_CONNECTING:   s_mood = M_SCAN;   s_base_open = 1.0f;  break;
    case ST_LISTENING:           s_mood = M_LISTEN; s_base_open = 1.08f; break;   // wide awake
    case ST_THINKING:            s_mood = M_THINK;  s_base_open = 0.92f; break;
    case ST_SPEAKING:            s_mood = M_SPEAK;  s_base_open = 1.0f;  break;
    case ST_ERROR:               s_mood = M_ERROR;  s_base_open = 0.62f; s_shake_t0 = now; break;   // narrowed and shaking
    default:                     s_mood = M_IDLE;   s_base_open = 1.0f;  break;
    }
    s_next_move = 0;                                      // pick a new target straight away
    if (s == ST_BOOT || s == ST_IDLE) { s_blink_phase = 3; s_blink_t0 = now; }   // wake up: open from closed

    ui_caption_t cap = UI_CAP_NONE;
    if (s == ST_WIFI_CONNECTING || s == ST_BRIDGE_CONNECTING) cap = UI_CAP_CONNECTING;
    if (s == ST_OTA_UPDATING) cap = UI_CAP_UPDATING;
    if (s != ST_WIFI_PROVISIONING) ui_anim_set_caption(cap);   // setup keeps the caption chosen by show_qr
    else apply_scene();
    if (s == ST_OTA_UPDATING) ui_anim_set_ota_progress(0);
}

void ui_anim_set_battery(int level, bool charging, bool present)
{
    if (!present) {
        lv_label_set_text(s_batt_lbl, LV_SYMBOL_USB " USB");
        lv_obj_set_style_text_color(s_batt_lbl, lv_color_hex(COL_TEXT_DIM), 0);
    } else {
        const char *sym = level >= 88 ? LV_SYMBOL_BATTERY_FULL : level >= 63 ? LV_SYMBOL_BATTERY_3 :
                          level >= 38 ? LV_SYMBOL_BATTERY_2 : level >= 13 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
        lv_label_set_text_fmt(s_batt_lbl, "%s%s %d%%", charging ? LV_SYMBOL_CHARGE : "", sym, level);
        uint32_t col = charging ? 0x58D8FF : level > 50 ? 0x2EE6A8 : level > 20 ? 0xFFD93A : 0xFF4B4B;
        lv_obj_set_style_text_color(s_batt_lbl, lv_color_hex(col), 0);
    }
    lv_obj_align_to(s_batt_lbl, s_time_lbl, LV_ALIGN_OUT_LEFT_MID, -8, 0);
}
