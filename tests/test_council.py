import re
import tempfile
import unittest
from dataclasses import replace
from pathlib import Path

from cases import CASES, Advocate, load_rulebook
from council import Council, parse


def scripted(replies, prompts=None):
    """A stand-in model. replies[advocate] = (its turns, its closing vote); turns may be a list, one per round."""
    def ask(system, user, max_tokens, seed):
        if prompts is not None:
            prompts.append((system, user))
        name = re.search(r"advocate for (.+?) on a sports rules council", system).group(1)
        reply = replies[name][1 if "Cast your final vote" in user else 0]
        if isinstance(reply, list):
            rnd = int(re.search(r"Round (\d+) of", user).group(1)) - 1
            reply = reply[min(rnd, len(reply) - 1)]
        return reply
    return ask


def council(name, replies, rounds=4, prompts=None, **changes):
    return Council(name, replace(CASES[name], **changes), scripted(replies, prompts), rounds, out=lambda _: None)


def say(position, message="I stand by that."):
    return f"MESSAGE: {message}\nPOSITION: {position}"


def vote(position, reason):
    return f"REASON: {reason}\nPOSITION: {position}"


class CaseTests(unittest.TestCase):
    def test_every_case_is_well_formed(self):
        for name, c in CASES.items():
            with self.subTest(name):
                self.assertIn(c.call, c.rulings)
                self.assertTrue(all(re.fullmatch(r"F\d+", f) for f in c.facts))
                self.assertTrue(all(re.fullmatch(r"D\d+", d) for d in c.disputed))
                self.assertLessEqual(set(c.rules), set(load_rulebook(c.rulebook)))
                for a in c.advocates():
                    self.assertEqual(set(a.likes), set(c.rulings), a.name)
                self.assertGreaterEqual(len(c.parties), 2)

    def test_rulebook_reads_ids_titles_sources_and_text(self):
        rule = load_rulebook("soccer")["LOTG-11.2"]
        self.assertEqual(rule.title, "Offside offence")
        self.assertEqual(rule.source, "Law 11 (Offside), section 2")
        self.assertIn("deliberate save", rule.text)


