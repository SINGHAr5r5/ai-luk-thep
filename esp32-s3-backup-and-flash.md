# สำรองและเขียนทับเฟิร์มแวร์ ESP32-S3
บอร์ด: SpotPear ESP32-AI-1.54 V2.0 · เครื่องที่ใช้: Mac mini

---

## 0. เตรียมเครื่องมือ

```bash
pip install esptool
esptool.py version
```

> **หมายเหตุ:** esptool v5 ขึ้นไปใช้คำสั่ง `esptool` และชื่อคำสั่งย่อยแบบขีดกลาง (`read-flash`, `write-flash`, `erase-flash`)
> ส่วน v4 ใช้ `esptool.py` และขีดล่าง (`read_flash` ...) ไฟล์นี้เขียนแบบ v4 ถ้าใช้ v5 ให้เปลี่ยนชื่อคำสั่งตามนี้

---

## 1. ต่อบอร์ดและหาพอร์ต

1. เสียบ USB-C เข้ากับ Mac mini (ต้องใช้สายที่ส่งข้อมูลได้ ไม่ใช่สายชาร์จอย่างเดียว)
2. หาพอร์ต:

```bash
ls /dev/cu.*
# ตัวอย่าง: /dev/cu.usbmodem1101
```

3. ตั้งตัวแปรไว้ จะได้ไม่ต้องพิมพ์ซ้ำ:

```bash
PORT=/dev/cu.usbmodem1101
```

### ถ้าบอร์ดไม่ขึ้น หรือต่อไม่ติด → เข้าโหมดดาวน์โหลด
- ถอด USB ออก แล้ว**กด BOOT ค้างไว้** ระหว่างเสียบ USB-C กลับเข้าไป จากนั้นค่อยปล่อย
- หรือถ้าเสียบอยู่แล้ว: กด BOOT ค้าง → กด PWR/รีเซ็ต 1 ครั้ง → ปล่อย BOOT

---

## 2. เช็คชิปและขนาดแฟลช

```bash
esptool.py --chip esp32s3 --port $PORT flash_id
```

ดูบรรทัด `Detected flash size:` ซึ่งควรเป็น **16MB**
ถ้าได้ค่าอื่น ให้เปลี่ยนขนาดในขั้นตอนถัดไปให้ตรงกับที่เห็น:

| ขนาดแฟลช | ค่าที่ใช้ |
|---|---|
| 4MB | `0x400000` |
| 8MB | `0x800000` |
| 16MB | `0x1000000` |

---

## 3. สำรองเฟิร์มแวร์เดิม (ทั้งแฟลช)

```bash
mkdir -p ~/esp32-backup && cd ~/esp32-backup

esptool.py --chip esp32s3 --port $PORT --baud 921600 \
  read_flash 0 0x1000000 backup_spotpear_v2_16MB.bin
```

- ใช้เวลาประมาณ 2–5 นาที
- ถ้าอ่านแล้วขึ้น error ให้ลดเหลือ `--baud 460800` หรือ `115200`

### ตรวจว่าไฟล์ที่สำรองไว้ใช้ได้
```bash
ls -lh backup_spotpear_v2_16MB.bin     # ต้องได้ 16M พอดี
shasum -a 256 backup_spotpear_v2_16MB.bin > backup.sha256
```

ถ้าต้องการแน่ใจ ให้อ่านซ้ำอีกรอบเป็นไฟล์ที่สอง แล้วเทียบ hash กัน ต้องได้ค่าตรงกัน

> **เก็บไฟล์ .bin นี้ไว้หลายที่** (เช่น บนเครื่อง + cloud) เพราะเป็นทางเดียวที่จะกลับไปใช้เฟิร์มแวร์เดิมจากโรงงานได้

---

## 4. ล้างแฟลช (ก่อนลงของใหม่)

```bash
esptool.py --chip esp32s3 --port $PORT erase_flash
```

