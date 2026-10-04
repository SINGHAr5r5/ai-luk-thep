// Wi-Fi manager: connect with saved credentials, otherwise run a SoftAP + captive portal
// whose join/setup links are shown as QR codes on the LCD.
#include "wifi_mgr.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "settings.h"
#include "ui.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_mac.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "mdns.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

static const char *TAG = "wifi_mgr";

#define BIT_CONNECTED BIT0
#define BIT_FAILED    BIT1
#define MAX_RETRY     3
#define AP_IP         "192.168.4.1"
#define MAX_SCAN      24

static EventGroupHandle_t s_events;
static esp_netif_t *s_sta_netif, *s_ap_netif;
static bool s_connecting;       // inside an explicit connect attempt
static bool s_want_reconnect;   // keep reconnecting after a drop (normal operation)
static int  s_retry;
static int  s_last_reason;
static int64_t s_down_since_us;  // 0 while connected
static char s_ip[16];

static bool s_prov_active;
static int  s_ap_clients;
static char s_ap_ssid[32], s_ap_pass[9];
static httpd_handle_t s_httpd;
static TaskHandle_t s_dns_task;
static int s_dns_sock = -1;

// scan results cached before the AP starts (scanning later would hurt the AP clients)
static char s_scan_json[1536] = "[]";

// connection test started from the web page
static volatile int s_test_state;  // 0 idle, 1 testing, 2 ok, 3 failed
static char s_test_msg[192];  // Thai text: 3 bytes per character

// ------------------------------------------------------------------ events
static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *d = data;
            s_last_reason = d->reason;
            if (!s_down_since_us) s_down_since_us = esp_timer_get_time();
            if (s_connecting) {
                if (++s_retry < MAX_RETRY) {
                    ESP_LOGW(TAG, "connect failed (reason %d), retry %d/%d", d->reason, s_retry, MAX_RETRY);
                    esp_wifi_connect();
                } else {
                    xEventGroupSetBits(s_events, BIT_FAILED);
                }
            } else if (s_want_reconnect) {
                ESP_LOGW(TAG, "link lost (reason %d), reconnecting", d->reason);
                esp_wifi_connect();
            }
            break;
        }
        case WIFI_EVENT_AP_STACONNECTED:
            s_ap_clients++;
            ESP_LOGI(TAG, "phone joined the setup AP (%d)", s_ap_clients);
            if (s_prov_active) ui_show_qr("http://" AP_IP "/", UI_CAP_SCAN_SETUP);
            break;
        case WIFI_EVENT_AP_STADISCONNECTED:
            if (s_ap_clients > 0) s_ap_clients--;
            if (s_prov_active && s_ap_clients == 0) {
                char payload[96];
                snprintf(payload, sizeof(payload), "WIFI:T:WPA;S:%s;P:%s;;", s_ap_ssid, s_ap_pass);
                ui_show_qr(payload, UI_CAP_SCAN_AP);
            }
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_down_since_us = 0;
        ESP_LOGI(TAG, "got IP %s", s_ip);
        xEventGroupSetBits(s_events, BIT_CONNECTED);
    }
}

esp_err_t wifi_mgr_init(void)
{
    s_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(s_sta_netif, "ai-luk-thep");

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));  // credentials live in our own NVS keys
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL));
    return ESP_OK;
}

// ------------------------------------------------------------------ station
static esp_err_t sta_connect(const char *ssid, const char *pass, uint32_t timeout_ms)
{
    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));

    xEventGroupClearBits(s_events, BIT_CONNECTED | BIT_FAILED);
    s_retry = 0;
    s_last_reason = 0;
    s_connecting = true;
    esp_wifi_disconnect();
    esp_wifi_connect();
    EventBits_t bits = xEventGroupWaitBits(s_events, BIT_CONNECTED | BIT_FAILED, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));
    s_connecting = false;
    if (bits & BIT_CONNECTED) return ESP_OK;
    esp_wifi_disconnect();
    return (bits & BIT_FAILED) ? ESP_FAIL : ESP_ERR_TIMEOUT;
}

