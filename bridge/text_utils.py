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
