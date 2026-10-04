# AI ลูกเทพ (Hermes Voice Box) — สรุปส่งต่องาน

บอร์ด SpotPear ESP32-AI-1.54 V2.0 (ESP32-S3 N16R8) คุยด้วยเสียงกับ Hermes Agent บน Raspberry Pi 4
แผนหลัก: `hermes-voice-box-plan.md` · โค้ด: `firmware/` (ESP-IDF 6.1 ผ่าน PlatformIO)
อัปเดตล่าสุด: 2026-10-05 · เฟิร์มแวร์บนบอร์ด: **0.4.3** (ota_0)

## สถานะตามแผน

| Phase | งาน | สถานะ |
|---|---|---|
| 0 | pin map, ยืนยันฮาร์ดแวร์ | เสร็จ (`main/board_config.h`) |
| 1 | skeleton + LVGL + Orb | เสร็จ ผู้ใช้ยืนยันแล้ว |
| 2 | Wi-Fi + QR provisioning | เสร็จ ผู้ใช้ทดสอบผ่านครบ |
| 3 | OTA push + rollback | เสร็จ ทดสอบ rollback และ auth/SHA แล้ว ไม่ต้องใช้สายอีก |
| 4 | เสียง ไมค์/ลำโพง + loopback | ซอฟต์แวร์เสร็จ **รอผู้ใช้ฟังเสียงจริง** (ดูข้อ A) |
| 5 | Hermes API + bridge บน Pi | ยังไม่เริ่ม |
| 6 | ws_client + push-to-talk ครบ loop | ยังไม่เริ่ม |
| 7 | อนิเมชันครบทุก state + ฟอนต์ไทย + subtitle | ยังไม่เริ่ม |
| 8 | wake word, VAD, OTA pull, systemd | ยังไม่เริ่ม |

## งานที่เหลือ

**A. ปิด Phase 4** — ผู้ใช้กด BOOT ค้างแล้วพูด (≤4 วินาที) ปล่อยแล้วบอร์ดเล่นกลับ ต้องเสียงชัด ไม่หอน ไม่แตก
ถ้าแตก/เบา ปรับ `esp_codec_dev_set_in_gain` (ตอนนี้ 24 dB) และ `set_out_vol` (50) ใน `main/audio.c` แล้ว OTA
ผล self-test: vol 40 / gain 18 สะอาดสุด ค่าปัจจุบันจะ clip ถ้าลำโพงอยู่ติดไมค์ (กรณีเลวร้าย)

**B. Phase 5 — ฝั่ง Pi** (`ssh singha@192.168.1.57` ขอรหัสผ่านจากผู้ใช้ ห้ามบันทึกลงไฟล์)
- Hermes อยู่ที่ `~/.local/bin/hermes`, config ที่ `~/.hermes/`, รันเป็น systemd `hermes-gateway.service`
  (รีสตาร์ทแล้วช่องทางแชทที่ผู้ใช้ใช้อยู่จะสะดุด **แจ้งผู้ใช้ก่อนเสมอ**)
- **API server ยังไม่เปิด**: ต้องเพิ่ม `API_SERVER_ENABLED=true` และ `API_SERVER_KEY=<สุ่ม ≥16 ตัว>` ใน `~/.hermes/.env`
  (ฟังที่ 127.0.0.1:8642) ตรวจชื่อ env กับเอกสารของเวอร์ชันที่ติดตั้งจริงก่อน (`hermes --version`)
- เขียน `bridge/` (Python, ฟัง :8765 เฉพาะ LAN): รับ PCM 16 kHz → STT → Hermes (stream) → TTS ทีละประโยค → ส่งเสียงกลับ
  โปรโตคอลอยู่ในแผนข้อ 8.4
- **ต้องตัดสินใจ/ขอจากผู้ใช้:** ผู้ให้บริการ STT/TTS (แผนแนะนำเริ่มที่ cloud: Groq Whisper + edge-tts `th-TH-PremwadeeNeural`)
  และ API key ของมัน ตอนนี้ใน `.env` ของ Pi มีแค่ `OPENAI_API_KEY`
- สมมติฐานที่ใช้ (ผู้ใช้ไม่ได้คัดค้าน): บอร์ดอยู่บ้านตลอด ใช้ LAN อย่างเดียว ไม่ใช้ Tailscale Funnel
  และให้ตอบกลับเป็นเสียง + subtitle บนจอ

