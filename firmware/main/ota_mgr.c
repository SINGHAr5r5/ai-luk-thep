#include "ota_mgr.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "settings.h"
#include "app_state.h"
#include "ui.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "psa/crypto.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ota_mgr";

#define VERIFY_DEADLINE_US (60LL * 1000000)
#define TOKEN_MIN_LEN      16
#define CHUNK              4096

static esp_timer_handle_t s_verify_timer;
static bool s_pending;
static bool s_rolled_back;
static httpd_handle_t s_httpd;
static void (*s_progress_cb)(int) = NULL;
static volatile bool s_busy;

static void verify_deadline(void *arg)
{
    ESP_LOGE(TAG, "new firmware was not confirmed healthy within 60 s -> rolling back");
    esp_ota_mark_app_invalid_rollback_and_reboot();
}

const char *ota_mgr_running_version(void)
{
    return esp_app_get_description()->version;
}

bool ota_mgr_was_rolled_back(void) { return s_rolled_back; }
void ota_mgr_on_progress(void (*cb)(int pct)) { s_progress_cb = cb; }

esp_err_t ota_mgr_init(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        s_pending = true;
        ESP_LOGW(TAG, "running a new OTA image (%s %s): must be confirmed within 60 s",
                 run->label, ota_mgr_running_version());
        const esp_timer_create_args_t a = {.callback = verify_deadline, .name = "ota_verify"};
        ESP_ERROR_CHECK(esp_timer_create(&a, &s_verify_timer));
        ESP_ERROR_CHECK(esp_timer_start_once(s_verify_timer, VERIFY_DEADLINE_US));
    }
    const esp_partition_t *bad = esp_ota_get_last_invalid_partition();
    if (bad) {
        s_rolled_back = true;
        ESP_LOGW(TAG, "a previous image in %s was rejected and rolled back", bad->label);
    }

    // The upload token lives in NVS only; it is created once and read out over USB (console `token`).
    char tok[64];
    if (settings_get(SETTING_TOKEN, tok, sizeof(tok)) != ESP_OK || strlen(tok) < TOKEN_MIN_LEN) {
        uint8_t r[16];
        esp_fill_random(r, sizeof(r));
        for (int i = 0; i < 16; i++) snprintf(tok + i * 2, 3, "%02x", r[i]);
        settings_set(SETTING_TOKEN, tok);
        ESP_LOGI(TAG, "generated a new OTA token (read it with the `token` console command)");
    }
    return ESP_OK;
}

void ota_mgr_mark_valid_if_healthy(void)
{
    if (!s_pending) return;
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        s_pending = false;
        if (s_verify_timer) { esp_timer_stop(s_verify_timer); esp_timer_delete(s_verify_timer); s_verify_timer = NULL; }
        ESP_LOGI(TAG, "new firmware confirmed valid, rollback cancelled");
    }
}

esp_err_t ota_mgr_check_and_pull(const char *manifest_url)
{
    ESP_LOGW(TAG, "pull OTA arrives in Phase 8 (%s)", manifest_url ? manifest_url : "");
    return ESP_ERR_NOT_SUPPORTED;
}

// ------------------------------------------------------------------ HTTP
static bool token_ok(httpd_req_t *r)
{
    char want[64] = "", got[96] = "";
    if (settings_get(SETTING_TOKEN, want, sizeof(want)) != ESP_OK || strlen(want) < TOKEN_MIN_LEN) return false;
    if (httpd_req_get_hdr_value_str(r, "X-OTA-Token", got, sizeof(got)) != ESP_OK) return false;
    size_t lw = strlen(want), lg = strlen(got);
    unsigned diff = lw ^ lg;                      // constant-time compare
    for (size_t i = 0; i < lw; i++) diff |= (unsigned char)want[i] ^ (unsigned char)(i < lg ? got[i] : 0);
    return diff == 0;
}

static void hex(const uint8_t *in, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++) snprintf(out + i * 2, 3, "%02x", in[i]);
}

static void report(int pct)
{
    static int last = -1;
    if (pct == last) return;
    last = pct;
    ui_set_ota_progress(pct);
    if (s_progress_cb) s_progress_cb(pct);
}

static esp_err_t fail(httpd_req_t *r, const char *status, const char *msg)
{
    ESP_LOGE(TAG, "OTA refused/failed: %s", msg);
    httpd_resp_set_status(r, status);
    httpd_resp_set_type(r, "application/json");
    char body[160];
    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", msg);
    httpd_resp_sendstr(r, body);
    return ESP_FAIL;
}

