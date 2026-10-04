"""Text-to-speech with edge-tts, decoded to PCM 16 kHz / 16-bit / mono via ffmpeg."""
import asyncio

import edge_tts


async def mp3_to_pcm(mp3: bytes, sample_rate: int = 16000) -> bytes:
    proc = await asyncio.create_subprocess_exec(
        "ffmpeg", "-hide_banner", "-loglevel", "error",
        "-f", "mp3", "-i", "pipe:0",
        "-f", "s16le", "-ac", "1", "-ar", str(sample_rate), "pipe:1",
        stdin=asyncio.subprocess.PIPE,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
    )
    out, err = await proc.communicate(mp3)
    if proc.returncode != 0:
        raise RuntimeError(f"ffmpeg failed: {err.decode(errors='replace')[:200]}")
    return out


class TTS:
    def __init__(self, cfg: dict, sample_rate: int = 16000):
        if cfg.get("provider", "edge") != "edge":
            raise ValueError(f"unsupported TTS provider: {cfg.get('provider')}")
        self.voice = cfg.get("voice", "th-TH-PremwadeeNeural")
        self.rate = cfg.get("rate", "+0%")
        self.sample_rate = sample_rate

    async def synth(self, text: str) -> bytes:
        """Synthesize one sentence and return raw PCM."""
        mp3 = bytearray()
        async for chunk in edge_tts.Communicate(text, self.voice, rate=self.rate).stream():
            if chunk["type"] == "audio":
                mp3 += chunk["data"]
        if not mp3:
            return b""
        return await mp3_to_pcm(bytes(mp3), self.sample_rate)
