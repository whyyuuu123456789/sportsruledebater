"""A local website for the sports rules council. Paste a link to a clip (or upload one), mark when the incident
happens, say what you know, and get a ruling: the advocates discuss the call, and the neutral panel, who watch the
clip themselves, decide it.

    python3 web.py                 # then open http://localhost:8000
    python3 web.py --port 8080

It runs on this machine (Ollama for the models, ffmpeg for the frames) and only listens on localhost. A direct link
to a video file (.mp4, .webm, .mov and so on) is downloaded as it is. A link to a page, such as YouTube, needs yt-dlp
(brew install yt-dlp): check that the site's terms and the footage's copyright allow downloading it first.
"""

from __future__ import annotations

import argparse
import json
import queue
import re
import shutil
import subprocess
import threading
import time
import traceback
import urllib.error
import urllib.parse
import urllib.request
import uuid
from dataclasses import asdict
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import review
from cases import Case, case_from_dict, save_case
from council import MAX_ROUNDS, PANEL, SAVED_HEADER, Council, debate
from evidence import seconds
from intake import IntakeError
from library import load_rulebook
from llm import MODEL, VISION_MODEL, connect

WEB = Path(__file__).resolve().parent / "web"
UPLOADS = Path(__file__).resolve().parent / "uploads"
VIDEO_EXT = (".mp4", ".webm", ".mov", ".mkv", ".m4v", ".avi", ".ogv", ".ogg")
MAX_VIDEO = 500 * 2**20  # bytes, for downloads and uploads
MAX_JSON = 2**20
AGENT = "sportsruledebater/0.1 (local review tool)"
STATIC = {"index.html": "text/html; charset=utf-8", "app.js": "text/javascript; charset=utf-8",
          "style.css": "text/css; charset=utf-8"}

JOBS: dict[str, "Job"] = {}
QUEUE: "queue.Queue[Job]" = queue.Queue()  # one review runs at a time: the models share this machine


def home() -> Path:
    return review.HERE  # read at call time, so tests can point it somewhere else


class Job:
    """One review, from the link to the ruling. The page polls view() for progress."""

    def __init__(self, params: dict):
        self.id = uuid.uuid4().hex[:12]
        self.name = time.strftime("web_%Y%m%d_%H%M%S_") + self.id[:4]
        self.params = params
        self.status = "queued"  # queued, downloading, watching, building, checking, discussing, done, failed
        self.lines: list[str] = []
        self.raw_case: dict = {}
        self.case: dict | None = None
        self.warnings: list[str] = []
        self.result: dict | None = None
        self.error = ""
        self.edited: Case | None = None
        self.go_on = threading.Event()  # set when the user has checked the case
        self.lock = threading.Lock()

    def say(self, text: str) -> None:
        with self.lock:
            self.lines.append(text)

    def view(self, since: int) -> dict:
        with self.lock:
            return {"id": self.id, "name": self.name, "status": self.status, "error": self.error,
                    "lines": self.lines[since:], "next": len(self.lines), "case": self.case,
                    "warnings": self.warnings, "result": self.result, "check": self.params["check"],
                    "has_clip": bool(self.params["source"])}


def review_params(body: dict) -> dict:
    """The form's fields, checked. Raises ValueError with a message for the page."""
    source = str(body.get("source") or "").strip()
    context = str(body.get("context") or "").strip()
    if not source and not context:
        raise ValueError("Paste a link or upload a clip, and describe what happened.")
    if len(context) > 4000:
        raise ValueError("Keep the description under 4,000 characters.")

    def when(key: str) -> float | None:
        text = str(body.get(key) or "").strip()
        if not text:
            return None
        try:
            return seconds(text)
        except ValueError:
            raise ValueError(f"Write the {key} time like 1:12 or 72.5.") from None

    start, end = when("start"), when("end")
    if end is not None and start is None:
        raise ValueError("Set when the incident starts, too.")
    if start is not None and end is not None and end <= start:
        raise ValueError("The end has to come after the start.")
    try:
        rounds, panel, frames = int(body.get("rounds") or MAX_ROUNDS), int(body.get("panel", len(PANEL))), \
            int(body.get("frames") or 12)
    except (TypeError, ValueError):
        raise ValueError("Rounds, panel members and frames have to be numbers.") from None
    return {"source": source, "context": context, "start": start, "end": end,
            "rounds": min(max(rounds, 1), 8), "panel": min(max(panel, 0), len(PANEL)),
            "frames": min(max(frames, 4), 20), "check": bool(body.get("check", True))}