static esp_err_t h_ota(httpd_req_t *r)
{
    if (!token_ok(r)) return fail(r, "401 Unauthorized", "bad token");
    if (s_busy) return fail(r, "409 Conflict", "update already running");

    char want_sha[80] = "";
    if (httpd_req_get_hdr_value_str(r, "X-OTA-SHA256", want_sha, sizeof(want_sha)) != ESP_OK || strlen(want_sha) != 64)
        return fail(r, "400 Bad Request", "X-OTA-SHA256 header (64 hex chars) required");

    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    size_t total = r->content_len;
    if (!target || total < 4096 || total > target->size) return fail(r, "400 Bad Request", "bad image size");

    s_busy = true;
    app_state_t prev = app_get_state();
    app_set_state(ST_OTA_UPDATING);
    ESP_LOGI(TAG, "OTA start: %u bytes -> %s", (unsigned)total, target->label);

    uint8_t *buf = malloc(CHUNK);
    esp_ota_handle_t h = 0;
    psa_hash_operation_t hop = PSA_HASH_OPERATION_INIT;
    bool hashing = false;
    esp_err_t err = ESP_FAIL;
    const char *why = "internal error";

    if (!buf) { why = "out of memory"; goto done; }
    if (psa_crypto_init() != PSA_SUCCESS || psa_hash_setup(&hop, PSA_ALG_SHA_256) != PSA_SUCCESS) { why = "hash init failed"; goto done; }
    hashing = true;
    if (esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &h) != ESP_OK) { why = "esp_ota_begin failed"; goto done; }

    size_t got = 0;
    while (got < total) {
        int n = httpd_req_recv(r, (char *)buf, (total - got) > CHUNK ? CHUNK : (total - got));
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) { why = "connection lost during upload"; esp_ota_abort(h); h = 0; goto done; }
        if (esp_ota_write(h, buf, n) != ESP_OK) { why = "flash write failed"; esp_ota_abort(h); h = 0; goto done; }
        psa_hash_update(&hop, buf, n);
        got += n;
        report((int)(got * 100 / total));
    }

    uint8_t digest[32];
    size_t dlen = 0;
    psa_hash_finish(&hop, digest, sizeof(digest), &dlen);
    hashing = false;
    char have_sha[65];
    hex(digest, 32, have_sha);
    bool match = true;
    for (int i = 0; i < 64; i++) match &= (have_sha[i] == (want_sha[i] | 0x20));   // case-insensitive
    if (!match) { why = "SHA-256 mismatch"; esp_ota_abort(h); h = 0; goto done; }

    if (esp_ota_end(h) != ESP_OK) { why = "image validation failed"; h = 0; goto done; }   // checks header + embedded hash
    h = 0;
    if (esp_ota_set_boot_partition(target) != ESP_OK) { why = "set_boot_partition failed"; goto done; }
    err = ESP_OK;

done:
    if (hashing) psa_hash_abort(&hop);
    free(buf);
    if (err != ESP_OK) {
        if (h) esp_ota_abort(h);
        s_busy = false;
        app_set_state(prev);
        return fail(r, "500 Internal Server Error", why);
    }
    ESP_LOGI(TAG, "OTA ok (%s), rebooting into %s", have_sha, target->label);
    ui_set_caption(UI_CAP_UPDATE_DONE);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_sendstr(r, "{\"ok\":true,\"reboot\":true}");
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t h_info(httpd_req_t *r)
{
    char body[200];
    snprintf(body, sizeof(body), "{\"name\":\"%s\",\"version\":\"%s\",\"partition\":\"%s\",\"pending\":%s,\"rolled_back\":%s}",
             esp_app_get_description()->project_name, ota_mgr_running_version(),
             esp_ota_get_running_partition()->label, s_pending ? "true" : "false", s_rolled_back ? "true" : "false");
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_sendstr(r, body);
}

esp_err_t ota_mgr_start_push_server(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.recv_wait_timeout = 15;
    cfg.max_uri_handlers = 4;
    esp_err_t err = httpd_start(&s_httpd, &cfg);
    if (err != ESP_OK) return err;
    const httpd_uri_t ota = {.uri = "/ota", .method = HTTP_POST, .handler = h_ota};
    const httpd_uri_t info = {.uri = "/info", .method = HTTP_GET, .handler = h_info};
    httpd_register_uri_handler(s_httpd, &ota);
    httpd_register_uri_handler(s_httpd, &info);
    ESP_LOGI(TAG, "push OTA ready: POST /ota, GET /info");
    return ESP_OK;
}
