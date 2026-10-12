import json
import shutil
import subprocess
import tempfile
import time
import unittest
import urllib.error
import urllib.request
from pathlib import Path
from unittest import mock

import review
import web
from tests.test_review import model


def call(base, path, body=None, headers=None, raw=None):
    """(status, parsed JSON or raw bytes) for a request to the test server."""
    data = raw if raw is not None else (json.dumps(body).encode() if body is not None else None)
    req = urllib.request.Request(base + path, data=data, headers=headers or {})
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            payload, kind, status = resp.read(), resp.headers.get_content_type(), resp.status
    except urllib.error.HTTPError as err:
        payload, kind, status = err.read(), err.headers.get_content_type(), err.code
    return status, (json.loads(payload) if kind == "application/json" else payload)


SEND = {"X-Requested-With": "council", "Content-Type": "application/json"}


class FormTests(unittest.TestCase):
    def test_the_form_is_checked(self):
        p = web.review_params({"source": "https://example.com/clip.mp4", "context": "x", "start": "1:12",
                               "end": "1:20", "rounds": "3", "panel": "9"})
        self.assertEqual((p["start"], p["end"], p["rounds"], p["panel"], p["check"]), (72.0, 80.0, 3, 3, True))
        for body, message in [({}, "Paste a link"), ({"context": "x", "start": "1:20", "end": "1:12"}, "after the start"),
                              ({"context": "x", "start": "soon"}, "like 1:12"), ({"context": "x", "end": "0:05"},
                                                                                   "starts, too")]:
            with self.subTest(message), self.assertRaisesRegex(ValueError, message):
                web.review_params(body)

    def test_page_links_need_yt_dlp(self):
        with mock.patch.object(web.shutil, "which", lambda name: None), \
                self.assertRaisesRegex(ValueError, "brew install yt-dlp"):
            web.fetch_page("https://www.youtube.com/watch?v=abcdefghijk", Path("."), None, None, print)
        with self.assertRaisesRegex(ValueError, "http:// or https://"):
            web.fetch("ftp://example.com/clip.mp4", Path("."), None, None, print)


@unittest.skipUnless(shutil.which("ffmpeg"), "needs ffmpeg")
class SiteTests(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = Path(tmp.name)
        for target, name, value in [(review, "HERE", self.tmp), (web, "UPLOADS", self.tmp / "uploads"),
                                    (web, "connect", lambda model_name: model())]:
            patcher = mock.patch.object(target, name, value)
            patcher.start()
            self.addCleanup(patcher.stop)
        self.server = web.start_server(0)
        self.addCleanup(self.server.shutdown)
        self.base = f"http://127.0.0.1:{self.server.server_port}"

    def wait_for(self, job_id, status):
        for _ in range(200):
            code, view = call(self.base, f"/api/reviews/{job_id}")
            if view["status"] in (status, "failed"):
                return view
            time.sleep(0.05)
        self.fail(f"the review never reached {status}")

    def test_a_review_from_upload_to_ruling(self):
        code, page = call(self.base, "/")
        self.assertEqual(code, 200)
        self.assertIn(b"<title>Sports Rules Council</title>", page)
        self.assertEqual(call(self.base, "/api/config")[1]["panel"], 3)
        self.assertEqual(call(self.base, "/api/reviews", {"context": "x"},
                              {"Content-Type": "application/json"})[0], 403)  # no X-Requested-With header

        clip = self.tmp / "clip.mp4"
        subprocess.run(["ffmpeg", "-v", "error", "-f", "lavfi", "-i", "testsrc=duration=3:size=320x240:rate=10",
                        "-pix_fmt", "yuv420p", str(clip)], check=True)
        code, up = call(self.base, "/api/upload?name=clip.mp4", raw=clip.read_bytes(),
                        headers={"X-Requested-With": "council"})
        self.assertEqual(code, 200)
        code, made = call(self.base, "/api/reviews", {"source": up["source"], "context": "Northgate v Eastport",
                                                      "start": "0:01", "end": "0:02", "rounds": 2}, SEND)
        self.assertEqual(code, 200)

        view = self.wait_for(made["id"], "checking")
        self.assertEqual(view["status"], "checking", view["error"])
        case = view["case"]
        self.assertTrue(case["frame_urls"])
        self.assertEqual(case["rule_titles"]["LOTG-11.2"], "Offside offence")
        code, jpeg = call(self.base, case["frame_urls"][0])
        self.assertEqual((code, jpeg[:2]), (200, b"\xff\xd8"))

        code, answer = call(self.base, f"/api/reviews/{made['id']}/start", {"case": {"call": "nonsense"}}, SEND)
        self.assertEqual(code, 400)
        self.assertIn("isn't one of the rulings", answer["error"])
        facts = {**case["facts"], "F1": "Edited: the striker was offside when the ball was kicked. [your context]"}
        self.assertEqual(call(self.base, f"/api/reviews/{made['id']}/start", {"case": {"facts": facts}}, SEND)[0], 200)

        view = self.wait_for(made["id"], "done")
        self.assertEqual(view["status"], "done", view["error"])
        result = view["result"]
        self.assertEqual((result["decided_by"], result["choice"]), ("panel", "goal_stands"))  # A and B vote for it
        self.assertEqual(result["ruling"], "The goal stands: 2-1")
        self.assertTrue(set(result["frames_seen"]) <= set(case["frame_urls"]))
        saved = json.loads((self.tmp / "cases" / view["name"] / "case.json").read_text())
        self.assertTrue(saved["facts"]["F1"].startswith("Edited:"))
        self.assertTrue((self.tmp / "cases" / view["name"] / "transcript.md").exists())


if __name__ == "__main__":
    unittest.main()
