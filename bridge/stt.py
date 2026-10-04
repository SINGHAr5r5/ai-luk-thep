"""Speech-to-text. PCM 16 kHz / 16-bit / mono in, text out."""
import io
import wave

from openai import AsyncOpenAI


def pcm_to_wav(pcm: bytes, sample_rate: int = 16000) -> bytes:
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sample_rate)
        w.writeframes(pcm)
    return buf.getvalue()


class STT:
    def __init__(self, cfg: dict, api_key: str, sample_rate: int = 16000):
        if cfg.get("provider", "openai") != "openai":
            raise ValueError(f"unsupported STT provider: {cfg.get('provider')}")
        self.client = AsyncOpenAI(api_key=api_key, timeout=30, max_retries=1)
        self.model = cfg.get("model", "gpt-4o-mini-transcribe")
        self.language = cfg.get("language", "th")
        self.sample_rate = sample_rate

    async def transcribe(self, pcm: bytes) -> str:
        wav = pcm_to_wav(pcm, self.sample_rate)
        res = await self.client.audio.transcriptions.create(
            model=self.model,
            file=("speech.wav", wav, "audio/wav"),
            language=self.language,
        )
        return (res.text or "").strip()