bool wifi_mgr_has_credentials(void)
{
    return settings_has(SETTING_WIFI_SSID);
}

esp_err_t wifi_mgr_connect_saved(uint32_t timeout_ms)
{
    char ssid[33] = "", pass[65] = "";
    if (settings_get(SETTING_WIFI_SSID, ssid, sizeof(ssid)) != ESP_OK) return ESP_ERR_NOT_FOUND;
    settings_get(SETTING_WIFI_PASS, pass, sizeof(pass));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "connecting to \"%s\"", ssid);
    esp_err_t err = sta_connect(ssid, pass, timeout_ms);
    if (err == ESP_OK) {
        s_want_reconnect = true;
    } else {
        ESP_LOGW(TAG, "could not connect (last reason %d)", s_last_reason);
        esp_wifi_stop();
    }
    return err;
}

bool wifi_mgr_is_connected(void) { return s_ip[0] && s_down_since_us == 0; }
const char *wifi_mgr_ip_str(void) { return wifi_mgr_is_connected() ? s_ip : ""; }
uint32_t wifi_mgr_down_seconds(void)
{
    if (!s_down_since_us) return 0;
    return (uint32_t)((esp_timer_get_time() - s_down_since_us) / 1000000);
}

esp_err_t wifi_mgr_save_credentials(const char *ssid, const char *pass)
{
    esp_err_t err = settings_set(SETTING_WIFI_SSID, ssid);
    if (err == ESP_OK) err = settings_set(SETTING_WIFI_PASS, pass ? pass : "");
    return err;
}

void wifi_mgr_clear_credentials(void)
{
    settings_erase(SETTING_WIFI_SSID);
    settings_erase(SETTING_WIFI_PASS);
}

esp_err_t wifi_mgr_scan(wifi_ap_record_t *out, uint16_t *count)
{
    wifi_scan_config_t sc = {.show_hidden = false};
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (err != ESP_OK) return err;
    return esp_wifi_scan_get_ap_records(count, out);
}

void wifi_mgr_start_mdns(const char *hostname)
{
    if (mdns_init() != ESP_OK) return;
    mdns_hostname_set(hostname);
    mdns_instance_name_set("AI Luk Thep");
    ESP_LOGI(TAG, "mDNS: %s.local", hostname);
}

// ------------------------------------------------------------------ helpers
static void json_escape(const char *in, char *out, size_t out_len)
{
    size_t o = 0;
    for (; *in && o + 7 < out_len; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = c; }
        else if (c < 0x20) { o += snprintf(out + o, out_len - o, "\\u%04x", c); }
        else out[o++] = c;
    }
    out[o] = 0;
}

static void url_decode(char *s)
{
    char *w = s;
    for (; *s; s++) {
        if (*s == '+') *w++ = ' ';
        else if (*s == '%' && s[1] && s[2]) {
            char hex[3] = {s[1], s[2], 0};
            *w++ = (char)strtol(hex, NULL, 16);
            s += 2;
        } else *w++ = *s;
    }
    *w = 0;
}

static void build_scan_json(void)
{
    wifi_ap_record_t recs[MAX_SCAN];
    uint16_t n = MAX_SCAN;
    if (wifi_mgr_scan(recs, &n) != ESP_OK) n = 0;
    size_t o = 0;
    o += snprintf(s_scan_json, sizeof(s_scan_json), "[");
    int emitted = 0;
    for (int i = 0; i < n && o + 120 < sizeof(s_scan_json); i++) {
        const char *ssid = (const char *)recs[i].ssid;
        if (!ssid[0]) continue;
        bool dup = false;
        for (int j = 0; j < i; j++) if (!strcmp((const char *)recs[j].ssid, ssid)) dup = true;
        if (dup) continue;
        char esc[100];
        json_escape(ssid, esc, sizeof(esc));
        o += snprintf(s_scan_json + o, sizeof(s_scan_json) - o, "%s{\"s\":\"%s\",\"r\":%d,\"l\":%d}",
                      emitted++ ? "," : "", esc, recs[i].rssi, recs[i].authmode != WIFI_AUTH_OPEN);
    }
    snprintf(s_scan_json + o, sizeof(s_scan_json) - o, "]");
    ESP_LOGI(TAG, "scan found %d networks", emitted);
}

