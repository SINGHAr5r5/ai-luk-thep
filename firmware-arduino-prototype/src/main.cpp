// AI ลูกเทพ - SpotPear ESP32-S3 1.54 (sp-esp32-s3-1.54-muma)
//
// Wi-Fi setup by QR code (the board has no camera, so the phone scans the board):
//   1. No saved Wi-Fi -> board opens its own AP and shows a QR code to join it.
//   2. Once a phone has joined, the screen shows a second QR code for the setup page.
//   3. Pick the home Wi-Fi + password on the page -> saved to flash -> board reboots and connects.
// Hold BOOT for 3 s to forget the saved Wi-Fi. After Wi-Fi is up, ArduinoOTA is enabled.
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ArduinoOTA.h>
#include <LovyanGFX.hpp>
#include "secrets.h"
#include "thai_text.h"

constexpr int PIN_LED = 48;  // WS2812 status LED
constexpr int PIN_BL = 42;   // backlight, active low
constexpr int PIN_BOOT = 0;
constexpr const char *HOSTNAME = "ai-luk-thep";
constexpr uint32_t CONNECT_TIMEOUT_MS = 20000;
constexpr uint32_t RESET_HOLD_MS = 3000;

class Display : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 panel_;
  lgfx::Bus_SPI bus_;

 public:
  Display() {
    auto b = bus_.config();
    b.spi_host = SPI3_HOST;
    b.spi_mode = 0;
    b.freq_write = 40000000;
    b.pin_sclk = 4;
    b.pin_mosi = 2;
    b.pin_miso = -1;
    b.pin_dc = 47;
    bus_.config(b);
    panel_.setBus(&bus_);

    auto p = panel_.config();
    p.pin_cs = 5;
    p.pin_rst = 38;
    p.panel_width = 240;
    p.panel_height = 240;
    p.invert = true;
    panel_.config(p);
    setPanel(&panel_);
  }
};

Display lcd;
Preferences prefs;
WebServer web(80);
DNSServer dns;

enum class Mode { Provision, Online };
Mode mode = Mode::Provision;
String apName, apPass;
bool showingSetupQr = false;
bool otaActive = false;
String scanOptions;  // <option> list built before the AP starts

// ---------- drawing ----------
void drawTitle() {
  lcd.fillScreen(TFT_BLACK);
  lcd.drawBitmap((240 - TH_TITLE_W) / 2, 4, TH_TITLE, TH_TITLE_W, TH_TITLE_H, TFT_CYAN);
}

void drawCaption(const uint8_t *bmp, int w, int h, int y, uint32_t color) {
  lcd.fillRect(0, y, 240, h + 2, TFT_BLACK);
  lcd.drawBitmap((240 - w) / 2, y, bmp, w, h, color);
}

void drawQr(const String &text) {
  constexpr int size = 165, x = (240 - size) / 2, y = 50;
  lcd.fillRect(x - 8, y - 8, size + 16, size + 16, TFT_WHITE);  // quiet zone
  lcd.qrcode(text.c_str(), x, y, size, 4);
}

void showStatus(const uint8_t *bmp, int w, int h, uint32_t color, const String &detail = "") {
  drawTitle();
  lcd.drawBitmap((240 - w) / 2, 90, bmp, w, h, color);
  if (detail.length()) {
    lcd.setTextColor(TFT_WHITE);
    lcd.setTextSize(2);
    lcd.setTextDatum(textdatum_t::top_center);
    lcd.drawString(detail, 120, 140);
    lcd.setTextDatum(textdatum_t::top_left);
  }
}

void showJoinApQr() {
  drawTitle();
  drawQr("WIFI:T:WPA;S:" + apName + ";P:" + apPass + ";;");
  drawCaption(TH_SCAN_AP, TH_SCAN_AP_W, TH_SCAN_AP_H, 210, TFT_YELLOW);
  showingSetupQr = false;
}

void showSetupQr() {
  drawTitle();
  drawQr("http://192.168.4.1/");
  drawCaption(TH_SCAN_SETUP, TH_SCAN_SETUP_W, TH_SCAN_SETUP_H, 210, TFT_GREEN);
  showingSetupQr = true;
}

// ---------- provisioning (AP + captive portal) ----------
String htmlEscape(String s) {
  s.replace("&", "&amp;"); s.replace("<", "&lt;"); s.replace(">", "&gt;"); s.replace("\"", "&quot;");
  return s;
}

void handleRoot() {
  String page =
      "<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
      "<title>AI ลูกเทพ</title><style>body{font-family:sans-serif;max-width:420px;margin:24px auto;padding:0 16px}"
      "input,select,button{width:100%;font-size:18px;padding:10px;margin:6px 0;box-sizing:border-box}"
      "button{background:#0a84ff;color:#fff;border:0;border-radius:8px}</style></head><body>"
      "<h2>AI ลูกเทพ</h2><p>เลือก Wi-Fi ที่จะให้บอร์ดเชื่อมต่อ</p>"
      "<form method=POST action=/save><label>Wi-Fi</label><select name=ssid>" + scanOptions +
      "</select><label>หรือพิมพ์ชื่อเอง</label><input name=custom placeholder='SSID'>"
      "<label>รหัสผ่าน</label><input name=pass type=password><button>บันทึกและเชื่อมต่อ</button></form></body></html>";
  web.send(200, "text/html; charset=utf-8", page);
}