def fetch(source: str, folder: Path, start: float | None, end: float | None, say) -> tuple[Path, float]:
    """Gets the clip. Returns (path, offset): offset is where the file starts in the original video, when only the
    part around the incident was downloaded."""
    if source.startswith("upload:"):
        path = UPLOADS / Path(source.removeprefix("upload:")).name
        if not path.exists():
            raise ValueError("That upload is gone. Upload the clip again.")
        return path, 0.0
    if not re.match(r"https?://", source, re.IGNORECASE):
        raise ValueError("Paste a link that starts with http:// or https://, or upload a file.")
    folder.mkdir(parents=True, exist_ok=True)
    try:
        resp = urllib.request.urlopen(urllib.request.Request(source, headers={"User-Agent": AGENT}), timeout=30)
    except (urllib.error.URLError, ValueError) as exc:
        raise ValueError(f"Couldn't open that link ({exc}).") from None
    with resp:
        kind = resp.headers.get_content_type()
        ext = Path(urllib.parse.urlparse(resp.geturl()).path).suffix.lower()
        if kind.startswith("video/") or kind == "application/ogg" or ext in VIDEO_EXT and "html" not in kind:
            dest = folder / f"clip{ext if ext in VIDEO_EXT else '.mp4'}"
            say(f"Downloading the clip from {urllib.parse.urlparse(source).netloc}")
            size = reported = 0
            with open(dest, "wb") as f:
                while chunk := resp.read(1 << 20):
                    size += len(chunk)
                    if size > MAX_VIDEO:
                        raise ValueError("That video is bigger than 500 MB. Trim it, or link to a shorter clip.")
                    f.write(chunk)
                    if size - reported >= 25 << 20:
                        say(f"  {size >> 20} MB so far")
                        reported = size
            say(f"  Downloaded {size / 2**20:.1f} MB")
            return dest, 0.0
    return fetch_page(source, folder, start, end, say)


def fetch_page(source: str, folder: Path, start: float | None, end: float | None, say) -> tuple[Path, float]:
    """A link to a page rather than a video file: download it with yt-dlp, if it's installed."""
    if not shutil.which("yt-dlp"):
        raise ValueError("That link is a web page, not a video file. Links to sites like YouTube need yt-dlp "
                         "(brew install yt-dlp); check that the site's terms and the footage's copyright allow "
                         "downloading it first. Or upload the clip, or paste a direct link to a video file.")
    offset, section = 0.0, []
    if start is not None:
        offset = max(0.0, start - 5)
        section = ["--download-sections", f"*{offset:.2f}-{(end if end is not None else start + 3) + 5:.2f}",
                   "--force-keyframes-at-cuts"]
    for old in folder.glob("clip.*"):
        old.unlink()
    say("Downloading the clip with yt-dlp" + (", just the part around the incident" if section else ""))
    done = subprocess.run(["yt-dlp", "--no-playlist", "--quiet", "--no-warnings", "-f",
                           "b[height<=720]/bv*[height<=720]+ba/b", "--merge-output-format", "mp4",
                           "-o", str(folder / "clip.%(ext)s"), *section, "--", source],
                          capture_output=True, text=True, timeout=900)
    clips = sorted(folder.glob("clip.*"))
    if done.returncode or not clips:
        problem = (done.stderr.strip().splitlines() or ["no file came back"])[-1]
        raise ValueError(f"yt-dlp couldn't download that link: {problem}")
    return clips[0], offset


