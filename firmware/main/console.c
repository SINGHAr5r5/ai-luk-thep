#include "console.h"
#include <stdio.h>
#include <string.h>
#include "esp_console.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "settings.h"
#include "wifi_mgr.h"
#include "ota_mgr.h"
#include <stdlib.h>
#include "audio_test.h"
#include "voice.h"
#include "img_text.h"
#include "ui.h"
#include "battery.h"
#include "app_state.h"
#include "esp_heap_caps.h"
#include "audio_in.h"
#include "audio_out.h"

static int cmd_info(int argc, char **argv)
{
    printf("version   : %s\npartition : %s\nip        : %s\nrolled back: %s\n", ota_mgr_running_version(),
           esp_ota_get_running_partition()->label, wifi_mgr_ip_str()[0] ? wifi_mgr_ip_str() : "-",
           ota_mgr_was_rolled_back() ? "yes" : "no");
    return 0;
}

static int cmd_token(int argc, char **argv)
{
    char tok[64];
    if (settings_get(SETTING_TOKEN, tok, sizeof(tok)) != ESP_OK) { printf("no token\n"); return 1; }
    printf("OTA_TOKEN=%s\n", tok);
    return 0;
}

static int cmd_wifi_forget(int argc, char **argv)
{
    wifi_mgr_clear_credentials();
    printf("Wi-Fi credentials erased, rebooting\n");
    esp_restart();
    return 0;
}

static int cmd_beep(int argc, char **argv)
{
    audio_test_beep(argc > 1 ? atoi(argv[1]) : 1000, argc > 2 ? atoi(argv[2]) : 800);
    return 0;
}
static int cmd_selftest(int argc, char **argv) { audio_test_selftest(); return 0; }
static int cmd_levels(int argc, char **argv) { audio_test_levels(); return 0; }
static int cmd_vol(int argc, char **argv)
{
    if (argc > 1) audio_out_set_volume((uint8_t)atoi(argv[1]));
    return 0;
}
static int cmd_gain(int argc, char **argv)
{
    if (argc > 1) audio_in_set_gain_db((float)atof(argv[1]));
    return 0;
}

static int cmd_bridge(int argc, char **argv)
{
    if (argc > 1) { settings_set(SETTING_BRIDGE_URL, argv[1]); printf("bridge url saved\n"); }
    char url[160] = "-";
    settings_get(SETTING_BRIDGE_URL, url, sizeof(url));
    printf("bridge url  : %s\nbridge token: %s\n", url, settings_has(SETTING_BRIDGE_TOKEN) ? "set" : "NOT set");
    return 0;
}
static int cmd_bridge_token(int argc, char **argv)
{
    if (argc < 2) { printf("usage: bridge_token <token>\n"); return 1; }
    settings_set(SETTING_BRIDGE_TOKEN, argv[1]);
    printf("bridge token saved (%d chars); reboot to connect\n", (int)strlen(argv[1]));
    return 0;
}

static int cmd_ptt(int argc, char **argv)
{
    voice_test_ptt(argc > 1 ? atoi(argv[1]) : 1500);
    return 0;
}

static int cmd_textdemo(int argc, char **argv)
{
    const lv_image_dsc_t *src[2] = {&img_scan_setup, &img_testing};
    const char *role[2] = {"stt", "reply"};
    for (int i = 0; i < 2; i++) {
        size_t n = src[i]->data_size;
        uint8_t *b = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
        if (!b) return 1;
        memcpy(b, src[i]->data, n);
        ui_show_text(role[i], src[i]->header.w, src[i]->header.h, b);
    }
    printf("showing two sample lines; they go away 12 s after the board is idle\n");
    return 0;
}

static int cmd_ui(int argc, char **argv)
{
    static const struct { const char *name; app_state_t st; } m[] = {
        {"boot", ST_BOOT}, {"connecting", ST_BRIDGE_CONNECTING}, {"idle", ST_IDLE}, {"listening", ST_LISTENING},
        {"thinking", ST_THINKING}, {"speaking", ST_SPEAKING}, {"ota", ST_OTA_UPDATING}, {"error", ST_ERROR},
    };
    if (argc < 2) { printf("usage: ui boot|connecting|idle|listening|thinking|speaking|ota|error\n"); return 1; }
    for (size_t i = 0; i < sizeof(m) / sizeof(m[0]); i++) {
        if (!strcmp(argv[1], m[i].name)) {
            if (m[i].st == ST_IDLE) app_enter_idle(); else app_set_state(m[i].st);
            if (m[i].st == ST_OTA_UPDATING) ui_set_ota_progress(argc > 2 ? atoi(argv[2]) : 62);
            return 0;
        }
    }
    printf("unknown state\n");
    return 1;
}

static int cmd_battery(int argc, char **argv)
{
    printf("battery: raw %d -> %d%%  charging=%d present=%d\n", battery_raw(), battery_level(), battery_charging(), battery_present());
    return 0;
}

static int cmd_reboot(int argc, char **argv)
{
    esp_restart();
    return 0;
}

void console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t rc = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    rc.prompt = "luk-thep> ";
    esp_console_dev_usb_serial_jtag_config_t hw = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw, &rc, &repl));
    esp_console_register_help_command();
    const esp_console_cmd_t cmds[] = {
        {.command = "info", .help = "firmware version, partition, IP", .func = cmd_info},
        {.command = "token", .help = "print the OTA upload token", .func = cmd_token},
        {.command = "wifi_forget", .help = "erase saved Wi-Fi and reboot", .func = cmd_wifi_forget},
        {.command = "reboot", .help = "restart", .func = cmd_reboot},
        {.command = "battery", .help = "battery ADC reading, level and charge state", .func = cmd_battery},
        {.command = "ui", .help = "ui <state> - preview a screen state (idle, listening, ... ota 40)", .func = cmd_ui},
        {.command = "textdemo", .help = "show sample text on the screen (layout test)", .func = cmd_textdemo},
        {.command = "ptt", .help = "ptt [ms] - simulate holding BOOT to talk", .func = cmd_ptt},
        {.command = "bridge", .help = "bridge [ws://ip:8765/ws] - show/set the bridge URL", .func = cmd_bridge},
        {.command = "bridge_token", .help = "bridge_token <t> - set the bridge device token", .func = cmd_bridge_token},
        {.command = "beep", .help = "beep [hz] [ms] - play a tone", .func = cmd_beep},
        {.command = "selftest", .help = "speaker plays 1 kHz, mic must hear it", .func = cmd_selftest},
        {.command = "levels", .help = "2 s microphone level meter", .func = cmd_levels},
        {.command = "vol", .help = "vol <0-100> speaker volume", .func = cmd_vol},
        {.command = "gain", .help = "gain <0-42> microphone gain in dB", .func = cmd_gain},
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
