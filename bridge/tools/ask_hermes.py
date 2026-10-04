"""Ask the local Hermes API one question, reading API_SERVER_KEY exactly like bridge.py does.

  cd ~/voice-bridge && venv/bin/python tools/ask_hermes.py "ตอบสั้นๆ ว่า สวัสดี"
Prints the HTTP status, how long it took and the reply; never prints a key.
"""
import json
import os
import sys
import time
import urllib.error
import urllib.request


def env_value(name: str) -> str:
    for path in ("~/.hermes/.env", "~/voice-bridge/.env"):      # later file wins, same as the bridge
        p = os.path.expanduser(path)
        if os.path.exists(p):
            for line in open(p).read().splitlines():
                if line.startswith(name + "="):
                    val = line.split("=", 1)[1].strip().strip('"').strip("'")
    return val if "val" in locals() else ""


key = env_value("API_SERVER_KEY")
text = sys.argv[1] if len(sys.argv) > 1 else "ตอบสั้นๆ ว่า สวัสดี"
req = urllib.request.Request(
    "http://127.0.0.1:8642/v1/chat/completions",
    data=json.dumps({"model": "hermes-agent", "messages": [{"role": "user", "content": text}]}).encode(),
    headers={"Authorization": f"Bearer {key}", "Content-Type": "application/json"},
)
t0 = time.monotonic()
try:
    with urllib.request.urlopen(req, timeout=float(os.environ.get("TIMEOUT", "60"))) as r:
        body = json.loads(r.read())
        reply = body["choices"][0]["message"]["content"]
        print(f"HTTP {r.status} in {time.monotonic() - t0:.1f} s\nreply: {reply[:300]}")
except urllib.error.HTTPError as e:
    print(f"HTTP {e.code} in {time.monotonic() - t0:.1f} s: {e.read().decode(errors='replace')[:300]}")
except Exception as e:
    print(f"failed after {time.monotonic() - t0:.1f} s: {type(e).__name__}: {e}")
