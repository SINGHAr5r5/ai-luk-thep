# Hermes Voice Box — แผนงานสำหรับ Claude Code

บอร์ด **SpotPear ESP32-AI-1.54 V2.0** (ESP32-S3 N16R8, LCD 1.54" 240×240, ไมค์ + ลำโพงในตัว)
ทำหน้าที่เป็นอุปกรณ์คุยด้วยเสียงกับ **Hermes Agent** ที่รันอยู่บน **Raspberry Pi 4**

> **ถึง Claude Code:** อ่านไฟล์นี้ทั้งหมดก่อนเริ่มงาน แล้วทำตาม Phase ทีละขั้น
> ห้ามข้าม Phase ถ้าเกณฑ์ผ่าน (Acceptance) ของ Phase ก่อนหน้ายังไม่ผ่าน
> และก่อนแตะแฟลช ต้องแน่ใจว่าผู้ใช้สำรองเฟิร์มแวร์เดิมไว้แล้ว (ดู `esp32-s3-backup-and-flash.md`)

---

## 1. สถาปัตยกรรม

```
┌───────────────── ESP32-S3 (บอร์ด) ─────────────────┐        ┌──────────── Raspberry Pi 4 ────────────┐
│ ไมค์ ─► I2S ─► audio_in ──┐                         │        │                                         │
│                           ├─► ws_client ◄══ WebSocket ══►   voice-bridge (Python, :8765)            │
│ ลำโพง ◄─ I2S ◄─ audio_out ┘    (JSON + PCM binary)  │  LAN   │   ├─ STT  (เสียง → ข้อความ)              │
│ LCD ◄─ LVGL ◄─ ui_anim (อนิเมชันตามสถานะ)          │        │   ├─ Hermes API  http://127.0.0.1:8642/v1│
│ Wi-Fi: wifi_mgr + provisioning (QR)                 │        │   └─ TTS  (ข้อความ → เสียง)              │
│ OTA:   ota_mgr (push จาก Mac / pull จาก Pi)         │        │ hermes gateway (API server, :8642)      │
└─────────────────────────────────────────────────────┘        └─────────────────────────────────────────┘
```

**ทำไมต้องมี voice-bridge:** Hermes API server รับและส่งเป็น**ข้อความ**แบบ OpenAI-compatible ส่วน ESP32 แปลงเสียงเป็นข้อความ (STT) หรือข้อความเป็นเสียง (TTS) คุณภาพดีเองไม่ได้ จึงต้องมีตัวกลางบน Pi ที่รับเสียงจากบอร์ด → แปลงเป็นข้อความ → ส่งให้ Hermes → แปลงคำตอบเป็นเสียง → ส่งกลับไปที่บอร์ด

**ข้อดีอีกข้อ:** Hermes ฟังแค่ localhost ได้เหมือนเดิม ไม่ต้องเปิดพอร์ต 8642 ออกนอกเครื่อง มีแค่ bridge ที่คุยกับบอร์ด

---

## 2. สิ่งที่ต้องยืนยันก่อนเขียนโค้ด (Phase 0)

| เรื่อง                                    | วิธีเช็ค                                                                                                |
| ----------------------------------------- | ------------------------------------------------------------------------------------------------------- |
| ขนาดแฟลชและ PSRAM                         | `esptool.py flash_id` และดู log ตอนบูต (`PSRAM: 8MB`)                                                   |
| ขาจอ, I2S, I2C ของ codec, ปุ่ม, backlight | ดูโฟลเดอร์ board ของ SpotPear 1.54 ใน repo `78/xiaozhi-esp32` (`main/boards/`) แล้วคัดลอก pin map มาใช้ |
| ชิป audio codec                           | I2C scan (ES8311 ปกติอยู่ที่ address `0x18`)                                                            |
| ไดรเวอร์จอ                                | น่าจะเป็น ST7789 ให้ยืนยันจาก board config ด้านบน                                                       |
| ปุ่ม                                      | BOOT = GPIO0, PWR (ดูว่าต่อเข้า GPIO ไหนหรือเป็นปุ่มฮาร์ดแวร์)                                          |
| ขาที่ว่าง                                 | IO39, IO40, IO43, IO44, RGB LED ที่ IO48                                                                |

**ผลลัพธ์ที่ต้องได้:** ไฟล์ `firmware/main/board_config.h` ที่รวม pin ทั้งหมดไว้ในที่เดียว

---

## 3. โครงสร้าง Repo

```
hermes-voice-box/
├── firmware/                    # ESP-IDF v5.x (target esp32s3)
│   ├── partitions.csv
│   ├── sdkconfig.defaults
│   └── main/
│       ├── main.c               # app_main + state machine
│       ├── board_config.h
│       ├── wifi_mgr.c/.h        # เชื่อม Wi-Fi + provisioning + QR
│       ├── ota_mgr.c/.h         # OTA push/pull + rollback
│       ├── audio_in.c/.h        # ไมค์ I2S + VAD
│       ├── audio_out.c/.h       # ลำโพง I2S + buffer
│       ├── ws_client.c/.h       # WebSocket ไปหา bridge
│       ├── ui.c/.h              # LVGL init + หน้าจอต่าง ๆ
│       ├── ui_anim.c/.h         # อนิเมชันตามสถานะ
│       ├── buttons.c/.h
│       └── settings.c/.h        # NVS (Wi-Fi, bridge URL, token)
├── bridge/                      # Python 3.11 บน Pi 4
│   ├── bridge.py
│   ├── stt.py
│   ├── tts.py
│   ├── hermes_client.py
│   ├── ota_server.py            # (ทางเลือก) ให้บอร์ดดึงเฟิร์มแวร์
│   ├── config.yaml
│   └── systemd/voice-bridge.service
└── docs/
    └── protocol.md
```

---

## 4. Partition Table (16MB, รองรับ OTA)

```csv
# Name,   Type, SubType, Offset,   Size
nvs,      data, nvs,     0x9000,   0x6000
otadata,  data, ota,     0xf000,   0x2000
phy_init, data, phy,     0x11000,  0x1000
ota_0,    app,  ota_0,   0x20000,  0x600000
ota_1,    app,  ota_1,   0x620000, 0x600000
assets,   data, spiffs,  0xC20000, 0x3E0000
```

- มี 2 สล็อต สล็อตละ 6MB: OTA จะเขียนลงสล็อตที่ไม่ได้ใช้อยู่ ถ้าเฟิร์มแวร์ใหม่พังก็ย้อนกลับไปสล็อตเดิมได้
- `assets` ใช้เก็บฟอนต์ไทย, รูป และเสียงแจ้งเตือน (อัปเดตแยกจากโค้ดได้)
- การแฟลชครั้งแรกต้องเสียบสาย (`idf.py flash`) หลังจากนั้นใช้ OTA ได้ตลอด

---

## 5. State Machine หลัก (`main.c`)

```
BOOT
 └─► WIFI_CONNECTING ──สำเร็จ──► BRIDGE_CONNECTING ──สำเร็จ──► IDLE
        │ ไม่มีค่า Wi-Fi / ต่อไม่ติดเกิน 20 วิ                      │
        ▼                                                    กดปุ่ม / wake word
     WIFI_PROVISIONING (แสดง QR)                                   ▼
        │ ผู้ใช้ตั้งค่าสำเร็จ → รีบูต                            LISTENING ──ปล่อยปุ่ม/VAD เงียบ──► THINKING
                                                                                                   │
                                     IDLE ◄──── tts_end ──── SPEAKING ◄──── tts_start ─────────────┘
ทุกสถานะ ──► OTA_UPDATING (เมื่อได้คำสั่ง OTA)      ทุกสถานะ ──► ERROR (แสดงบนจอ แล้วลองใหม่)
```

ฟังก์ชันที่ต้องมี:

```c
typedef enum { ST_BOOT, ST_WIFI_CONNECTING, ST_WIFI_PROVISIONING, ST_BRIDGE_CONNECTING,
               ST_IDLE, ST_LISTENING, ST_THINKING, ST_SPEAKING, ST_OTA_UPDATING, ST_ERROR } app_state_t;

void        app_set_state(app_state_t s);   // เปลี่ยนสถานะ + แจ้ง ui_anim
app_state_t app_get_state(void);
void        app_event_loop_task(void *arg); // รับ event จาก wifi/ws/button/audio ผ่าน FreeRTOS queue
```

---

## 6. ฟีเจอร์ 1: Wi-Fi + QR Code (`wifi_mgr`)

### Logic

1. ตอนบูต อ่าน SSID/รหัสผ่านจาก NVS
2. **ถ้ามีค่า** → ลองเชื่อมต่อ (timeout 20 วิ, retry 3 ครั้ง)
   - เชื่อมได้ → **ไม่แสดง QR** และไปที่ `BRIDGE_CONNECTING` ทันที
   - เชื่อมไม่ได้ → เข้าโหมดตั้งค่า (QR)
3. **ถ้าไม่มีค่า** → เข้าโหมดตั้งค่าเลย
4. **รีเซ็ต Wi-Fi:** กด BOOT ค้าง 5 วิ → ลบค่า Wi-Fi จาก NVS แล้วรีบูต
5. ถ้าเน็ตหลุดระหว่างใช้งาน ให้ reconnect อัตโนมัติก่อน ถ้าเกิน 60 วิแล้วยังต่อไม่ได้ ค่อยแสดงหน้าจอเตือน (ไม่ต้องกลับไปโหมด QR อัตโนมัติ เพราะ router อาจแค่รีบูตอยู่)

### โหมดตั้งค่า (SoftAP + Captive Portal + QR)

- เปิด AP ชื่อ `HermesBox-XXXX` (XXXX = 4 หลักท้ายของ MAC) พร้อมรหัสผ่านแบบสุ่ม 8 ตัว
- **หน้าจอ QR #1:** `WIFI:T:WPA;S:HermesBox-XXXX;P:<รหัส>;;` สแกนแล้วมือถือจะเข้า AP ให้เอง
- เปิด DNS captive portal ให้มือถือเด้งหน้าตั้งค่าขึ้นมาเอง (สำรอง: **QR #2** = `http://192.168.4.1`)
- หน้าเว็บ: แสดงรายชื่อ Wi-Fi ที่สแกนเจอ (`esp_wifi_scan_start`) → เลือก → ใส่รหัส → ช่องเสริมสำหรับ Bridge URL → กดบันทึก
- ทดสอบเชื่อมต่อก่อนบันทึก: ถ้าผ่าน → บันทึกลง NVS → รีบูต ถ้าไม่ผ่าน → แจ้ง error บนหน้าเว็บ
- ใช้ไลบรารี QR: `espressif/qrcode` (component) หรือ `lv_qrcode` ของ LVGL

### ฟังก์ชัน

```c
esp_err_t wifi_mgr_init(void);
bool      wifi_mgr_has_credentials(void);
esp_err_t wifi_mgr_connect_saved(uint32_t timeout_ms);
esp_err_t wifi_mgr_start_provisioning(void);      // SoftAP + http server + dns + แสดง QR
void      wifi_mgr_stop_provisioning(void);
esp_err_t wifi_mgr_scan(wifi_ap_record_t *out, uint16_t *count);
esp_err_t wifi_mgr_save_credentials(const char *ssid, const char *pass);
void      wifi_mgr_clear_credentials(void);       // จากการกด BOOT ค้าง
bool      wifi_mgr_is_connected(void);
void      wifi_mgr_start_mdns(const char *hostname); // hermes-box.local
```

**Acceptance:** ถ้าไม่มีค่า Wi-Fi ต้องขึ้น QR → สแกนแล้วตั้งค่าได้ → รีบูตแล้วเชื่อมเองโดยไม่ขึ้น QR อีก → กด BOOT ค้าง 5 วิแล้วกลับมาขึ้น QR

---

## 7. ฟีเจอร์ 2: OTA (`ota_mgr`)

รองรับ 2 แบบ:

### A) Push: จาก Mac ส่งตรงเข้าบอร์ด (ใช้ตอนพัฒนา)

- บอร์ดเปิด HTTP endpoint `POST http://hermes-box.local/ota` (header `X-OTA-Token`)
- รับไฟล์ `.bin` แบบ stream → `esp_ota_begin/write/end` → `esp_ota_set_boot_partition` → รีบูต
- คำสั่งที่ Claude Code ใช้หลัง build:

```bash
idf.py build
curl -X POST --data-binary @build/hermes_voice_box.bin \
     -H "X-OTA-Token: $OTA_TOKEN" http://hermes-box.local/ota
```

- เขียนเป็นสคริปต์ `tools/ota_push.sh` ไว้

### B) Pull: บอร์ดดึงจาก Pi (ใช้งานจริง)

- Pi ให้บริการไฟล์ `manifest.json` → `{"version":"1.2.0","url":"http://pi.local:8765/fw/1.2.0.bin","sha256":"..."}`
- บอร์ดเช็คตอนบูตและทุก 6 ชม. หรือเมื่อ bridge ส่งคำสั่ง `{"type":"ota_check"}`
- ใช้ `esp_https_ota` (ถ้าใช้ HTTP ใน LAN ต้องเปิด `CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP`)

### ความปลอดภัยและการกันบอร์ดพัง

- เปิด `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`
- เฟิร์มแวร์ใหม่ต้องเรียก `esp_ota_mark_app_valid_cancel_rollback()` **หลังจาก** ต่อ Wi-Fi และ bridge ได้แล้วเท่านั้น ถ้าไม่ผ่านภายใน 60 วิ → รีบูตแล้วย้อนกลับเวอร์ชันเดิมอัตโนมัติ
- ตรวจ SHA-256 ของไฟล์ก่อนสลับ partition
- ใช้ token ยาว ≥ 16 ตัวอักษร เก็บไว้ใน NVS ห้าม hardcode ใน repo
- หน้าจอ: แสดงแถบ progress ระหว่าง OTA

### ฟังก์ชัน

```c
esp_err_t ota_mgr_init(void);                         // เช็ค rollback state
esp_err_t ota_mgr_start_push_server(void);            // POST /ota
esp_err_t ota_mgr_check_and_pull(const char *manifest_url);
void      ota_mgr_mark_valid_if_healthy(void);        // เรียกเมื่อ wifi+bridge OK
const char *ota_mgr_running_version(void);
void      ota_mgr_on_progress(void (*cb)(int pct));   // ส่งให้ UI
```

**Acceptance:** อัปโหลดโค้ดใหม่ผ่าน `ota_push.sh` ได้โดยไม่เสียบสาย และถ้าอัปโหลดเฟิร์มแวร์ที่ตั้งใจให้พัง บอร์ดต้องย้อนกลับเวอร์ชันเดิมได้เอง

---

## 8. ฟีเจอร์ 3: เชื่อมต่อกับ Hermes บน Pi 4

### 8.1 เปิด API server ของ Hermes (ทำบน Pi)

เพิ่มใน `~/.hermes/.env`:

```env
API_SERVER_ENABLED=true
API_SERVER_KEY=<สุ่มอย่างน้อย 16 ตัวอักษร>
# API_SERVER_HOST=127.0.0.1   (ค่า default: ฟังแค่ในเครื่อง ซึ่งเหมาะแล้ว เพราะ bridge อยู่บน Pi เครื่องเดียวกัน)
# API_SERVER_PORT=8642
```

แล้วรัน:

```bash
hermes gateway
# ต้องเห็น: [API Server] API server listening on http://127.0.0.1:8642
```

ทดสอบ:

```bash
curl http://127.0.0.1:8642/v1/chat/completions \
  -H "Authorization: Bearer $API_SERVER_KEY" -H "Content-Type: application/json" \
  -d '{"model":"hermes-agent","messages":[{"role":"user","content":"สวัสดี"}]}'
```

ทำ `hermes gateway` เป็น systemd service ให้รันตลอด (หรืออย่างน้อยรันไว้ใน tmux)

> Claude Code: ตรวจกับเอกสาร Hermes เวอร์ชันที่ติดตั้งอยู่จริงอีกครั้ง (`hermes --version` และหน้า docs “API Server”) เพราะชื่อ env หรือ endpoint อาจเปลี่ยนไปตามเวอร์ชัน

### 8.2 การจำบทสนทนา

- **แนะนำ:** ใช้ **Responses API** (`POST /v1/responses`) แล้วส่ง `previous_response_id` จากรอบก่อน ให้ Hermes เก็บประวัติเองฝั่ง server
- หรือใช้ Chat Completions แล้วให้ bridge เก็บ `messages` เองต่อหนึ่งอุปกรณ์
- เริ่มบทสนทนาใหม่เมื่อเงียบไปเกิน 10 นาที หรือเมื่อกดปุ่ม PWR สั้น ๆ 2 ครั้ง

### 8.3 Voice Bridge (`bridge/`, Python บน Pi)

**Pipeline ต่อหนึ่งรอบการคุย:**

1. รับ PCM 16kHz/16-bit/mono จากบอร์ด จนได้ `listen_stop`
2. **STT** → ได้ข้อความ → ส่ง `{"type":"stt","text":...}` กลับไปให้บอร์ดแสดงเป็น subtitle
3. ส่งข้อความเข้า Hermes แบบ **stream** พร้อมคำสั่งเสริม: _"ตอบเป็นภาษาพูด สั้น กระชับ ไม่ใช้ markdown หรือ emoji"_
4. ตัดข้อความเป็นประโยค (เมื่อเจอ `.` `?` `!` ช่องว่างยาว หรือขึ้นบรรทัดใหม่) → **TTS ทีละประโยค** → stream เสียงกลับทันที (ผู้ใช้จะได้ยินเสียงเร็วขึ้น ไม่ต้องรอจนตอบครบ)
5. ลบ markdown/emoji ออกก่อนส่งเข้า TTS

**ตัวเลือก STT/TTS (ภาษาไทย):**

|     | Local (ฟรี, ไม่ต้องใช้เน็ต)                                 | Cloud (เร็ว, คุณภาพดี)                                              |
| --- | ----------------------------------------------------------- | ------------------------------------------------------------------- |
| STT | faster-whisper `base`/`small` (Pi 4 ช้า: ใช้เวลาหลายวินาที) | Groq Whisper / OpenAI Whisper                                       |
| TTS | Piper (ต้องเช็คก่อนว่ามีเสียงไทยหรือไม่)                    | edge-tts (`th-TH-PremwadeeNeural`, `th-TH-NiwatNeural`), OpenAI TTS |

→ **เริ่มด้วย cloud ก่อน** ให้ระบบทำงานครบ แล้วค่อยลอง local ทีหลัง ทำเป็น interface ให้สลับได้ใน `config.yaml`

**ฟังก์ชันฝั่ง Python:**

```python
# bridge.py
async def ws_handler(ws)                       # 1 connection ต่อ 1 อุปกรณ์, ตรวจ token ใน hello
async def handle_utterance(session, pcm: bytes)
def    split_sentences(stream) -> AsyncIterator[str]
def    strip_markdown(text: str) -> str
# stt.py
class STT: async def transcribe(pcm: bytes, lang="th") -> str
# tts.py
class TTS: async def synth_stream(text: str) -> AsyncIterator[bytes]   # คืน PCM 16kHz mono
# hermes_client.py
class HermesClient:
    async def ask_stream(text: str, session_id: str) -> AsyncIterator[str]
    def reset(session_id: str)
```

ใช้ `openai` SDK โดยตั้ง `base_url="http://127.0.0.1:8642/v1"`, `api_key=API_SERVER_KEY`, `model="hermes-agent"`

### 8.4 Protocol บอร์ด ↔ Bridge (WebSocket `ws://<pi>:8765/ws`)

| ทิศทาง | ข้อความ                                                           | ความหมาย                        |
| ------ | ----------------------------------------------------------------- | ------------------------------- |
| ESP→Pi | `{"type":"hello","device_id","fw_version","token"}`               | เริ่ม session                   |
| ESP→Pi | `{"type":"listen_start"}` / binary PCM / `{"type":"listen_stop"}` | ส่งเสียงพูด                     |
| ESP→Pi | `{"type":"abort"}`                                                | ผู้ใช้กดปุ่มขัดจังหวะ           |
| Pi→ESP | `{"type":"stt","text"}`                                           | ข้อความที่ฟังได้                |
| Pi→ESP | `{"type":"state","value":"thinking"}`                             | ให้บอร์ดเปลี่ยนอนิเมชัน         |
| Pi→ESP | `{"type":"tts_start"}` / binary PCM / `{"type":"tts_end"}`        | เสียงตอบ                        |
| Pi→ESP | `{"type":"reply_text","text"}`                                    | ข้อความคำตอบ (ไว้แสดง subtitle) |
| Pi→ESP | `{"type":"ota_check"}` / `{"type":"error","msg"}`                 | คำสั่งอื่น ๆ                    |

- เสียง: PCM 16kHz 16-bit mono, chunk ละ 20–60ms (ใน LAN ใช้ raw PCM ได้เลย ถ้าอยากประหยัด bandwidth ค่อยเปลี่ยนเป็น Opus ทีหลัง)
- ถ้าหลุด ให้ reconnect แบบ exponential backoff (1, 2, 4 ... สูงสุด 30 วิ)
- ฝั่งบอร์ดเก็บ bridge URL + token ไว้ใน NVS (ตั้งค่าได้จากหน้า captive portal)

### 8.5 ฟังก์ชันฝั่ง ESP32

```c
// audio_in
esp_err_t audio_in_init(void);                 // I2S RX + codec init (ES8311 ถ้ายืนยันแล้ว)
void      audio_in_start(void); void audio_in_stop(void);
float     audio_in_level(void);                // RMS 0..1 ส่งให้อนิเมชัน
bool      audio_in_vad_is_silent(uint32_t ms); // ใช้หยุดฟังอัตโนมัติ
// audio_out
esp_err_t audio_out_init(void);                // I2S TX + เปิดแอมป์
void      audio_out_write(const uint8_t *pcm, size_t len); // เข้า ring buffer ใน PSRAM
void      audio_out_flush(void); void audio_out_set_volume(uint8_t v);
float     audio_out_level(void);               // ส่งให้อนิเมชันตอนพูด
// ws_client
esp_err_t ws_client_start(const char *url, const char *token);
void      ws_send_json(const char *json);
void      ws_send_audio(const uint8_t *pcm, size_t len);
bool      ws_is_connected(void);
// buttons
void      buttons_init(void);                  // BOOT: กดค้าง = พูด, ค้าง 5 วิ = รีเซ็ต Wi-Fi
```

**วิธีเริ่มพูด:**

- **Phase แรก:** Push-to-talk (กด BOOT ค้างเพื่อพูด ปล่อยเพื่อส่ง) เพราะง่ายและเสถียรที่สุด
- **Phase หลัง:** Wake word ด้วย ESP-SR WakeNet (ใช้คำปลุกที่มีให้ในชุด หรือเทรนคำของตัวเอง) + VAD เพื่อหยุดฟังอัตโนมัติเมื่อเงียบ 0.8 วิ

**Acceptance:** กดปุ่มพูดภาษาไทย → จอขึ้นข้อความที่ฟังได้ → Hermes ตอบเป็นเสียงทางลำโพงภายใน ~3–5 วิ (ถ้าใช้ cloud STT/TTS) → ถามต่อเนื่องแล้ว Hermes ยังจำบริบทรอบก่อนได้

---

## 9. ฟีเจอร์ 4: หน้าจอและอนิเมชัน (`ui`, `ui_anim`)

ใช้ **LVGL 9** (`esp_lvgl_port`) + ไดรเวอร์ ST7789 ใช้ buffer คู่ใน PSRAM ทำงานที่ 30 fps และรัน LVGL task บน core 1 แยกจากงานเสียง

### ธีม: “Orb” ลูกแก้วพลังงานตรงกลางจอ (วาดด้วยโค้ด ไม่ต้องใช้ไฟล์ภาพ)

| สถานะ             | อนิเมชัน                                                                 | สีหลัก      |
| ----------------- | ------------------------------------------------------------------------ | ----------- |
| BOOT              | โลโก้ fade-in + ข้อความเวอร์ชัน                                          | ขาว         |
| WIFI_PROVISIONING | QR ใหญ่ตรงกลาง + ข้อความ “สแกนเพื่อเชื่อม Wi-Fi” + ขอบกระพริบช้า ๆ       | ฟ้า         |
| BRIDGE_CONNECTING | วงแหวนหมุน (spinner)                                                     | ฟ้าอ่อน     |
| IDLE              | Orb “หายใจ” (ขยาย/หด ช้า ๆ รอบละ 3 วิ, glow จาง ๆ) + มีเวลาเล็ก ๆ ด้านบน | น้ำเงินม่วง |
| LISTENING         | Orb ขยาย + แถบคลื่นเสียงรอบวงที่ขยับตาม `audio_in_level()`               | เขียวมิ้นต์ |
| THINKING          | อนุภาค 3–6 จุดโคจรรอบ Orb + Orb หมุน gradient                            | ม่วง/ชมพู   |
| SPEAKING          | Orb เต้นตาม `audio_out_level()` + subtitle คำตอบเลื่อนด้านล่าง           | ส้มทอง      |
| OTA_UPDATING      | แถบ progress วงกลม + %                                                   | เหลือง      |
| ERROR             | Orb สีแดงสั่นเบา ๆ + ข้อความสั้น                                         | แดง         |

### แนวทางเทคนิค

- ใช้ `lv_anim_t` คุมค่า scale/opacity/angle และใช้ easing (`lv_anim_path_ease_in_out`) ให้การเคลื่อนไหวนุ่ม
- เปลี่ยนสถานะด้วย **crossfade 200–300ms** ห้ามตัดภาพทันที
- ทำ glow ด้วยวงกลมซ้อน 3–4 ชั้นที่ opacity ลดหลั่นกัน หรือใช้ `shadow` ของ LVGL
- ทำ level smoothing ก่อนใช้ค่า (`level = level*0.8 + new*0.2`) เพื่อให้คลื่นไม่กระตุก
- **ฟอนต์ไทย:** แปลง Noto Sans Thai เป็นฟอนต์ LVGL (`lv_font_conv`) เก็บไว้ใน `assets` ต้องทดสอบสระบน/ล่างและวรรณยุกต์ว่าวางถูกตำแหน่ง ถ้าเพี้ยน ให้ใช้ฟีเจอร์ Thai/complex text ของ LVGL หรือเลือกฟอนต์ที่วาง mark ไว้แล้ว
- (ทางเลือก) ทำ “ตา” แบบหุ่นยนต์ที่กระพริบตอน IDLE เป็นธีมที่ 2 แล้วสลับได้ใน config

### ฟังก์ชัน

```c
esp_err_t ui_init(void);                          // LCD + LVGL + backlight
void      ui_show_state(app_state_t s);           // เรียกจาก app_set_state
void      ui_show_qr(const char *payload, const char *caption);
void      ui_set_subtitle(const char *text);      // ข้อความ STT/คำตอบ
void      ui_set_ota_progress(int pct);
void      ui_anim_tick(float mic_level, float spk_level); // เรียกทุก ~33ms
void      ui_set_brightness(uint8_t pct);
```

**Acceptance:** ทุกสถานะมีอนิเมชันของตัวเอง เปลี่ยนสถานะแบบนุ่มนวล ทำงานได้ ≥ 25 fps และเสียงไม่สะดุดในขณะที่อนิเมชันเล่นอยู่

---

## 10. ลำดับการทำงาน (Phases)

| Phase | งาน                                                  | Acceptance                                               |
| ----- | ---------------------------------------------------- | -------------------------------------------------------- |
| 0     | ยืนยัน pin/codec/จอ → `board_config.h`               | จอแสดงสีได้, I2C scan เจอ codec                          |
| 1     | Skeleton ESP-IDF + partition + LVGL + Orb IDLE       | แฟลชผ่านสายแล้วเห็น Orb หายใจ                            |
| 2     | `wifi_mgr` + QR provisioning                         | ผ่านเกณฑ์ข้อ 6                                           |
| 3     | `ota_mgr` (push ก่อน) + rollback                     | ผ่านเกณฑ์ข้อ 7 **หลังจากนี้ห้ามใช้สายอีก**               |
| 4     | audio_in/out: loopback test (พูดแล้วเล่นเสียงกลับ)   | เสียงชัด ไม่มีเสียงหอน/แตก                               |
| 5     | Hermes API + bridge (ทดสอบด้วยไฟล์ wav จาก Mac ก่อน) | wav → ได้ข้อความตอบ + เสียง                              |
| 6     | ws_client + push-to-talk ครบ loop                    | ผ่านเกณฑ์ข้อ 8                                           |
| 7     | อนิเมชันครบทุกสถานะ + subtitle ไทย                   | ผ่านเกณฑ์ข้อ 9                                           |
| 8     | Wake word + VAD, OTA แบบ pull, systemd services      | ใช้งานได้โดยไม่ต้องกดปุ่ม รีบูต Pi แล้วทุกอย่างกลับมาเอง |

---

## 11. Config และ Secrets

- `firmware`: ค่าเริ่มต้นอยู่ใน `menuconfig` (Kconfig) ค่าจริงเก็บใน NVS ห้ามเอา token ลง git
- `bridge/config.yaml`: `hermes.base_url`, `hermes.api_key` (อ่านจาก env), `stt.provider`, `tts.provider`, `tts.voice`, `device_tokens`
- ใส่ `.env` และ `secrets.*` ไว้ใน `.gitignore`

## 12. ความเสี่ยงที่ต้องรู้

- **Pi 4 ทำ STT แบบ local ช้า** → ใช้ cloud ไปก่อน
- **Hermes ใช้เวลาตอบนาน** ถ้าเรียกใช้ tools (ค้นเว็บ, รันคำสั่ง) → แสดงอนิเมชัน THINKING ไว้ และอาจให้ bridge พูด “ขอคิดแป๊บนึงนะ” ถ้าเกิน 4 วิ
- **ความปลอดภัย:** Hermes รันคำสั่งบน Pi ได้ ให้ bridge ตรวจ device token ทุกครั้ง และเปิดพอร์ต 8765 ให้แค่ใน LAN
- **ไฟและแบต:** Wi-Fi, ลำโพง และ backlight กินไฟมาก → หรี่จอเมื่อ IDLE เกิน 30 วิ
