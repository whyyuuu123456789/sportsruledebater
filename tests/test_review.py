import importlib.util
import json
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import review
from cases import load_case, save_case
from council import Council
from evidence import frame_times, parse_observations, seconds, watch
from intake import Intake, IntakeError
from library import load_rulebook, rulebooks, search, title_of


def tiny_pdf(lines):
    """A one-page PDF with these lines of text (no parentheses)."""
    content = "BT /F1 10 Tf 40 760 Td 12 TL " + " ".join(f"({l}) Tj T*" for l in lines) + " ET"
    objects = ["<< /Type /Catalog /Pages 2 0 R >>", "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
               "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R "
               "/Resources << /Font << /F1 5 0 R >> >> >>",
               f"<< /Length {len(content)} >>\nstream\n{content}\nendstream",
               "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"]
    out, offsets = "%PDF-1.4\n", []
    for i, obj in enumerate(objects, 1):
        offsets.append(len(out))
        out += f"{i} 0 obj\n{obj}\nendobj\n"
    xref = len(out)
    out += f"xref\n0 {len(objects) + 1}\n0000000000 65535 f \n" + "".join(f"{o:010d} 00000 n \n" for o in offsets)
    return (out + f"trailer\n<< /Size {len(objects) + 1} /Root 1 0 R >>\nstartxref\n{xref}\n%%EOF\n").encode("latin-1")


FILLER = "the committee decides questions of fact on all the information reasonably available to it at the time"


