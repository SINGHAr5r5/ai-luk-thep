"""Test client: plays the role of the board.

Sends an audio file (any format ffmpeg can read) as one push-to-talk turn and
saves the spoken reply to a WAV file.

  python tools/send_wav.py question.wav --out reply.wav
  python tools/send_wav.py --say "สวัสดี วันนี้อากาศเป็นยังไง" --out reply.wav   # makes the question with edge-tts

The device token is read from ~/voice-bridge/.env (DEVICE_TOKENS, first entry)
unless --token is given.
"""
import argparse
import asyncio
import json
import os
import subprocess
import sys
import time
import wave
from pathlib import Path

from websockets.asyncio.client import connect

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from tts import TTS  # noqa: E402

RATE = 16000


def file_to_pcm(path: str) -> bytes:
    return subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-i", path,
         "-f", "s16le", "-ac", "1", "-ar", str(RATE), "pipe:1"],
        check=True, capture_output=True).stdout


def default_token() -> str:
    env = Path(os.path.expanduser("~/voice-bridge/.env"))
    for line in env.read_text().splitlines():
        if line.startswith("DEVICE_TOKENS="):
            return line.split("=", 1)[1].split(",")[0].strip()
    sys.exit("no DEVICE_TOKENS in ~/voice-bridge/.env; pass --token")


async def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("audio", nargs="?")
    ap.add_argument("--say", help="synthesize this Thai text as the question instead of reading a file")
    ap.add_argument("--text", help="send typed text (skips STT) instead of audio")
    ap.add_argument("--url", default="ws://127.0.0.1:8765/ws")
    ap.add_argument("--token")
    ap.add_argument("--device", default="test-client")
    ap.add_argument("--out", default="reply.wav")
    args = ap.parse_args()

    pcm = b""
    if args.say:
        pcm = await TTS({"voice": "th-TH-NiwatNeural"}).synth(args.say)
    elif args.audio:
        pcm = file_to_pcm(args.audio)
    elif not args.text:
        sys.exit("give an audio file, --say or --text")

    reply = bytearray()
    t0 = time.monotonic()
    first_audio = None
    async with connect(args.url, max_size=2**22) as ws:
        await ws.send(json.dumps({"type": "hello", "device_id": args.device,
                                  "fw_version": "test", "token": args.token or default_token()}))
        if args.text:
            await ws.send(json.dumps({"type": "text", "text": args.text}))
        else:
            await ws.send(json.dumps({"type": "listen_start"}))
            for i in range(0, len(pcm), 1280):
                await ws.send(pcm[i:i + 1280])
            await ws.send(json.dumps({"type": "listen_stop"}))
        t_stop = time.monotonic()
        seen_turn = False
        async for msg in ws:
            if isinstance(msg, bytes):
                if first_audio is None:
                    first_audio = time.monotonic() - t_stop
                reply += msg
                continue
            m = json.loads(msg)
            print(f"[{time.monotonic() - t0:5.1f}s] {m}")
            if m.get("type") == "state" and m.get("value") in ("thinking", "speaking"):
                seen_turn = True
            if seen_turn and m.get("type") == "state" and m.get("value") == "idle":
                break

    with wave.open(args.out, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE); w.writeframes(bytes(reply))
    secs = len(reply) / (RATE * 2)
    print(f"saved {args.out}: {secs:.1f}s of audio; first audio {first_audio:.1f}s after listen_stop"
          if first_audio is not None else f"saved {args.out}: no audio received")


if __name__ == "__main__":
    asyncio.run(main())
