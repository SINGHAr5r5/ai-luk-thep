"""Streaming client for the Hermes API server (OpenAI Responses API).

Conversation memory lives on the Hermes side: every request carries a
`conversation` name, and Hermes chains it to the previous response.
"""
import time
from typing import AsyncIterator

from openai import AsyncOpenAI


class HermesClient:
    def __init__(self, cfg: dict, api_key: str):
        self.client = AsyncOpenAI(base_url=cfg["base_url"], api_key=api_key)
        self.model = cfg.get("model", "hermes-agent")
        self.instructions = cfg.get("instructions")
        self.idle_s = float(cfg.get("session_idle_minutes", 10)) * 60
        self._sessions: dict[str, tuple[str, float]] = {}   # device_id -> (conversation, last_used)

    def conversation_for(self, device_id: str) -> str:
        now = time.time()
        conv, last = self._sessions.get(device_id, (None, 0.0))
        if conv is None or now - last > self.idle_s:
            conv = f"voicebox-{device_id}-{int(now)}"
        self._sessions[device_id] = (conv, now)
        return conv

    def reset(self, device_id: str) -> None:
        self._sessions.pop(device_id, None)

    async def ask_stream(self, text: str, device_id: str) -> AsyncIterator[str]:
        stream = await self.client.responses.create(
            model=self.model,
            input=text,
            instructions=self.instructions,
            stream=True,
            extra_body={"conversation": self.conversation_for(device_id)},
        )
        async for ev in stream:
            etype = getattr(ev, "type", "")
            if etype == "response.output_text.delta":
                yield ev.delta
            elif etype in ("response.failed", "error"):
                raise RuntimeError(f"hermes: {getattr(ev, 'error', None) or getattr(ev, 'message', ev)}")
