"""Hermes voice bridge: board <-> WebSocket <-> STT -> Hermes -> TTS.

Protocol: see ../hermes-voice-box-plan.md section 8.4 and README.md in this folder.
"""
import asyncio
import hmac
import ipaddress
import json
import logging
import os
import sys
from dataclasses import dataclass, field
from pathlib import Path

import yaml
from websockets.asyncio.server import ServerConnection, serve
from websockets.exceptions import ConnectionClosed

from hermes_client import HermesClient
from stt import STT
from text_utils import split_sentences, strip_markdown
from tts import TTS

log = logging.getLogger("voice-bridge")
HERE = Path(__file__).resolve().parent


def load_env_files(paths: list[str]) -> dict[str, str]:
    env: dict[str, str] = {}
    for p in paths:
        path = Path(os.path.expanduser(p))
        if not path.exists():
            continue
        for line in path.read_text().splitlines():
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                k, v = line.split("=", 1)
                env[k.strip()] = v.strip().strip('"').strip("'")
    env.update({k: v for k, v in os.environ.items() if k in env or k.startswith(("API_SERVER_", "OPENAI_", "DEVICE_"))})
    return env


def client_allowed(ip: str) -> bool:
    addr = ipaddress.ip_address(ip.split("%")[0])
    if addr.version == 6 and addr.ipv4_mapped:
        addr = addr.ipv4_mapped
    return addr.is_loopback or addr.is_private


@dataclass
class Session:
    ws: ServerConnection
    device_id: str
    listening: bool = False
    audio: bytearray = field(default_factory=bytearray)
    task: asyncio.Task | None = None