def describe(case: Case, name: str) -> dict:
    """The case as the page shows it: the saved fields plus rule titles and frame URLs."""
    rules = {}
    for rulebook in case.rulebooks:
        rules.update(load_rulebook(rulebook))
    return {**asdict(case), "rule_titles": {r: rules[r].title for r in case.rules if r in rules},
            "frame_urls": [f"/frames/{name}/{Path(f).name}" for f in case.frames]}


def run(job: Job) -> None:
    p = job.params
    try:
        try:
            chat, look = connect(MODEL), connect(VISION_MODEL)
        except SystemExit as exc:  # Ollama isn't running, or a model hasn't been downloaded
            raise RuntimeError(str(exc)) from None
        folder = home() / "cases" / job.name
        video, offset = None, 0.0
        if p["source"]:
            job.status = "downloading"
            video, offset = fetch(p["source"], folder, p["start"], p["end"], job.say)
        at, window = None, 3.0
        if p["start"] is not None:
            end = p["end"] if p["end"] is not None else p["start"] + 3
            at, window = (p["start"] + end) / 2 - offset, max(0.5, (end - p["start"]) / 2)

        def step(stage: str) -> None:
            job.status = stage

        case, path, job.warnings = review.prepare(video, p["context"], job.name, chat, look, at=at, window=window,
                                                  count=p["frames"], log=job.say, step=step)
        job.raw_case, job.case = asdict(case), describe(case, job.name)
        if p["check"]:
            job.status = "checking"
            job.say("The case is ready for you to check.")
            job.go_on.wait()
            if job.edited:
                case = job.edited
                save_case(case, path, context=p["context"], warnings=job.warnings)
                job.case = describe(case, job.name)
        job.status = "discussing"
        result = debate(job.name, case, chat, p["rounds"], path.with_name("transcript.md"), SAVED_HEADER,
                        look if p["panel"] else None, p["panel"], out=job.say)
        seen = {f"frame_{t:07.2f}.jpg" for t in result.get("frames_seen", [])}
        job.result = {**result, "ruling": case.rulings[result["choice"]], "call": case.call,
                      "rulings": case.rulings, "frames_seen": [u for u in job.case["frame_urls"]
                                                               if u.rsplit("/", 1)[-1] in seen]}
        job.status = "done"
    except IntakeError as exc:
        job.error, job.status = f"Couldn't build the case: {exc}", "failed"
    except (ValueError, RuntimeError, OSError, subprocess.SubprocessError) as exc:
        job.error, job.status = str(exc), "failed"
    except Exception as exc:  # keep the worker alive, and show what happened
        job.say(traceback.format_exc())
        job.error, job.status = f"Something went wrong: {exc}", "failed"


def worker() -> None:
    while True:
        run(QUEUE.get())