class DiscussionTests(unittest.TestCase):
    def test_speakers_hear_everything_said_so_far_including_this_round(self):
        prompts = []
        replies = {"Game officials": (say("no_basket", "NBA-TIME.1 decides it."), vote("no_basket", "Too late.")),
                   "Lakeside Lynx": (say("basket_counts", "It was one motion."), vote("basket_counts", "One motion.")),
                   "Metro Monarchs": (say("no_basket", "The clock hit zero."), vote("no_basket", "Too late."))}
        council("point_two", replies, rounds=2, prompts=prompts).run()
        turns = [(s.split(" on a sports")[0].removeprefix("You are the advocate for "), u)
                 for s, u in prompts if "MESSAGE:" in u]
        first = dict(turns[:3])  # round 1, in speaking order
        self.assertIn("Nobody has spoken yet.", first["Game officials"])
        self.assertIn("Open by explaining the call (no_basket)", first["Game officials"])
        self.assertIn("Open by making the case for basket_counts", first["Lakeside Lynx"])
        self.assertIn("- Game officials (backs no_basket): NBA-TIME.1 decides it.", first["Lakeside Lynx"])
        self.assertIn("- Lakeside Lynx (backs basket_counts): It was one motion.", first["Metro Monarchs"])
        officials_again = next(u for name, u in turns[3:] if name == "Game officials")
        self.assertIn("- You (backing no_basket): NBA-TIME.1 decides it.", officials_again)

    def test_being_persuaded_ends_the_discussion_in_consensus(self):
        replies = {"Game officials": (say("no_basket"), vote("no_basket", "")),
                   "Lakeside Lynx": ([say("basket_counts"), say("no_basket", "Fair point, Monarchs: F3 settles it.")],
                                     vote("no_basket", "")),
                   "Metro Monarchs": (say("no_basket"), vote("no_basket", ""))}
        c = council("point_two", replies)
        r = c.run()
        self.assertEqual((r["status"], r["choice"], r["rounds"], r["upheld"]), ("CONSENSUS", "no_basket", 2, True))
        lynx = [t for t in c.turns if t.speaker == "Lakeside Lynx"]
        self.assertEqual((lynx[-1].before, lynx[-1].position), ("basket_counts", "no_basket"))

    def test_without_consensus_a_closing_vote_decides(self):
        replies = {"Referee & VAR": (say("pen_red"), vote("pen_yellow", "He tried to play the ball, so it's a caution.")),
                   "Riverside FC": (say("pen_red"), vote("pen_red", "He denied a clear chance.")),
                   "Harbor City": (say("no_foul"), vote("pen_yellow", "A caution at most."))}
        r = council("dogso_card", replies).run()
        self.assertEqual((r["status"], r["choice"], r["upheld"]), ("VOTE", "pen_yellow", False))
        self.assertEqual(r["tally"], {"pen_yellow": 2, "pen_red": 1})
        self.assertEqual(r["dissent"], ["Riverside FC (pen_red): He denied a clear chance."])
        self.assertEqual(r["rounds"], 3)  # nobody moved in round 3, so the discussion ended before round 4

    def test_a_tied_vote_goes_the_officials_way(self):
        replies = {"Rules Committee": (say("two_strokes"), vote("two_strokes", "She caused it.")),
                   "Dana Hale": (say("no_penalty"), vote("no_penalty", "The wind moved it.")),
                   "Lucia Ortiz": (say("two_strokes"), vote("two_strokes", "The replay is clear.")),
                   "Ebba Lindqvist": (say("two_strokes"), vote("no_penalty", "It isn't 95% likely."))}
        r = council("moved_ball", replies).run()
        self.assertEqual((r["status"], r["choice"]), ("TIE", "two_strokes"))

    def test_a_tie_the_officials_didnt_vote_for_leaves_the_call_standing(self):
        ruiz = Advocate("Ana Ruiz", "You represent Ana Ruiz.", {"two_strokes": 1.0, "one_stroke": 0.5, "no_penalty": 0.0})
        replies = {"Rules Committee": (say("two_strokes"), vote("one_stroke", "A middle way.")),
                   "Dana Hale": (say("no_penalty"), vote("no_penalty", "The wind.")),
                   "Lucia Ortiz": (say("two_strokes"), vote("two_strokes", "The replay.")),
                   "Ebba Lindqvist": (say("two_strokes"), vote("no_penalty", "Doubt.")),
                   "Ana Ruiz": (say("two_strokes"), vote("two_strokes", "The replay."))}
        c = council("moved_ball", replies, parties=[*CASES["moved_ball"].parties, ruiz])
        r = c.run()
        self.assertEqual((r["status"], r["choice"]), ("TIE", "two_strokes"))
        self.assertTrue(any("so the call stands" in line for line in c.lines))

    def test_citations_outside_the_case_are_struck(self):
        replies = {n: (say("no_foul", "Under LOTG-12.3 and LOTG-99.1, F2 and F9 show it."), vote("no_foul", ""))
                   for n in ("Referee & VAR", "Riverside FC", "Harbor City")}
        c = council("dogso_card", replies, rounds=1)
        self.assertEqual(c.speak(c.advocates[2], 0).struck, ["LOTG-99.1", "F9"])

    def test_a_turn_without_a_position_keeps_the_last_one(self):
        replies = {n: ("MESSAGE: I'm not sure yet.", vote("goal", ""))
                   for n in ("Referee & VAR", "Northgate United", "Eastport Athletic")}
        c = council("deflection_offside", replies, rounds=1)
        turn = c.speak(c.advocates[2], 0)  # Eastport starts out backing no_goal
        self.assertEqual((turn.position, turn.note), ("no_goal", "no valid POSITION line, so its position is unchanged"))

    def test_replies_are_parsed_in_any_reasonable_shape(self):
        self.assertEqual(parse("MESSAGE: My position is firm.\n**POSITION:** pen_yellow"),
                         ("My position is firm.", "pen_yellow"))
        self.assertEqual(parse("Fair enough.\nPosition - NO_FOUL"), ("Fair enough.", "no_foul"))
        self.assertEqual(parse("No position line here."), ("No position line here.", ""))


