// The "Orb": a glowing sphere in the middle of the screen, drawn from stacked circles.
// Phase 1 implements the IDLE breathing; the other states only change colour for now
// (their real animations arrive in Phase 7). Phase 2 adds the QR scene used while the
// board waits for Wi-Fi to be set up.
#include "ui_anim.h"
#include <string.h>
#include "img_text.h"

#define LAYERS 5

static lv_obj_t *s_layer[LAYERS];
static lv_obj_t *s_highlight;
static lv_obj_t *s_status;
static lv_obj_t *s_title;
static lv_obj_t *s_qr;
static lv_obj_t *s_caption;
static lv_obj_t *s_arc;       // OTA progress ring
static lv_obj_t *s_pct;
static lv_obj_t *s_frame;     // blinking border while waiting for Wi-Fi setup
static lv_anim_t s_breath, s_blink;
static app_state_t s_state = ST_BOOT;

// radius at rest, and how much each layer grows at the peak of a breath
static const int16_t BASE_R[LAYERS] = {100, 84, 68, 54, 42};
static const int16_t GROW_R[LAYERS] = {  6,  8, 10, 11, 12};
static const lv_opa_t OPA[LAYERS]   = { 25, 45, 75, 140, 255};

#define ORB_CY 138

static lv_color_t state_color(app_state_t s)
{
    switch (s) {
    case ST_BOOT:                return lv_color_hex(0xFFFFFF);
    case ST_WIFI_PROVISIONING:   return lv_color_hex(0x3AA0FF);
    case ST_WIFI_CONNECTING:
    case ST_BRIDGE_CONNECTING:   return lv_color_hex(0x7FD0FF);
    case ST_IDLE:                return lv_color_hex(0x5B5BFF);  // blue-violet
    case ST_LISTENING:           return lv_color_hex(0x2EE6A8);  // mint
    case ST_THINKING:            return lv_color_hex(0xB45CFF);  // violet/pink
    case ST_SPEAKING:            return lv_color_hex(0xFFA630);  // gold
    case ST_OTA_UPDATING:        return lv_color_hex(0xFFD93A);  // yellow
    case ST_ERROR:               return lv_color_hex(0xFF3B3B);  // red
    }
    return lv_color_hex(0x5B5BFF);
}