void handleSave() {
  String ssid = web.arg("custom").length() ? web.arg("custom") : web.arg("ssid");
  if (ssid.isEmpty()) {
    web.send(400, "text/plain; charset=utf-8", "ต้องระบุชื่อ Wi-Fi");
    return;
  }
  prefs.begin("wifi", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", web.arg("pass"));
  prefs.end();
  web.send(200, "text/html; charset=utf-8",
           "<meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
           "<h2>บันทึกแล้ว</h2><p>บอร์ดกำลังรีสตาร์ทและเชื่อมต่อ Wi-Fi</p>");
  delay(1500);
  ESP.restart();
}

void buildScanOptions() {
  int n = WiFi.scanNetworks();
  scanOptions = "";
  String seen;
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (s.isEmpty() || seen.indexOf("\n" + s + "\n") >= 0) continue;
    seen += "\n" + s + "\n";
    scanOptions += "<option>" + htmlEscape(s) + "</option>";
  }
  if (scanOptions.isEmpty()) scanOptions = "<option value=''>(ไม่พบ Wi-Fi)</option>";
  WiFi.scanDelete();
}

void startProvisioning() {
  mode = Mode::Provision;
  WiFi.mode(WIFI_AP_STA);
  buildScanOptions();

  uint8_t mac[6];
  WiFi.macAddress(mac);
  char suffix[8];
  snprintf(suffix, sizeof(suffix), "%02X%02X", mac[4], mac[5]);
  apName = String("AI-LukThep-") + suffix;
  char pw[9];
  snprintf(pw, sizeof(pw), "%08x", (unsigned)esp_random());
  apPass = pw;

  WiFi.softAP(apName.c_str(), apPass.c_str());
  dns.start(53, "*", WiFi.softAPIP());  // captive portal: every name -> us
  web.on("/", handleRoot);
  web.on("/save", HTTP_POST, handleSave);
  web.onNotFound([]() {
    web.sendHeader("Location", "http://192.168.4.1/", true);
    web.send(302, "text/plain", "");
  });
  web.begin();
  showJoinApQr();
  Serial.printf("Provisioning AP: %s / %s\n", apName.c_str(), apPass.c_str());
}

// ---------- normal operation ----------
void setupOta() {
  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() {
    otaActive = true;
    showStatus(TH_CONNECTING, TH_CONNECTING_W, TH_CONNECTING_H, TFT_YELLOW, "OTA update");
  });
  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
    lcd.fillRect(0, 140, 240, 30, TFT_BLACK);
    lcd.setTextColor(TFT_YELLOW);
    lcd.setTextSize(2);
    lcd.setCursor(100, 140);
    lcd.printf("%u%%", done * 100 / total);
  });
  ArduinoOTA.onEnd([]() { showStatus(TH_CONNECTED, TH_CONNECTED_W, TH_CONNECTED_H, TFT_GREEN, "OTA done"); });
  ArduinoOTA.onError([](ota_error_t) { otaActive = false; });
  ArduinoOTA.begin();
}

bool connectSaved() {
  prefs.begin("wifi", true);
  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");
  prefs.end();
  if (ssid.isEmpty()) return false;

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.begin(ssid.c_str(), pass.c_str());
  showStatus(TH_CONNECTING, TH_CONNECTING_W, TH_CONNECTING_H, TFT_YELLOW, ssid);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < CONNECT_TIMEOUT_MS) delay(200);
  if (WiFi.status() == WL_CONNECTED) {
    showStatus(TH_CONNECTED, TH_CONNECTED_W, TH_CONNECTED_H, TFT_GREEN, WiFi.localIP().toString());
    setupOta();
    mode = Mode::Online;
    return true;
  }
  showStatus(TH_FAILED, TH_FAILED_W, TH_FAILED_H, TFT_RED, ssid);
  delay(2500);
  WiFi.disconnect(true);
  return false;
}

void forgetWifi() {
  prefs.begin("wifi", false);
  prefs.clear();
  prefs.end();
  showStatus(TH_RESET, TH_RESET_W, TH_RESET_H, TFT_YELLOW);
  delay(1500);
  ESP.restart();
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_BOOT, INPUT_PULLUP);
  pinMode(PIN_BL, OUTPUT);
  digitalWrite(PIN_BL, LOW);  // backlight on
  lcd.init();
  lcd.setRotation(0);
  drawTitle();

  if (!connectSaved()) startProvisioning();
}

void loop() {
  static uint32_t lastBlink = 0, bootDownAt = 0;
  static bool blinkOn = false;

  // hold BOOT 3 s -> forget Wi-Fi
  if (digitalRead(PIN_BOOT) == LOW) {
    if (!bootDownAt) bootDownAt = millis();
    if (millis() - bootDownAt > RESET_HOLD_MS) forgetWifi();
  } else {
    bootDownAt = 0;
  }

  if (mode == Mode::Online) {
    ArduinoOTA.handle();
    if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();
  } else {
    dns.processNextRequest();
    web.handleClient();
    bool joined = WiFi.softAPgetStationNum() > 0;
    if (joined && !showingSetupQr) showSetupQr();
    if (!joined && showingSetupQr) showJoinApQr();
  }

  if (!otaActive && millis() - lastBlink > 500) {
    lastBlink = millis();
    blinkOn = !blinkOn;
    if (mode == Mode::Online) neopixelWrite(PIN_LED, 0, blinkOn ? 20 : 0, 0);
    else neopixelWrite(PIN_LED, 0, 0, blinkOn ? 25 : 0);
  }
}