การล้างจะลบทุกอย่าง รวมถึงค่า Wi-Fi และค่าตั้งค่าที่เก็บไว้ใน NVS ด้วย

---

## 5. เขียนเฟิร์มแวร์ใหม่ทับ

### แบบ A: ไฟล์รวมไฟล์เดียว (merged bin)
เฟิร์มแวร์สำเร็จรูป เช่น XiaoZhi release มักให้มาเป็นไฟล์เดียว และเขียนที่ offset `0x0`:

```bash
esptool.py --chip esp32s3 --port $PORT --baud 921600 \
  write_flash 0x0 firmware_merged.bin
```

### แบบ B: ไฟล์แยก (ได้มาจากการ build ด้วย ESP-IDF เอง)
offset มาตรฐานของ ESP32-S3:

| ไฟล์ | Offset |
|---|---|
| bootloader.bin | `0x0` |
| partition-table.bin | `0x8000` |
| ota_data_initial.bin (ถ้ามี) | ตามที่ partition table กำหนด (มักเป็น `0xd000`) |
| app (เช่น xiaozhi.bin) | ตามที่ partition table กำหนด (มักเป็น `0x10000`) |

```bash
esptool.py --chip esp32s3 --port $PORT --baud 921600 \
  write_flash \
  0x0     bootloader.bin \
  0x8000  partition-table.bin \
  0x10000 app.bin
```

> ถ้า build ด้วย ESP-IDF ใช้ `idf.py -p $PORT flash` ได้เลย ไม่ต้องจำ offset เอง
> หรือดู offset ที่ถูกต้องได้ในไฟล์ `build/flash_args`

หลังเขียนเสร็จ ให้กด PWR/รีเซ็ต หรือถอดแล้วเสียบ USB ใหม่

---

## 6. กู้คืนเฟิร์มแวร์เดิม (เมื่ออยากย้อนกลับ)

```bash
esptool.py --chip esp32s3 --port $PORT erase_flash

esptool.py --chip esp32s3 --port $PORT --baud 921600 \
  write_flash 0x0 backup_spotpear_v2_16MB.bin
```

บอร์ดจะกลับไปเป็นสภาพเดิมเหมือนตอนก่อนแกะทุกอย่าง

---

## 7. แก้ปัญหาที่เจอบ่อย

| อาการ | วิธีแก้ |
|---|---|
| `Failed to connect` / `No serial data received` | เข้าโหมดดาวน์โหลด (กด BOOT ค้างตอนเสียบสาย), เปลี่ยนสาย USB |
| หาพอร์ตไม่เจอใน `/dev/cu.*` | ใช้สายที่ส่งข้อมูลได้, ลองพอร์ต USB อื่นบน Mac |
| อ่านหรือเขียนแล้วขึ้น error กลางทาง | ลด baud เหลือ `460800` หรือ `115200` |
| เขียนเสร็จแต่จอดำ บอร์ดรีบูตวนซ้ำ | เฟิร์มแวร์ไม่ตรงรุ่นบอร์ด (V1/V2) หรือ offset ผิด → ให้กู้คืนจากไฟล์สำรอง |
| พอร์ตหายหลังเขียนเฟิร์มแวร์ใหม่ | เฟิร์มแวร์ปิด USB-CDC ไว้ → เข้าโหมดดาวน์โหลดด้วยปุ่ม BOOT |
| ดู log ตอนบอร์ดบูต | `python -m serial.tools.miniterm $PORT 115200` |

---

## Checklist

- [ ] `flash_id` แล้วได้ 16MB
- [ ] สำรองไฟล์ `backup_spotpear_v2_16MB.bin` แล้ว (ได้ 16M พอดี)
- [ ] เก็บ hash และสำเนาไฟล์สำรองไว้นอกเครื่องแล้ว
- [ ] เตรียมเฟิร์มแวร์ใหม่ที่ตรงกับบอร์ด V2.0 แล้ว
- [ ] `erase_flash` → `write_flash` → รีเซ็ต → ทดสอบ
