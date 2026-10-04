"""Sentence splitting and markdown/emoji cleanup for spoken replies."""
import re
from typing import AsyncIterator

_HARD_END = re.compile(r"[.!?。！？\n]")
_EMOJI = re.compile(
    "[\U0001F000-\U0001FAFF\U00002600-\U000027BF\U0001F1E6-\U0001F1FF‍️]",
    flags=re.UNICODE,
)

SOFT_SPLIT_CHARS = 60   # Thai has no full stops: split at a space once a sentence gets this long
MIN_SENTENCE_CHARS = 12


def strip_markdown(text: str) -> str:
    text = re.sub(r"```.*?```", " ", text, flags=re.S)
    text = re.sub(r"`([^`]*)`", r"\1", text)
    text = re.sub(r"!?\[([^\]]*)\]\([^)]*\)", r"\1", text)
    text = re.sub(r"https?://\S+", " ", text)
    text = re.sub(r"^\s{0,3}(#{1,6}|>|[-*+]|\d+\.)\s+", "", text, flags=re.M)
    text = re.sub(r"[*_~|#]+", "", text)
    text = _EMOJI.sub("", text)
    return re.sub(r"\s+", " ", text).strip()


def _cut(buf: str):
    """Return (sentence, rest) if buf holds a complete sentence, else None."""
    m = _HARD_END.search(buf)
    if m and m.end() >= MIN_SENTENCE_CHARS:
        return buf[: m.end()], buf[m.end():]
    if len(buf) >= SOFT_SPLIT_CHARS:
        cut = buf.rfind(" ", MIN_SENTENCE_CHARS, len(buf))
        if cut > 0:
            return buf[:cut], buf[cut + 1:]
    return None


async def split_sentences(deltas: AsyncIterator[str]) -> AsyncIterator[str]:
    buf = ""
    async for delta in deltas:
        buf += delta
        while (res := _cut(buf)) is not None:
            sentence, buf = res
            if sentence.strip():
                yield sentence.strip()
    if buf.strip():
        yield buf.strip()


import re as _re

# Hermes reports provider/model failures as ordinary reply text ("Model 'x' isn't available ...").
# Read aloud by a Thai voice that is just noise, so recognise it and say something useful instead.
_HERMES_ERROR = _re.compile(
    r"isn'?t available|No active credentials|Error code: \d{3}|rate.?limit|hermes model|API call failed|"
    r"model_not_found|insufficient_quota|Unauthorized|Invalid API key",
    _re.IGNORECASE,
)
HERMES_ERROR_SPOKEN = "ตอนนี้ลูกเทพต่อกับโมเดลไม่ได้ค่ะ ลองใหม่อีกครั้งนะคะ"


def looks_like_hermes_error(text: str) -> bool:
    return bool(_HERMES_ERROR.search(text)) and not _re.search(r"[\u0E00-\u0E7F]", text)   # an error in English, no Thai in it
