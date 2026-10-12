"""Watching the video: ffmpeg pulls frames, and a vision model says what it sees in them.

Frames are spread evenly over the clip, or packed around the moment named with --at. For each frame the vision
model describes who is involved (team colours, shirt numbers), the ball, any official and their signal, and any
on-screen text such as the score, the clock or a VAR caption. It's told to describe only what it can see.
"""

from __future__ import annotations

import re
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

from llm import Chat

BATCH = 3  # frames per vision call
END_MARGIN = 0.25  # seconds: the last frame of a clip comes a little before its stated end
SYSTEM = "You review video frames of a sports incident for a rules review. You describe only what is visible."
LINE = re.compile(r"t\s*=\s*(\d+(?:\.\d+)?)\s*s?\s*[:\-–]\s*(.+)", re.IGNORECASE)


@dataclass
class Observation:
    t: float
    text: str

    def __str__(self) -> str:
        return f"[video {stamp(self.t)}] {self.text}"


def stamp(t: float) -> str:
    return f"{int(t // 60)}:{t % 60:04.1f}"


def seconds(text: str) -> float:
    """'1:02.5' or '62.5' -> 62.5"""
    total = 0.0
    for part in text.split(":"):
        total = total * 60 + float(part)
    return total


def duration(video: Path) -> float:
    out = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", str(video)],
                         capture_output=True, text=True, check=True).stdout
    return float(out.strip())


def frame_times(length: float, count: int, at: float | None = None, window: float = 3.0) -> list[float]:
    """count times spread over the clip, or over [at - window, at + window]."""
    start, end = (max(0.0, at - window), at + window) if at is not None else (0.0, length)
    end = max(start, min(end, length - END_MARGIN))
    if count <= 1:
        return [round((start + end) / 2, 2)]
    return [round(start + i * (end - start) / (count - 1), 2) for i in range(count)]


def extract_frames(video: Path, times: list[float], out_dir: Path, width: int = 768) -> list[tuple[float, Path]]:
    """One JPEG per time. A time ffmpeg can't get a frame for is skipped."""
    out_dir.mkdir(parents=True, exist_ok=True)
    frames = []
    for t in times:
        path = out_dir / f"frame_{t:07.2f}.jpg"
        done = subprocess.run(["ffmpeg", "-v", "error", "-y", "-ss", f"{t:.2f}", "-i", str(video), "-frames:v", "1",
                               "-vf", f"scale='min({width},iw)':-2", "-q:v", "3", str(path)], capture_output=True)
        if done.returncode == 0 and path.exists() and path.stat().st_size:
            frames.append((t, path))
    if not frames:
        raise RuntimeError(f"ffmpeg couldn't read any frames from {video.name}")
    return frames


def parse_observations(reply: str, times: list[float]) -> list[Observation]:
    """One 't=12.5s: ...' line per frame. A line whose time isn't one of these frames' (an echoed example, or a
    made-up moment) is dropped. A reply in any other shape is kept whole, at the first frame's time."""
    seen: dict[float, str] = {}
    for m in filter(None, map(LINE.search, reply.splitlines())):
        t = min(times, key=lambda x: abs(x - float(m.group(1))))
        if abs(t - float(m.group(1))) <= 0.1:
            seen[t] = f"{seen[t]} {m.group(2).strip()}" if t in seen else m.group(2).strip()
    found = [Observation(t, text) for t, text in seen.items()]
    if not found and reply.strip():
        found = [Observation(times[0], " ".join(reply.split()))]
    return found


def watch(video: Path, context: str, look: Chat, out_dir: Path, count: int = 12, at: float | None = None,
          window: float = 3.0, log: Callable[[str], None] | None = None) -> list[Observation]:
    log = log or print
    frames = extract_frames(video, frame_times(duration(video), count, at, window), out_dir)
    observations: list[Observation] = []
    for i in range(0, len(frames), BATCH):
        batch_times, batch = [t for t, _ in frames[i:i + BATCH]], [f for _, f in frames[i:i + BATCH]]
        user = (f"What the person who sent the video says: {context or 'nothing'}\n"
                f"These {len(batch)} frames are from the video at "
                + ", ".join(f"t={t:g}s" for t in batch_times) + ", in that order.\n"
                "For each frame, write one line that starts with its time, like 't=<time>s: <what you see>'. Describe what matters "
                "for an officiating decision: who is involved (team colours, shirt numbers), where the ball is, any "
                "official and their signal, and any on-screen text (score, clock, names, captions). Describe only "
                "what you can see; don't guess names, scores or outcomes.")
        reply = look(SYSTEM, user, 120 * len(batch), 7 + i, images=tuple(f.read_bytes() for f in batch))
        observations += parse_observations(reply, batch_times)
        log(f"  watched {stamp(batch_times[0])} to {stamp(batch_times[-1])}")
    return observations