def panel_model(votes, calls):
    """A stand-in vision model for the panel. votes[panelist] = the ruling it votes for."""
    def look(system, user, max_tokens, seed, images=(), json_mode=False):
        name = re.match(r"You are (Panelist \w)", system).group(1)
        calls.append((name, user, images))
        if "Before hearing the advocates" in user:
            return f"NOTES: {name} sees the defender stretch."
        return f"REASON: {name} went by the clip.\nPOSITION: {votes[name]}"
    return look


class PanelTests(unittest.TestCase):
    advocates = {"Referee & VAR": (say("goal", "Reyes chose to play it."), ""),
                 "Northgate United": (say("goal", "He had two seconds."), ""),
                 "Eastport Athletic": (say("no_goal", "It was a reflex."), "")}

    def review(self, votes, panel=3, prompts=None):
        frames = tempfile.TemporaryDirectory()
        self.addCleanup(frames.cleanup)
        paths = [Path(frames.name) / f"frame_{t:07.2f}.jpg" for t in (41.5, 42.0)]
        for p in paths:
            p.write_bytes(b"\xff\xd8 a still")
        calls = []
        case = replace(CASES["deflection_offside"], frames=[str(p) for p in paths])
        c = Council("t", case, scripted(self.advocates, prompts), rounds=2, out=lambda _: None,
                    look=panel_model(votes, calls), panel=panel)
        return c, c.run(), calls

    def test_the_panel_watches_the_clip_then_hears_the_discussion_and_decides(self):
        prompts = []
        c, r, calls = self.review({"Panelist A": "no_goal", "Panelist B": "goal", "Panelist C": "no_goal"},
                                  prompts=prompts)
        looks = [(n, u, i) for n, u, i in calls if "Before hearing the advocates" in u]
        self.assertEqual([n for n, _, _ in looks], ["Panelist A", "Panelist B", "Panelist C"])
        self.assertTrue(all(len(images) == 2 for _, _, images in calls))  # every panel call sees the frames
        self.assertIn("0:41.5, 0:42.0", looks[0][1])
        vote_a = next(u for n, u, _ in calls if n == "Panelist A" and "Now vote" in u)
        self.assertIn("Panelist A sees the defender stretch.", vote_a)      # its own notes from the clip...
        self.assertNotIn("Panelist B sees", vote_a)                         # ...and nobody else's
        self.assertIn("- Eastport Athletic (backs no_goal): It was a reflex.", vote_a)
        self.assertIn("THEIR CLOSING STATEMENTS:", vote_a)
        self.assertIn("a neutral panel, who also watch the clip, decides it", prompts[0][0])
        self.assertEqual((r["status"], r["choice"], r["decided_by"], r["upheld"]), ("VOTE", "no_goal", "panel", False))
        self.assertEqual(r["positions"], {"Referee & VAR": "goal", "Northgate United": "goal",
                                          "Eastport Athletic": "no_goal"})
        self.assertEqual(r["dissent"], ["Panelist B (goal): Panelist B went by the clip."])

    def test_a_tied_panel_goes_with_the_advocates_final_positions(self):
        c, r, _ = self.review({"Panelist A": "no_goal", "Panelist B": "goal"}, panel=2)
        self.assertEqual((r["status"], r["choice"]), ("TIE", "goal"))  # two advocates ended up backing goal
        self.assertTrue(any("the advocates' final positions break the tie" in line for line in c.lines))


if __name__ == "__main__":
    unittest.main()
