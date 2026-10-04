"""Speech-to-text. PCM 16 kHz / 16-bit / mono in, text out.

Providers (config.yaml -> stt.provider):
  groq    Groq-hosted Whisper through its OpenAI-compatible API: free tier, ~1 s (needs GROQ_API_KEY).
  openai  OpenAI transcription API (needs OPENAI_API_KEY).
  local   faster-whisper on the Pi itself: free, no key, but ~7-20 s per sentence on a Pi 4 (see tools/bench_stt.py).
"""
import asyncio
import io
import logging
import time
import wave

log = logging.getLogger("voice-bridge")

# provider -> (base_url, default model); both speak the OpenAI audio-transcription API
CLOUD = {
    "openai": (None, "gpt-4o-mini-transcribe"),
    "groq": ("https://api.groq.com/openai/v1", "whisper-large-v3-turbo"),
}


def pcm_to_wav(pcm: bytes, sample_rate: int = 16000) -> bytes:
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sample_rate)
        w.writeframes(pcm)
    return buf.getvalue()


class STT:
    def __init__(self, cfg: dict, api_key: str = "", sample_rate: int = 16000):
        self.provider = cfg.get("provider", "openai")
        self.language = cfg.get("language", "th")
        self.sample_rate = sample_rate
        if self.provider == "local":
            from faster_whisper import WhisperModel   # imported lazily: only this provider needs it

            name = cfg.get("model", "small")
            t0 = time.monotonic()
            self.model = WhisperModel(
                name,
                device="cpu",
                compute_type=cfg.get("compute_type", "int8"),
                cpu_threads=int(cfg.get("cpu_threads", 4)),
            )
            self.beam_size = int(cfg.get("beam_size", 1))
            log.info("local STT model '%s' loaded in %.1f s", name, time.monotonic() - t0)
        elif self.provider in CLOUD:
            from openai import AsyncOpenAI

            base_url, default_model = CLOUD[self.provider]
            self.client = AsyncOpenAI(api_key=api_key, base_url=cfg.get("base_url", base_url), timeout=30, max_retries=1)
            self.model_name = cfg.get("model", default_model)
        else:
            raise ValueError(f"unsupported STT provider: {self.provider}")

    async def transcribe(self, pcm: bytes) -> str:
        if self.provider == "local":
            return await asyncio.to_thread(self._transcribe_local, pcm)   # keep the event loop free
        wav = pcm_to_wav(pcm, self.sample_rate)
        res = await self.client.audio.transcriptions.create(
            model=self.model_name,
            file=("speech.wav", wav, "audio/wav"),
            language=self.language,
        )
        return (res.text or "").strip()

    def _transcribe_local(self, pcm: bytes) -> str:
        import numpy as np

        audio = np.frombuffer(pcm, dtype=np.int16).astype(np.float32) / 32768.0
        t0 = time.monotonic()
        segments, _ = self.model.transcribe(
            audio,
            language=self.language,
            beam_size=self.beam_size,
            vad_filter=True,                    # drops silence, which Whisper otherwise "hears" words in
            condition_on_previous_text=False,
        )
        text = " ".join(s.text.strip() for s in segments).strip()
        log.info("local STT: %.1f s of audio -> %.1f s", len(audio) / self.sample_rate, time.monotonic() - t0)
        return text
