"""Benchmark the configured STT on the Pi: synthesize Thai sentences with edge-tts, transcribe, time it.

  cd ~/voice-bridge && venv/bin/python tools/bench_stt.py
Synthetic speech is cleaner than a real microphone, so accuracy here is an upper bound.
"""
import asyncio
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import yaml

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from stt import STT  # noqa: E402

SENTENCES = [
    "สวัสดีครับ วันนี้อากาศเป็นยังไงบ้าง",
    "ช่วยเปิดไฟในห้องนอนให้หน่อยได้ไหม",
    "พรุ่งนี้ตอนเช้ามีนัดกี่โมง แล้วต้องเตรียมอะไรบ้าง",
]


async def synth_pcm(text: str, voice: str = "th-TH-NiwatNeural") -> bytes:
    import edge_tts

    with tempfile.TemporaryDirectory() as d:
        mp3 = Path(d) / "s.mp3"
        await edge_tts.Communicate(text, voice).save(str(mp3))
        return subprocess.run(
            ["ffmpeg", "-v", "error", "-i", str(mp3), "-f", "s16le", "-ac", "1", "-ar", "16000", "-"],
            capture_output=True, check=True,
        ).stdout


async def main() -> None:
    cfg = yaml.safe_load((Path(__file__).resolve().parent.parent / "config.yaml").read_text())
    if len(sys.argv) > 1:                       # e.g. bench_stt.py base
        cfg["stt"]["model"] = sys.argv[1]
    t0 = time.monotonic()
    import os
    key = os.environ.get("STT_API_KEY", "")   # for cloud providers: STT_API_KEY=... bench_stt.py
    stt = STT(cfg["stt"], key, 16000)
    print(f"provider={cfg['stt']['provider']} model={cfg['stt'].get('model')} loaded in {time.monotonic() - t0:.1f} s")
    for text in SENTENCES:
        pcm = await synth_pcm(text)
        secs = len(pcm) / 32000
        t = time.monotonic()
        heard = await stt.transcribe(pcm)
        print(f"\naudio {secs:.1f} s -> STT {time.monotonic() - t:.1f} s")
        print(f"  said : {text}\n  heard: {heard}")


asyncio.run(main())
