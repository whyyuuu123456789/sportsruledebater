"""Model access for the council and the video review. Uses litellm when it's installed; otherwise talks to Ollama
directly, so no packages are required.

    LLM_MODEL      the advocates and the case builder (default ollama_chat/llama3.1:8b)
    VISION_MODEL   watches the video frames (default ollama_chat/qwen2.5vl:7b)
"""

from __future__ import annotations

import base64
import json
import os
import sys
import time
import urllib.error
import urllib.request
from typing import Protocol

MODEL = os.environ.get("LLM_MODEL", "ollama_chat/llama3.1:8b")
VISION_MODEL = os.environ.get("VISION_MODEL", "ollama_chat/qwen2.5vl:7b")
OLLAMA = os.environ.get("OLLAMA_API_BASE", "http://localhost:11434")


class Chat(Protocol):
    def __call__(self, system: str, user: str, max_tokens: int, seed: int,
                 images: tuple[bytes, ...] = (), json_mode: bool = False) -> str: ...


def wait_for_ollama(timeout: float = 30) -> None:
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            urllib.request.urlopen(f"{OLLAMA}/api/version", timeout=2)
            return
        except OSError:
            time.sleep(0.5)
    sys.exit(f"Couldn't reach Ollama at {OLLAMA}. Start it with: brew services start ollama")


def _ollama(path: str, body: dict, timeout: float = 600) -> dict:
    req = urllib.request.Request(f"{OLLAMA}{path}", json.dumps(body).encode(), {"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.load(resp)


def connect(model: str = MODEL) -> Chat:
    """A chat function for one model: chat(system, user, max_tokens, seed, images=(), json_mode=False) -> reply."""
    try:
        import litellm

        litellm.drop_params = True
        litellm.suppress_debug_info = True
    except ImportError:
        litellm = None
    local = model.startswith("ollama")
    if not local and litellm is None:
        sys.exit(f"{model} needs litellm (pip install litellm). Ollama models work without it.")
    if local:
        wait_for_ollama()
        try:
            _ollama("/api/show", {"model": model.split("/", 1)[-1]}, timeout=30)
        except urllib.error.HTTPError:
            sys.exit(f"Ollama doesn't have {model.split('/', 1)[-1]}. Download it with: "
                     f"ollama pull {model.split('/', 1)[-1]}")

    def chat(system: str, user: str, max_tokens: int, seed: int,
             images: tuple[bytes, ...] = (), json_mode: bool = False) -> str:
        pics = [base64.b64encode(i).decode() for i in images]
        num_ctx = 16384 if pics else 8192  # frames take a few hundred tokens each
        if litellm:
            content: str | list = user
            if pics:
                content = [{"type": "text", "text": user}] + [
                    {"type": "image_url", "image_url": {"url": f"data:image/jpeg;base64,{p}"}} for p in pics]
            extra = {"num_ctx": num_ctx} if local else {}
            if json_mode:
                extra["response_format"] = {"type": "json_object"}
            resp = litellm.completion(model=model, max_tokens=max_tokens, temperature=0.3, seed=seed, **extra,
                                      messages=[{"role": "system", "content": system},
                                                {"role": "user", "content": content}])
            return resp.choices[0].message.content or ""
        message = {"role": "user", "content": user, **({"images": pics} if pics else {})}
        body = {"model": model.split("/", 1)[-1], "stream": False,
                "messages": [{"role": "system", "content": system}, message],
                "options": {"temperature": 0.3, "seed": seed, "num_predict": max_tokens, "num_ctx": num_ctx},
                **({"format": "json"} if json_mode else {})}
        return _ollama("/api/chat", body)["message"]["content"]

    return chat
