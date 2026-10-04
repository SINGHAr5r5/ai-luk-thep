#pragma once

// Push-to-talk conversation with Hermes through the bridge: hold BOOT and speak, release, listen.
// Needs Wi-Fi, a working codec and the `bridge_url` / `bridge_token` settings.
void voice_start(void);   // spawns a task that waits for audio, then connects to the bridge
bool voice_configured(void);
void voice_test_ptt(int hold_ms);   // console aid: act as if BOOT were held for hold_ms
