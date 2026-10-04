# voice-bridge (runs on the Pi)

Board ⇄ WebSocket ⇄ STT (OpenAI) → Hermes API (Responses, streaming) → TTS (edge-tts) ⇄ Board.
Implements plan section 8 (`../hermes-voice-box-plan.md`).

## Where things live

| What | Where |
|---|---|
| Source (git) | this folder, on the Mac: `~/Documents/MyWork/gitlab/iot/aiBot/bridge` |
| Running copy | Pi: `~/voice-bridge` (installed by `deploy.sh`, so it keeps running when the Mac is off) |
| Service | Pi user unit `voice-bridge` (`systemctl --user status voice-bridge`), starts at boot |
| Hermes API | Pi: `127.0.0.1:8642`, enabled via `API_SERVER_ENABLED`/`API_SERVER_KEY` in `~/.hermes/.env` |
| Secrets | `~/.hermes/.env` (`API_SERVER_KEY`, `OPENAI_API_KEY`), then `~/voice-bridge/.env` (`DEVICE_TOKENS`, overrides) — never in git |

Deploy after editing: `bash ~/mac/gitlab/iot/aiBot/bridge/deploy.sh` (on the Pi).

## Board connection

- URL: `ws://<pi-lan-ip>:8765/ws` (Pi is currently `192.168.1.57`; reserve it in the router). LAN/loopback clients only.
- Token: first entry of `DEVICE_TOKENS` in the Pi's `~/voice-bridge/.env`. This is a **bridge token, separate from the OTA token.**
- Audio both ways: raw PCM s16le, 16 kHz, mono. Bridge sends 40 ms frames (1280 bytes), paced ~real time with ≤0.6 s lead.

## Protocol (plan 8.4 plus these additions)

Board → Pi
| Message | Notes |
|---|---|
| `{"type":"hello","device_id","fw_version","token"}` | must be the first message, within 10 s |
| `{"type":"listen_start"}`, binary PCM, `{"type":"listen_stop"}` | push-to-talk; `listen_start` also cancels a reply in progress |
| `{"type":"abort"}` | stop current reply |
| `{"type":"reset"}` | **added**: start a new conversation (e.g. PWR double-press) |
| `{"type":"text","text"}` | **added**: typed input, skips STT (used by the test tool) |
| `{"type":"ping"}` | **added**: replies `{"type":"pong"}` |

Pi → Board
| Message | Notes |
|---|---|
| `{"type":"hello","ok":true}` | **added**: hello accepted (bad token → `error` "unauthorized" + close 1008) |
| `{"type":"state","value":"idle"\|"listening"\|"thinking"\|"speaking"}` | drive the animation from these |
| `{"type":"stt","text"}` | what was heard |
| `{"type":"reply_text","text","final":false}` | **one per spoken sentence**, sent just before its audio (subtitle) |
| `{"type":"reply_text","text","final":true}` | whole reply at the end |
| `{"type":"tts_start"}`, binary PCM, `{"type":"tts_end"}` | spoken reply |
| `{"type":"error","msg","stage"?}` | short Thai message; details only in the Pi log |

Conversation memory is kept by Hermes (`conversation` = `voicebox-<device_id>-<start>`); a new one starts after 10 min idle or on `reset`.
If Hermes has produced nothing after 4 s, the bridge speaks "ขอคิดแป๊บนึงนะ".

## Test without the board (Phase 5 acceptance)

```bash
cd ~/voice-bridge
venv/bin/python tools/send_wav.py --say "สวัสดีครับ วันนี้อากาศเป็นยังไง" --out reply.wav   # full path incl. STT
venv/bin/python tools/send_wav.py question.wav --out reply.wav                         # any audio file
venv/bin/python tools/send_wav.py --text "สวัสดีครับ" --out reply.wav                  # skip STT
```