static const char *state_text(app_state_t s)
{
    switch (s) {
    case ST_BOOT:                return "booting";
    case ST_WIFI_CONNECTING:     return "wifi...";
    case ST_WIFI_PROVISIONING:   return "setup";
    case ST_BRIDGE_CONNECTING:   return "connecting";
    case ST_IDLE:                return "ready";
    case ST_LISTENING:           return "listening";
    case ST_THINKING:            return "thinking";
    case ST_SPEAKING:            return "speaking";
    case ST_OTA_UPDATING:        return "updating";
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

static void breath_cb(void *var, int32_t v)  // v: 0..1000
{
    (void)var;
    for (int i = 0; i < LAYERS; i++) {
        int32_t r = BASE_R[i] + GROW_R[i] * v / 1000;
        lv_obj_set_size(s_layer[i], r * 2, r * 2);
        lv_obj_align(s_layer[i], LV_ALIGN_TOP_MID, 0, ORB_CY - r);
    }
    int32_t hr = 14 + 3 * v / 1000;
    lv_obj_set_size(s_highlight, hr * 2, hr * 2);
    lv_obj_align(s_highlight, LV_ALIGN_TOP_MID, -14, ORB_CY - 24 - hr);
}

static void blink_cb(void *var, int32_t v)  // v: 40..255
{
    (void)var;
    lv_obj_set_style_border_opa(s_frame, (lv_opa_t)v, 0);
}

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

static void set_orb_visible(bool on)
{
    for (int i = 0; i < LAYERS; i++) lv_obj_set_hidden(s_layer[i], !on);
    lv_obj_set_hidden(s_highlight, !on);
}

void ui_anim_create(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    s_title = lv_image_create(scr);
    lv_image_set_src(s_title, &img_title);
    lv_obj_set_style_image_recolor(s_title, lv_color_hex(0x7FE7FF), 0);
    lv_obj_set_style_image_recolor_opa(s_title, LV_OPA_COVER, 0);
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 6);

    lv_color_t base = state_color(ST_IDLE);
    for (int i = 0; i < LAYERS; i++) s_layer[i] = circle(base, OPA[i]);
    s_highlight = circle(lv_color_white(), 70);

    s_qr = lv_qrcode_create(scr);
    lv_qrcode_set_size(s_qr, 156);
    lv_qrcode_set_dark_color(s_qr, lv_color_black());
    lv_qrcode_set_light_color(s_qr, lv_color_white());
    lv_qrcode_set_quiet_zone(s_qr, true);
    lv_obj_align(s_qr, LV_ALIGN_TOP_MID, 0, 46);
    lv_obj_set_hidden(s_qr, true);

    s_arc = lv_arc_create(scr);
    lv_obj_set_size(s_arc, 150, 150);
    lv_obj_align(s_arc, LV_ALIGN_TOP_MID, 0, ORB_CY - 75);
    lv_arc_set_rotation(s_arc, 270);
    lv_arc_set_bg_angles(s_arc, 0, 360);
    lv_arc_set_range(s_arc, 0, 100);
    lv_arc_set_value(s_arc, 0);
    lv_obj_remove_style(s_arc, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(s_arc, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 12, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(0x2A2A14), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(0xFFD93A), LV_PART_INDICATOR);
    lv_obj_set_clickable(s_arc, false);
    lv_obj_set_hidden(s_arc, true);

    s_pct = lv_label_create(scr);
    lv_obj_set_style_text_font(s_pct, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_pct, lv_color_hex(0xFFD93A), 0);
    lv_label_set_text(s_pct, "0%");
    lv_obj_align(s_pct, LV_ALIGN_TOP_MID, 0, ORB_CY - 12);
    lv_obj_set_hidden(s_pct, true);

    s_status = lv_label_create(scr);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_status, lv_color_hex(0x8890A0), 0);
    lv_obj_align(s_status, LV_ALIGN_BOTTOM_MID, 0, -6);

    s_caption = lv_image_create(scr);
    lv_obj_set_style_image_recolor_opa(s_caption, LV_OPA_COVER, 0);
    lv_obj_align(s_caption, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_hidden(s_caption, true);

    s_frame = lv_obj_create(scr);
    lv_obj_remove_style_all(s_frame);
    lv_obj_set_size(s_frame, 240, 240);
    lv_obj_set_style_border_width(s_frame, 4, 0);
    lv_obj_set_style_border_color(s_frame, lv_color_hex(0x3AA0FF), 0);
    lv_obj_set_style_radius(s_frame, 0, 0);
    lv_obj_set_clickable(s_frame, false);
    lv_obj_set_hidden(s_frame, true);

    breath_cb(NULL, 0);
    lv_anim_init(&s_breath);
    lv_anim_set_exec_cb(&s_breath, breath_cb);
    lv_anim_set_values(&s_breath, 0, 1000);
    lv_anim_set_duration(&s_breath, 1500);          // 1.5 s in + 1.5 s out = 3 s per breath
    lv_anim_set_reverse_duration(&s_breath, 1500);
    lv_anim_set_path_cb(&s_breath, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&s_breath, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&s_breath);

    lv_anim_init(&s_blink);                           // slow border pulse for the setup scene
    lv_anim_set_exec_cb(&s_blink, blink_cb);
    lv_anim_set_values(&s_blink, 40, 255);
    lv_anim_set_duration(&s_blink, 1200);
    lv_anim_set_reverse_duration(&s_blink, 1200);
    lv_anim_set_path_cb(&s_blink, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&s_blink, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&s_blink);

    ui_anim_set_state(ST_BOOT);
}

void ui_anim_set_caption(ui_caption_t cap)
{
    uint32_t color;
    const lv_image_dsc_t *img = caption_img(cap, &color);
    if (img) {
        lv_image_set_src(s_caption, img);
        lv_obj_set_style_image_recolor(s_caption, lv_color_hex(color), 0);
        lv_obj_align(s_caption, LV_ALIGN_BOTTOM_MID, 0, -4);
    }
    lv_obj_set_hidden(s_caption, img == NULL);
    lv_obj_set_hidden(s_status, img != NULL);
}

void ui_anim_set_status(const char *text)
{
    lv_label_set_text(s_status, text);
    lv_obj_align(s_status, LV_ALIGN_BOTTOM_MID, 0, -6);
}

void ui_anim_show_qr(const char *payload, ui_caption_t cap)
{
    lv_qrcode_update(s_qr, payload, strlen(payload));
    lv_obj_align(s_qr, LV_ALIGN_TOP_MID, 0, 46);
    lv_obj_set_hidden(s_qr, false);
    set_orb_visible(false);
    lv_obj_set_hidden(s_frame, false);
    ui_anim_set_caption(cap);
}

void ui_anim_set_ota_progress(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    lv_arc_set_value(s_arc, pct);
    lv_label_set_text_fmt(s_pct, "%d%%", pct);
    lv_obj_align(s_pct, LV_ALIGN_TOP_MID, 0, ORB_CY - 12);
}

void ui_anim_set_state(app_state_t s)
{
    s_state = s;
    lv_color_t c = state_color(s);
    for (int i = 0; i < LAYERS; i++) lv_obj_set_style_bg_color(s_layer[i], c, 0);

    bool setup = (s == ST_WIFI_PROVISIONING);
    lv_obj_set_hidden(s_qr, !setup || lv_obj_is_hidden(s_qr));
    lv_obj_set_hidden(s_frame, !setup);
    bool ota = (s == ST_OTA_UPDATING);
    set_orb_visible(!setup && !ota);
    lv_obj_set_hidden(s_arc, !ota);
    lv_obj_set_hidden(s_pct, !ota);
    if (ota) ui_anim_set_ota_progress(0);

    ui_caption_t cap = UI_CAP_NONE;
    if (s == ST_WIFI_CONNECTING || s == ST_BRIDGE_CONNECTING) cap = UI_CAP_CONNECTING;
    if (ota) cap = UI_CAP_UPDATING;
    if (!setup) ui_anim_set_caption(cap);
    ui_anim_set_status(state_text(s));
}