**C. Phase 6 — ฝั่งบอร์ด**
- `ws_client` (component `espressif/esp_websocket_client`): hello + token, ส่ง PCM ตอนกดปุ่ม, เล่นเสียงตอบ, reconnect แบบ backoff 1→30 วิ
- `buttons.c` มี callback push-to-talk พร้อมแล้ว (`buttons_set_ptt`) และ `audio_test.c` ใช้อยู่ ให้ย้ายเป็น ws loop จริง
- Bridge URL บันทึกใน NVS อยู่แล้ว (key `bridge_url` จากหน้า captive portal)
- **ข้อควรระวัง:** key `token` ใน NVS ตอนนี้ใช้เป็น **OTA token** ต้องแยก key ใหม่ (เช่น `bridge_token`) สำหรับ bridge
- ควรเรียก `ota_mgr_mark_valid_if_healthy()` หลังต่อ bridge ได้ (ตอนนี้เรียกหลัง Wi-Fi + push server)

**D. Phase 7 — UI**
- ตอนนี้ state อื่นนอกจาก IDLE เปลี่ยนแค่สี Orb ต้องทำอนิเมชันจริงตามตารางแผนข้อ 9 และ `ui_anim_tick(mic_level, spk_level)`
  (`audio_in_level()` / `audio_out_level()` พร้อมใช้)
- ข้อความไทยตอนนี้เป็นภาพ A8 ที่เรนเดอร์จาก Thonburi (`tools/gen_images.py`) ให้เปลี่ยนเป็นฟอนต์ LVGL (Noto Sans Thai ผ่าน `lv_font_conv`, มี node ใน `/opt/homebrew/bin`) แล้วทดสอบสระ/วรรณยุกต์
- subtitle STT/คำตอบ, หรี่จอเมื่อ idle เกิน 30 วิ, วัด ≥25 fps ขณะเสียงเล่น

**E. Phase 8** — wake word (ESP-SR WakeNet) + VAD (`audio_in_vad_is_silent` มีแล้ว), OTA แบบ pull (`ota_mgr_check_and_pull` ยังเป็น stub), systemd service ของ bridge, รีบูต Pi แล้วทุกอย่างกลับมาเอง

## วิธีทำงานกับโปรเจกต์

- **เครื่องมือ:** venv ที่ผมใช้อยู่ในโฟลเดอร์ชั่วคราวของ session (อาจหาย) ติดตั้งใหม่: `python3 -m venv v && v/bin/pip install platformio esptool pillow pyserial`
- **build:** `cd firmware && pio run -e usb` · **ขึ้นบอร์ดผ่าน Wi-Fi:** `tools/ota_push.sh` (อ่าน `firmware/.env` ซึ่งมี `OTA_TOKEN`, อยู่ใน .gitignore)
- **ทุกครั้งที่ปล่อยเวอร์ชัน** แก้ `firmware/version.txt` (สคริปต์ `tools/version_stamp.py` จะบังคับให้ป้ายเวอร์ชันถูก)
- **ทดสอบ rollback:** `pio run -e broken` แล้ว push ไฟล์นั้น บอร์ดต้องกลับเวอร์ชันเดิมเอง
- **คอนโซลทางสาย USB** (`/dev/cu.usbmodem2101`, 115200): `info`, `token`, `wifi_forget`, `reboot`, `beep`, `selftest`, `levels`, `vol N`, `gain N`
- **กู้บอร์ด:** ไฟล์สำรองเฟิร์มแวร์เดิม `~/esp32-backup/backup_spotpear_v2_16MB.bin` (วิธีเขียนกลับใน `esp32-s3-backup-and-flash.md`)
- บอร์ดบน LAN ที่ `192.168.1.54` / `ai-luk-thep.local` (macOS resolve ชื่อ .local ช้าบางครั้ง สคริปต์มี `OTA_IP` สำรอง)

## ปัญหา/หนี้ที่รู้อยู่

- ควรจอง IP ของบอร์ดและ Pi ไว้ในเราเตอร์ (DHCP เปลี่ยนแล้ว `OTA_IP` ใช้ไม่ได้)
- OTA token ส่งผ่าน HTTP ธรรมดาใน LAN (แผนยอมรับ) ถ้าต้องการเข้มขึ้นให้ทำ HTTPS
- SSH ของ Pi รับรหัสผ่านและรหัสสั้นมาก (`0.0.0.0:22`) แนะนำเปลี่ยนรหัสหรือใช้ SSH key
- เปิด codec ใช้เวลา ~5 วินาทีตอนบูต (ทำคู่ขนานกับ Wi-Fi แล้ว) · ครั้งหนึ่งเห็น `link lost (reason 8)` แล้วต่อใหม่เองได้
- ทัชสกรีน CST816D และการอ่านแบต/ชาร์จ ยังไม่ได้ใช้ (pin อยู่ใน `board_config.h`) · GPIO3 เป็นตัวล็อกไฟ ต้องคง HIGH ไว้ (ทำใน `main.c` แล้ว)
- โฟลเดอร์ `firmware-arduino-prototype/` เป็นต้นแบบเก่า ลบได้เมื่อไม่ต้องอ้างอิง · โปรเจกต์ยังไม่ได้ `git init`