// ------------------------------------------------------------------ captive-portal DNS
static void dns_task(void *arg)
{
    s_dns_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (s_dns_sock < 0 || bind(s_dns_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "DNS bind failed");
        if (s_dns_sock >= 0) close(s_dns_sock);
        s_dns_sock = -1;
        s_dns_task = NULL;
        vTaskDelete(NULL);
    }
    uint8_t buf[256];
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(s_dns_sock, buf, sizeof(buf) - 16, 0, (struct sockaddr *)&from, &fl);
        if (n < 0) break;          // socket closed by stop_provisioning
        if (n < 12) continue;
        // find end of the question (single question expected)
        int p = 12;
        while (p < n && buf[p]) p += buf[p] + 1;
        p += 5;                    // zero label + QTYPE + QCLASS
        if (p > n) continue;
        uint16_t qtype = (buf[p - 4] << 8) | buf[p - 3];
        buf[2] = 0x81; buf[3] = 0x80;          // response, recursion available
        buf[6] = 0; buf[7] = (qtype == 1) ? 1 : 0;  // answer count: only for A queries
        buf[8] = buf[9] = buf[10] = buf[11] = 0;
        int len = p;
        if (qtype == 1) {
            const uint8_t ans[] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4, 192, 168, 4, 1};
            memcpy(buf + len, ans, sizeof(ans));
            len += sizeof(ans);
        }
        sendto(s_dns_sock, buf, len, 0, (struct sockaddr *)&from, fl);
    }
    s_dns_task = NULL;
    vTaskDelete(NULL);
}

// ------------------------------------------------------------------ web page
static const char PAGE[] =
"<!doctype html><html lang=th><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'><title>AI ลูกเทพ</title><style>"
"body{font-family:-apple-system,sans-serif;max-width:440px;margin:0 auto;padding:20px;background:#0b0d1a;color:#eef}"
"h1{font-size:22px;margin:0 0 4px}p{color:#9aa}label{display:block;margin:14px 0 4px;font-size:14px;color:#9ab}"
"select,input,button{width:100%;box-sizing:border-box;font-size:17px;padding:12px;border-radius:10px;border:1px solid #334;"
"background:#151833;color:#eef}button{background:#5b5bff;border:0;margin-top:18px;font-weight:600}"
"button.s{background:#222645;margin-top:8px}#m{margin-top:16px;padding:12px;border-radius:10px;display:none}"
".ok{background:#12351f;color:#7dffb0}.er{background:#3a1520;color:#ff9bab}.wt{background:#2a2a14;color:#ffe27a}"
"</style></head><body><h1>AI ลูกเทพ</h1><p>เลือก Wi-Fi ที่ต้องการให้บอร์ดเชื่อมต่อ</p>"
"<label>Wi-Fi ที่พบ</label><select id=ss></select><button class=s type=button onclick=load()>ค้นหาใหม่</button>"
"<label>หรือพิมพ์ชื่อ Wi-Fi เอง</label><input id=cu autocapitalize=off>"
"<label>รหัสผ่าน Wi-Fi</label><input id=pw type=password>"
"<label>Bridge URL (ไม่บังคับ)</label><input id=br placeholder='ws://192.168.1.57:8765/ws' autocapitalize=off>"
"<label>Bridge token (ไม่บังคับ)</label><input id=bt type=password autocapitalize=off>"
"<button id=go type=button onclick=save()>ทดสอบและบันทึก</button><div id=m></div>"
"<script>"
"const $=i=>document.getElementById(i);"
"function msg(c,t){const m=$('m');m.className=c;m.textContent=t;m.style.display='block'}"
"async function load(){try{const r=await(await fetch('/scan')).json();$('ss').innerHTML=r.map(x=>"
"'<option>'+x.s.replace(/&/g,'&amp;').replace(/</g,'&lt;')+'</option>').join('')||'<option value=\"\">(ไม่พบ Wi-Fi)</option>'}catch(e){}}"
"async function save(){const ssid=$('cu').value||$('ss').value;if(!ssid){msg('er','กรุณาเลือกหรือพิมพ์ชื่อ Wi-Fi');return}"
"$('go').disabled=true;msg('wt','กำลังทดสอบการเชื่อมต่อ...');"
"const b='ssid='+encodeURIComponent(ssid)+'&pass='+encodeURIComponent($('pw').value)+'&bridge='+encodeURIComponent($('br').value)+'&btoken='+encodeURIComponent($('bt').value);"
"try{await fetch('/save',{method:'POST',body:b})}catch(e){}"
"let lost=0;const t=setInterval(async()=>{try{const s=await(await fetch('/status')).json();lost=0;"
"if(s.state==2){clearInterval(t);msg('ok','เชื่อมต่อสำเร็จ! บอร์ดกำลังรีสตาร์ท');}"
"else if(s.state==3){clearInterval(t);$('go').disabled=false;msg('er',s.msg)}}"
"catch(e){if(++lost>12){clearInterval(t);msg('ok','ขาดการเชื่อมต่อกับบอร์ด หากจอบอร์ดขึ้น \"เชื่อมต่อ Wi-Fi แล้ว\" แปลว่าสำเร็จ')}}},1000)}"
"load();"
"</script></body></html>";

