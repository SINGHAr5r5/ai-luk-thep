#pragma once

// Phase 4 bench tools. Push-to-talk loopback (hold BOOT, speak, release -> it plays back; max 4 s so the
// 5 s Wi-Fi-reset hold is never reached) and console helpers that need nobody to speak.
void audio_test_init(void);
void audio_test_beep(int freq_hz, int ms);
void audio_test_selftest(void);   // speaker plays 1 kHz, the microphone must hear it
void audio_test_levels(void);     // 2 s of microphone level meter