class Handler(BaseHTTPRequestHandler):
    server_version = "SportsRulesCouncil"

    def log_message(self, *args) -> None:  # keep the terminal quiet
        pass

    def send(self, code: int, body: bytes, kind: str) -> None:
        self.send_response(code)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def send_json(self, code: int, data: dict) -> None:
        self.send(code, json.dumps(data).encode(), "application/json")

    def do_GET(self) -> None:
        url = urllib.parse.urlparse(self.path)
        path = "/static/index.html" if url.path == "/" else url.path
        if m := re.fullmatch(r"/static/([a-z]+\.(?:html|js|css))", path):
            f = WEB / m.group(1)
            if m.group(1) in STATIC and f.exists():
                return self.send(200, f.read_bytes(), STATIC[m.group(1)])
        elif path == "/api/config":
            return self.send_json(200, {"model": MODEL, "vision_model": VISION_MODEL, "panel": len(PANEL),
                                        "rounds": MAX_ROUNDS, "yt_dlp": bool(shutil.which("yt-dlp"))})
        elif m := re.fullmatch(r"/api/reviews/([0-9a-f]{12})", path):
            if job := JOBS.get(m.group(1)):
                since = (urllib.parse.parse_qs(url.query).get("since") or ["0"])[0]
                return self.send_json(200, job.view(int(since) if since.isdigit() else 0))
        elif m := re.fullmatch(r"/frames/([A-Za-z0-9_]+)/(frame_[0-9.]+\.jpg)", path):
            f = home() / "cases" / m.group(1) / "frames" / m.group(2)
            if f.exists():
                return self.send(200, f.read_bytes(), "image/jpeg")
        self.send_json(404, {"error": "Not found."})

    def do_POST(self) -> None:
        if self.headers.get("X-Requested-With") != "council":  # another site's page can't send this header
            return self.send_json(403, {"error": "Requests have to come from the council's own page."})
        url = urllib.parse.urlparse(self.path)
        length = int(self.headers.get("Content-Length") or 0)
        if url.path == "/api/upload":
            return self.upload(url, length)
        if length > MAX_JSON:
            return self.send_json(413, {"error": "That request is too big."})
        try:
            body = json.loads(self.rfile.read(length) or b"{}")
        except json.JSONDecodeError:
            return self.send_json(400, {"error": "That wasn't valid JSON."})
        if url.path == "/api/reviews":
            try:
                job = Job(review_params(body))
            except ValueError as exc:
                return self.send_json(400, {"error": str(exc)})
            JOBS[job.id] = job
            QUEUE.put(job)
            return self.send_json(200, {"id": job.id, "ahead": QUEUE.qsize() - 1})
        if m := re.fullmatch(r"/api/reviews/([0-9a-f]{12})/start", url.path):
            return self.start(m.group(1), body)
        self.send_json(404, {"error": "Not found."})

    def upload(self, url, length: int) -> None:
        name = (urllib.parse.parse_qs(url.query).get("name") or [""])[0]
        ext = Path(name).suffix.lower()
        if ext not in VIDEO_EXT:
            return self.send_json(400, {"error": f"Upload a video file ({', '.join(VIDEO_EXT)})."})
        if not 0 < length <= MAX_VIDEO:
            return self.send_json(413, {"error": "The file is empty or bigger than 500 MB."})
        UPLOADS.mkdir(exist_ok=True)
        dest = UPLOADS / f"{uuid.uuid4().hex[:12]}{ext}"
        with open(dest, "wb") as f:
            remaining = length
            while remaining and (chunk := self.rfile.read(min(1 << 20, remaining))):
                f.write(chunk)
                remaining -= len(chunk)
        self.send_json(200, {"source": f"upload:{dest.name}"})

    def start(self, job_id: str, body: dict) -> None:
        """The user has checked the case (and maybe edited it): carry on with the discussion."""
        job = JOBS.get(job_id)
        if not job:
            return self.send_json(404, {"error": "No such review."})
        if job.status != "checking":
            return self.send_json(409, {"error": "This review isn't waiting for a check."})
        if isinstance(body.get("case"), dict):
            try:
                job.edited = case_from_dict({**job.raw_case, **body["case"]})
                Council(job.name, job.edited, ask=None)  # raises ValueError if a rule ID isn't in the rulebook
            except ValueError as exc:
                return self.send_json(400, {"error": f"Can't use that case: {exc}."})
        job.go_on.set()
        self.send_json(200, {"ok": True})


_started = False


def start_server(port: int = 8000) -> ThreadingHTTPServer:
    """Starts the review worker and the web server in background threads, and returns the server."""
    global _started
    if not _started:
        threading.Thread(target=worker, daemon=True).start()
        _started = True
    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=8000)
    args = ap.parse_args(argv)
    server = start_server(args.port)
    print(f"Sports Rules Council is running: open http://localhost:{server.server_port}  (Ctrl-C to stop)")
    try:
        threading.Event().wait()
    except KeyboardInterrupt:
        print()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