static esp_err_t h_root(httpd_req_t *r)
{
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    return httpd_resp_send(r, PAGE, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_scan(httpd_req_t *r)
{
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_send(r, s_scan_json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_status(httpd_req_t *r)
{
    char out[480], esc[400];
    json_escape(s_test_msg, esc, sizeof(esc));
    snprintf(out, sizeof(out), "{\"state\":%d,\"msg\":\"%s\"}", s_test_state, esc);
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_send(r, out, HTTPD_RESP_USE_STRLEN);
}

typedef struct { char ssid[33], pass[65], bridge[128], btoken[96]; } test_args_t;

static void test_task(void *arg)
{
    test_args_t *a = arg;
    ui_set_caption(UI_CAP_TESTING);
    esp_err_t err = sta_connect(a->ssid, a->pass, 20000);
    if (err == ESP_OK) {
        wifi_mgr_save_credentials(a->ssid, a->pass);
        if (a->bridge[0]) settings_set(SETTING_BRIDGE_URL, a->bridge);
        if (a->btoken[0]) settings_set(SETTING_BRIDGE_TOKEN, a->btoken);
        s_test_state = 2;
        ui_set_caption(UI_CAP_CONNECTED);
        vTaskDelay(pdMS_TO_TICKS(4000));   // let the browser read the result
        esp_restart();
    }
    const char *why = "เชื่อมต่อไม่สำเร็จ ลองตรวจสอบชื่อและรหัสผ่านอีกครั้ง";
    if (s_last_reason == WIFI_REASON_NO_AP_FOUND) why = "ไม่พบ Wi-Fi นี้ (บอร์ดรองรับเฉพาะ 2.4 GHz)";
    else if (s_last_reason == WIFI_REASON_AUTH_FAIL || s_last_reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
             s_last_reason == WIFI_REASON_HANDSHAKE_TIMEOUT) why = "รหัสผ่านไม่ถูกต้อง";
    snprintf(s_test_msg, sizeof(s_test_msg), "%s", why);
    s_test_state = 3;
    ui_set_caption(UI_CAP_FAILED);
    vTaskDelay(pdMS_TO_TICKS(2500));
    if (s_prov_active) {
        if (s_ap_clients) {
            ui_show_qr("http://" AP_IP "/", UI_CAP_SCAN_SETUP);
        } else {
            char payload[96];
            snprintf(payload, sizeof(payload), "WIFI:T:WPA;S:%s;P:%s;;", s_ap_ssid, s_ap_pass);
            ui_show_qr(payload, UI_CAP_SCAN_AP);
        }
    }
    free(a);
    vTaskDelete(NULL);
}

static esp_err_t h_save(httpd_req_t *r)
{
    char body[700];
    int n = httpd_req_recv(r, body, sizeof(body) - 1);
    if (n <= 0 || s_test_state == 1) {
        httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "bad request");
        return ESP_FAIL;
    }
    body[n] = 0;
    test_args_t *a = calloc(1, sizeof(*a));
    char tmp[sizeof(a->bridge)] = "";
    if (httpd_query_key_value(body, "ssid", tmp, sizeof(a->ssid)) == ESP_OK) { url_decode(tmp); strlcpy(a->ssid, tmp, sizeof(a->ssid)); }
    tmp[0] = 0;
    if (httpd_query_key_value(body, "pass", tmp, sizeof(a->pass)) == ESP_OK) { url_decode(tmp); strlcpy(a->pass, tmp, sizeof(a->pass)); }
    tmp[0] = 0;
    if (httpd_query_key_value(body, "bridge", tmp, sizeof(a->bridge)) == ESP_OK) { url_decode(tmp); strlcpy(a->bridge, tmp, sizeof(a->bridge)); }
    tmp[0] = 0;
    if (httpd_query_key_value(body, "btoken", tmp, sizeof(a->btoken)) == ESP_OK) { url_decode(tmp); strlcpy(a->btoken, tmp, sizeof(a->btoken)); }
    if (!a->ssid[0]) {
        free(a);
        httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "ssid required");
        return ESP_FAIL;
    }
    s_test_state = 1;
    s_test_msg[0] = 0;
    httpd_resp_set_type(r, "application/json");
    httpd_resp_sendstr(r, "{\"ok\":true}");
    xTaskCreate(test_task, "wifi_test", 5120, a, 5, NULL);
    return ESP_OK;
}

// phones probe well-known URLs to detect a captive portal; any unknown URL -> setup page
static esp_err_t h_redirect(httpd_req_t *r, httpd_err_code_t err)
{
    httpd_resp_set_status(r, "302 Found");
    httpd_resp_set_hdr(r, "Location", "http://" AP_IP "/");
    httpd_resp_send(r, NULL, 0);
    return ESP_OK;
}

static void start_http(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 6144;
    cfg.lru_purge_enable = true;
    ESP_ERROR_CHECK(httpd_start(&s_httpd, &cfg));
    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = h_root},
        {.uri = "/scan", .method = HTTP_GET, .handler = h_scan},
        {.uri = "/status", .method = HTTP_GET, .handler = h_status},
        {.uri = "/save", .method = HTTP_POST, .handler = h_save},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) httpd_register_uri_handler(s_httpd, &routes[i]);
    httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, h_redirect);
}