class LibraryTests(unittest.TestCase):
    def test_markdown_rulebooks_are_listed_with_titles(self):
        books = rulebooks()
        self.assertLessEqual({"soccer", "basketball", "golf"}, set(books))
        self.assertTrue(title_of(books["soccer"]).startswith("Soccer"))

    def test_search_finds_the_clause_that_matches(self):
        hits = search(load_rulebook("soccer"), "did the defender deliberately play the ball or was it a deflection", 3)
        self.assertIn(hits[0].id, {"LOTG-11.DP", "LOTG-11.2"})
        f1 = [r.id for r in search(load_rulebook("formula1"), "they collided at turn 1; who was to blame", 3)]
        self.assertLessEqual({"FIA-L4.2d", "FIA-B1.9.3"}, set(f1))
        lapped = search(load_rulebook("formula1"), "lapped cars were allowed to overtake behind the safety car", 2)
        self.assertEqual({r.id for r in lapped}, {"FIA-B5.13.4", "FIA-B5.13.5"})

    def test_every_rulebook_id_works_as_a_citation(self):
        for name in rulebooks():
            for rid in load_rulebook(name):
                with self.subTest(rid):
                    prefix, _, rest = rid.partition("-")
                    self.assertNotRegex(prefix, r"^[FD]\d+$")  # would read as a fact ID like F1
                    self.assertRegex(rest, r"^[A-Za-z0-9.]*[A-Za-z0-9]$")  # what the council's citation pattern takes

    @unittest.skipUnless(importlib.util.find_spec("pypdf"), "needs pypdf")
    def test_pdf_rulebook_is_split_into_page_clauses_and_markdown_wins(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            lines = (["Rule 9.2b Deciding What Caused Ball to Move"] + [FILLER] * 6
                     + ["Rule 9.3 Ball Moved by Natural Forces", "if wind moves the ball there is no penalty"] + [FILLER])
            (tmp / "randa-rules.pdf").write_bytes(tiny_pdf(lines))
            rules = load_rulebook("randa-rules", rules_dir=tmp)
            self.assertEqual(list(rules), ["RANDA-p1.1", "RANDA-p1.2"])
            self.assertTrue(rules["RANDA-p1.2"].title.startswith("Rule 9.3"))
            self.assertEqual(rules["RANDA-p1.1"].source, "randa-rules.pdf, page 1")
            self.assertTrue((tmp / ".cache" / "randa-rules.json").exists())
            (tmp / "randa-rules.md").write_text("# Golf\n\n## GOLF-9.3 Ball moved by natural forces\nNo penalty.\n")
            self.assertEqual(rulebooks(tmp)["randa-rules"].suffix, ".md")


class EvidenceTests(unittest.TestCase):
    def test_frame_times(self):
        self.assertEqual(frame_times(60, 5, at=30, window=2), [28.0, 29.0, 30.0, 31.0, 32.0])
        self.assertEqual(frame_times(10, 3), [0.0, 4.88, 9.75])
        self.assertEqual(frame_times(10, 3, at=9.5, window=2), [7.5, 8.62, 9.75])
        self.assertEqual((seconds("1:02.5"), seconds("42")), (62.5, 42.0))

    def test_observations_are_parsed_per_frame(self):
        obs = parse_observations("t=1.0s: red #9 shoots\nT = 2.5 s - ball in the net", [1.0, 2.5])
        self.assertEqual([(o.t, o.text) for o in obs], [(1.0, "red #9 shoots"), (2.5, "ball in the net")])
        self.assertEqual(str(obs[1]), "[video 0:02.5] ball in the net")
        loose = parse_observations("Players in red and blue.", [4.0, 5.0])
        self.assertEqual([(o.t, o.text) for o in loose], [(4.0, "Players in red and blue.")])

    @unittest.skipUnless(shutil.which("ffmpeg"), "needs ffmpeg")
    def test_watch_sends_frames_to_the_vision_model(self):
        with tempfile.TemporaryDirectory() as tmp:
            clip = Path(tmp) / "clip.mp4"
            subprocess.run(["ffmpeg", "-v", "error", "-f", "lavfi", "-i", "testsrc=duration=3:size=320x240:rate=10",
                            "-pix_fmt", "yuv420p", str(clip)], check=True)
            seen = []

            def look(system, user, max_tokens, seed, images=(), json_mode=False):
                seen.extend(images)
                return "\n".join(f"t={t}s: test pattern" for t in re.findall(r"t=([\d.]+)s", user))

            obs = watch(clip, "", look, Path(tmp) / "frames", count=4, log=lambda _: None)
            self.assertEqual(len(list((Path(tmp) / "frames").glob("*.jpg"))), 4)
            self.assertEqual(len(seen), 4)
            self.assertTrue(all(img.startswith(b"\xff\xd8") for img in seen))  # JPEGs
            self.assertEqual([o.t for o in obs], [0.0, 0.92, 1.83, 2.75])


UNDERSTAND = {"sport": "soccer", "rulebooks": ["soccer"], "title": "Late goal after a defender's touch",
              "incident": "Eastport's defender touched a long ball, which reached Northgate's striker, who scored.",
              "officials": "Referee & VAR", "call": "Goal allowed", "call_reason": "The defender played the ball.",
              "parties": [{"name": "Northgate United", "stake": "Leads 2-1 if the goal stands."},
                          {"name": "Eastport Athletic", "stake": "Stays 1-1 if it is disallowed."}]}


def model(understand=UNDERSTAND, rulings=None):
    """A stand-in model for the case builder (JSON) and for the council's advocates."""
    def chat(system, user, max_tokens, seed, images=(), json_mode=False):
        if "RULEBOOKS AVAILABLE" in user:
            return json.dumps(understand)
        if "CANDIDATE CLAUSES" in user:
            return json.dumps({"rules": ["[LOTG-11.2]", "LOTG-11.DP", "LOTG-99.9", "LOTG-11.4"]})
        if "List the rulings" in user:
            return json.dumps(rulings or {"rulings": [{"id": "Goal Stands", "text": "The goal stands: 2-1"},
                                                      {"id": "no_goal", "text": "Offside: still 1-1"}],
                                          "call_id": "goal_stands"})
        if "Split what is known" in user:
            return json.dumps({"facts": [{"text": "The striker was in an offside position.", "source": "context"},
                                         {"text": "The defender stretched for the ball.", "source": "video 0:41.5"}],
                               "disputed": [{"text": "Whether the touch was a deliberate play."}]})
        if "Rank every ruling" in user:
            best = "goal_stands" if "Northgate" in system else "no_goal"
            return json.dumps({"ranking": [best]})
        if system.startswith("You are Panelist"):  # the neutral panel
            if "Before hearing the advocates" in user:
                return "NOTES: A defender stretches for the ball."
            vote = "no_goal" if system.startswith("You are Panelist C") else "goal_stands"
            return f"REASON: The touch looked deliberate to me.\nPOSITION: {vote}"
        mine = "goal_stands" if system.startswith(("You are the advocate for Northgate", "You are the advocate for "
                                                    "Referee")) else "no_goal"
        if "MESSAGE:" in user:  # a turn in the council's discussion
            return f"MESSAGE: LOTG-11.DP and F2.\nPOSITION: {mine}"
        return f"REASON: A judgment call.\nPOSITION: {mine}"  # the closing vote
    return chat


class IntakeTests(unittest.TestCase):
    def test_builds_a_case_the_council_can_debate(self):
        case = Intake(model(), "Northgate v Eastport, 88th minute.", [], log=lambda _: None).build("from description")
        self.assertEqual(case.rulebooks, ["soccer"])
        self.assertEqual(case.rules, ("LOTG-11.2", "LOTG-11.DP", "LOTG-11.4"))  # unknown IDs dropped
        self.assertEqual(case.call, "goal_stands")
        self.assertEqual(list(case.rulings), ["goal_stands", "no_goal"])
        self.assertEqual(case.facts["F1"], "The striker was in an offside position. [your context]")
        self.assertEqual(case.facts["F2"], "The defender stretched for the ball. [video 0:41.5]")
        self.assertEqual(case.disputed, {"D1": "Whether the touch was a deliberate play."})
        northgate, eastport = case.parties
        self.assertEqual(northgate.likes, {"goal_stands": 1.0, "no_goal": 0.0})
        self.assertEqual(eastport.likes, {"no_goal": 1.0, "goal_stands": 0.0})

        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "case.json"
            save_case(case, path, evidence=["[video 0:41.5] a defender stretches"])
            again = load_case(path)
        self.assertEqual(again, case)
        result = Council("t", again, model(), rounds=2, out=lambda _: None).run()
        self.assertEqual((result["status"], result["choice"], result["tally"]), ("VOTE", "goal_stands",
                                                                                  {"goal_stands": 2, "no_goal": 1}))

    def test_doubtful_answers_are_flagged_for_the_user(self):
        rulings = {"rulings": [{"id": "no_goal", "text": "Offside"}, {"id": "retake", "text": "Retake"}]}
        intake = Intake(model(rulings=rulings), "...", [], log=lambda _: None)
        case = intake.build("x")
        self.assertEqual((case.call, list(case.rulings)), ("no_goal", ["no_goal", "retake"]))  # no call_id: first
        self.assertTrue(any("'no_goal' is the call" in w for w in intake.warnings))
        self.assertTrue(any("Every party's first choice is no_goal" in w for w in intake.warnings))

    def test_it_asks_for_help_instead_of_guessing(self):
        one_party = {**UNDERSTAND, "parties": [{"name": "Northgate United"}]}
        with self.assertRaisesRegex(IntakeError, "two parties"):
            Intake(model(one_party), "...", [], log=lambda _: None).build("x")
        curling = {**UNDERSTAND, "sport": "curling", "rulebooks": []}
        with self.assertRaisesRegex(IntakeError, "No rulebook in rules/ covers curling"):
            Intake(model(curling), "...", [], log=lambda _: None).build("x")


class ReviewTests(unittest.TestCase):
    def test_context_only_review_saves_the_case_and_the_transcript(self):
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(review, "connect", lambda m: model()), \
                mock.patch.object(review, "HERE", Path(tmp)), mock.patch("builtins.print"):
            self.assertEqual(review.main(["--context", "Northgate v Eastport", "--yes", "--name", "t",
                                          "--rounds", "2"]), 0)
            folder = Path(tmp) / "cases" / "t"
            self.assertEqual(load_case(folder / "case.json").call, "goal_stands")
            transcript = (folder / "transcript.md").read_text()
            self.assertIn("Closing statements to the panel", transcript)
            self.assertIn("DECISION after 2 rounds, by the panel (2 of 3)", transcript)


if __name__ == "__main__":
    unittest.main()