class Bridge:
    def __init__(self, cfg: dict, env: dict[str, str]):
        self.cfg = cfg
        a = cfg["audio"]
        self.rate = int(a.get("sample_rate", 16000))
        self.bytes_per_s = self.rate * 2
        self.max_utt_bytes = int(a.get("max_utterance_s", 60)) * self.bytes_per_s
        self.chunk_bytes = int(self.bytes_per_s * a.get("chunk_ms", 40) / 1000) & ~1
        self.max_lead_s = float(a.get("max_lead_s", 0.6))
        self.min_audio_bytes = int(cfg["stt"].get("min_audio_ms", 300) * self.bytes_per_s / 1000)

        self.tokens = [t.strip() for t in env.get("DEVICE_TOKENS", "").split(",") if t.strip()]
        if not self.tokens:
            sys.exit("DEVICE_TOKENS is empty; refusing to start")
        for key in ("API_SERVER_KEY", "OPENAI_API_KEY"):
            if not env.get(key):
                sys.exit(f"{key} missing from secrets files")

        self.stt = STT(cfg["stt"], env["OPENAI_API_KEY"], self.rate)
        self.tts = TTS(cfg["tts"], self.rate)
        self.hermes = HermesClient(cfg["hermes"], env["API_SERVER_KEY"])
        self.filler_after = float(cfg["hermes"].get("thinking_filler_after_s", 4))
        self.filler_text = cfg["hermes"].get("thinking_filler_text", "")
        self.filler_pcm = b""

    # ---------- sending helpers ----------
    @staticmethod
    async def send_json(ws: ServerConnection, **msg) -> None:
        await ws.send(json.dumps(msg, ensure_ascii=False))

    # ---------- connection ----------
    async def handler(self, ws: ServerConnection) -> None:
        peer = ws.remote_address[0] if ws.remote_address else "?"
        if not client_allowed(peer):
            log.warning("rejected non-LAN client %s", peer)
            await ws.close(1008, "lan only")
            return
        if ws.request.path.split("?")[0] != self.cfg["server"].get("path", "/ws"):
            await ws.close(1008, "bad path")
            return
        try:
            hello = json.loads(await asyncio.wait_for(ws.recv(), timeout=10))
        except (asyncio.TimeoutError, ValueError, TypeError, ConnectionClosed):
            await ws.close(1008, "hello required")
            return
        token = str(hello.get("token", ""))
        if hello.get("type") != "hello" or not any(hmac.compare_digest(token, t) for t in self.tokens):
            log.warning("bad hello/token from %s", peer)
            await self.send_json(ws, type="error", msg="unauthorized")
            await ws.close(1008, "unauthorized")
            return

        s = Session(ws=ws, device_id=str(hello.get("device_id") or peer))
        log.info("device %s connected from %s fw=%s", s.device_id, peer, hello.get("fw_version"))
        await self.send_json(ws, type="hello", ok=True)
        await self.send_json(ws, type="state", value="idle")
        try:
            async for msg in ws:
                if isinstance(msg, bytes):
                    if s.listening and len(s.audio) + len(msg) <= self.max_utt_bytes:
                        s.audio += msg
                    continue
                await self.on_control(s, msg)
        except ConnectionClosed:
            pass
        finally:
            if s.task:
                s.task.cancel()
            log.info("device %s disconnected", s.device_id)

    async def on_control(self, s: Session, raw: str) -> None:
        try:
            msg = json.loads(raw)
        except ValueError:
            return
        t = msg.get("type")
        if t == "listen_start":
            await self.cancel_turn(s)
            s.audio = bytearray()
            s.listening = True
            await self.send_json(s.ws, type="state", value="listening")
        elif t == "listen_stop":
            if not s.listening:
                return
            s.listening = False
            pcm, s.audio = bytes(s.audio), bytearray()
            s.task = asyncio.create_task(self.handle_utterance(s, pcm))
        elif t == "text":   # typed input (testing / future UI): skips STT
            await self.cancel_turn(s)
            s.task = asyncio.create_task(self.handle_utterance(s, b"", text=str(msg.get("text", ""))))
        elif t == "abort":
            s.listening = False
            await self.cancel_turn(s)
            await self.send_json(s.ws, type="state", value="idle")
        elif t == "reset":
            self.hermes.reset(s.device_id)
            await self.send_json(s.ws, type="state", value="idle")
        elif t == "ping":
            await self.send_json(s.ws, type="pong")

    async def cancel_turn(self, s: Session) -> None:
        if s.task and not s.task.done():
            s.task.cancel()
            try:
                await s.task
            except (asyncio.CancelledError, Exception):
                pass
        s.task = None

    # ---------- one turn ----------
    async def handle_utterance(self, s: Session, pcm: bytes, text: str | None = None) -> None:
        ws = s.ws
        tts_started = False
        stage = "stt"
        try:
            if text is None:
                if len(pcm) < self.min_audio_bytes:
                    await self.send_json(ws, type="error", msg="เสียงสั้นเกินไป")
                    await self.send_json(ws, type="state", value="idle")
                    return
                await self.send_json(ws, type="state", value="thinking")
                text = await self.stt.transcribe(pcm)
                log.info("[%s] STT: %s", s.device_id, text)
                await self.send_json(ws, type="stt", text=text)
            else:
                await self.send_json(ws, type="state", value="thinking")
            stage = "hermes"
            if not text:
                await self.send_json(ws, type="error", msg="ไม่ได้ยินเสียงพูด")
                await self.send_json(ws, type="state", value="idle")
                return

            queue: asyncio.Queue[bytes | None] = asyncio.Queue(maxsize=4)
            first_ready = asyncio.Event()
            reply: list[str] = []

            async def produce() -> None:
                async for sentence in split_sentences(self.hermes.ask_stream(text, s.device_id)):
                    spoken = strip_markdown(sentence)
                    if not spoken:
                        continue
                    reply.append(spoken)
                    audio = await self.tts.synth(spoken)
                    first_ready.set()
                    await self.send_json(ws, type="reply_text", text=spoken, final=False)
                    await queue.put(audio)
                first_ready.set()
                await queue.put(None)

            async def filler() -> None:
                try:
                    await asyncio.wait_for(first_ready.wait(), self.filler_after)
                except asyncio.TimeoutError:
                    if self.filler_pcm:
                        await queue.put(self.filler_pcm)

            producer = asyncio.create_task(produce())
            filler_task = asyncio.create_task(filler())
            loop = asyncio.get_running_loop()
            start, sent_s = loop.time(), 0.0
            try:
                while (audio := await queue.get()) is not None:
                    if not audio:
                        continue
                    if not tts_started:
                        await self.send_json(ws, type="state", value="speaking")
                        await self.send_json(ws, type="tts_start")
                        tts_started = True
                        start, sent_s = loop.time(), 0.0
                    for i in range(0, len(audio), self.chunk_bytes):
                        now = loop.time()
                        if now - start > sent_s:          # board drained its buffer: re-anchor
                            start = now - sent_s
                        lead = sent_s - (now - start)
                        if lead > self.max_lead_s:
                            await asyncio.sleep(lead - self.max_lead_s)
                        chunk = audio[i:i + self.chunk_bytes]
                        await ws.send(chunk)
                        sent_s += len(chunk) / self.bytes_per_s
                await producer
            finally:
                filler_task.cancel()
                producer.cancel()

            log.info("[%s] reply: %s", s.device_id, " ".join(reply))
            await self.send_json(ws, type="reply_text", text=" ".join(reply), final=True)
        except asyncio.CancelledError:
            raise
        except ConnectionClosed:
            return
        except Exception:  # report and recover; never kill the connection
            log.exception("[%s] turn failed at %s", s.device_id, stage)
            # Details stay in the Pi log: provider errors can echo parts of API keys.
            friendly = {"stt": "ฟังเสียงไม่สำเร็จ", "hermes": "ลูกเทพตอบไม่สำเร็จ"}[stage]
            try:
                await self.send_json(ws, type="error", msg=friendly, stage=stage)
            except ConnectionClosed:
                return
        finally:
            try:
                if tts_started:
                    await self.send_json(ws, type="tts_end")
                await self.send_json(ws, type="state", value="idle")
            except ConnectionClosed:
                pass

    async def run(self) -> None:
        if self.filler_text:
            try:
                self.filler_pcm = await self.tts.synth(self.filler_text)
            except Exception:
                log.warning("could not pre-synthesize filler; continuing without it")
        srv = self.cfg["server"]
        async with serve(self.handler, srv.get("host", "0.0.0.0"), int(srv.get("port", 8765)),
                         max_size=2**20, ping_interval=20, ping_timeout=20) as server:
            log.info("listening on ws://%s:%s%s", srv.get("host"), srv.get("port"), srv.get("path", "/ws"))
            await server.serve_forever()


def main() -> None:
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    cfg_path = Path(sys.argv[1]) if len(sys.argv) > 1 else HERE / "config.yaml"
    cfg = yaml.safe_load(cfg_path.read_text())
    env = load_env_files(cfg.get("secrets", []))
    asyncio.run(Bridge(cfg, env).run())


if __name__ == "__main__":
    main()