// ------------------------------------------------------------------ provisioning
esp_err_t wifi_mgr_start_provisioning(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "AI-LukThep-%02X%02X", mac[4], mac[5]);
    snprintf(s_ap_pass, sizeof(s_ap_pass), "%08x", (unsigned)esp_random());

    s_want_reconnect = false;
    s_test_state = 0;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    build_scan_json();   // before the AP exists, so nobody is disturbed

    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, s_ap_ssid, sizeof(ap.ap.ssid));
    strlcpy((char *)ap.ap.password, s_ap_pass, sizeof(ap.ap.password));
    ap.ap.ssid_len = strlen(s_ap_ssid);
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));

    s_ap_clients = 0;
    s_prov_active = true;
    start_http();
    xTaskCreate(dns_task, "dns", 3072, NULL, 4, &s_dns_task);

    char payload[96];
    snprintf(payload, sizeof(payload), "WIFI:T:WPA;S:%s;P:%s;;", s_ap_ssid, s_ap_pass);
    ui_show_qr(payload, UI_CAP_SCAN_AP);
    ESP_LOGW(TAG, "provisioning AP \"%s\" password \"%s\"", s_ap_ssid, s_ap_pass);
    return ESP_OK;
}

void wifi_mgr_stop_provisioning(void)
{
    s_prov_active = false;
    if (s_httpd) { httpd_stop(s_httpd); s_httpd = NULL; }
    if (s_dns_sock >= 0) { close(s_dns_sock); s_dns_sock = -1; }
    esp_wifi_set_mode(WIFI_MODE_STA);
}
