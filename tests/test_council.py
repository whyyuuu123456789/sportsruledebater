import re
import unittest
from dataclasses import replace

from cases import CASES, load_rulebook
from council import Council, parse_votes


def scripted(replies):
    """A stand-in model. replies[advocate] = (argument reply, vote reply); either may be a list, one per round."""
    def ask(system, user, max_tokens, seed):
        name = re.search(r"advocate for (.+?) on a sports rules council", system).group(1)
        reply = replies[name][0 if "ARGUMENT:" in user else 1]
        if isinstance(reply, list):
            rnd = int(re.search(r"Round (\d+)", user).group(1)) - 1
            reply = reply[min(rnd, len(reply) - 1)]
        return reply
    return ask


def council(name, replies, rounds=3, **changes):
    return Council(name, replace(CASES[name], **changes), scripted(replies), rounds, out=lambda _: None)


POINT_TWO = {
    "Game officials": ("ARGUMENT: NBA-TIME.1 and F2: he caught and shot.\nRULING: no_basket",
                       "basket_counts: VETO NBA-TIME.2 F3 - The clock hit 0.0 before the release."),
    "Metro Monarchs": ("ARGUMENT: F3 settles it.\nRULING: no_basket",
                       "basket_counts: REJECT - The shot came too late."),
    "Lakeside Lynx": ("ARGUMENT: D1, it was one motion.\nRULING: basket_counts",
                      "no_basket: APPROVE - NBA-TIME.1 is clear on F2."),
}


class CaseTests(unittest.TestCase):
    def test_every_case_is_well_formed(self):
        for name, c in CASES.items():
            with self.subTest(name):
                self.assertIn(c.call, c.rulings)
                self.assertIn(c.expect, ("CONSENSUS", "CALL_STANDS", "ESCALATE"))
                if c.expect_ruling:
                    self.assertIn(c.expect_ruling, c.rulings)
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


class ProtocolTests(unittest.TestCase):
    def test_consensus_waits_until_the_losing_side_may_concede(self):
        c = council("point_two", POINT_TWO, rounds=6)
        r = c.run()
        self.assertEqual((r["status"], r["choice"], r["rounds"]), ("CONSENSUS", "no_basket", 3))
        first = c.history[0]["votes"]["Lakeside Lynx"]["no_basket"]
        self.assertEqual(first.kind, "REJECT")
        self.assertIn("approval blocked", first.note)

    def test_veto_needs_a_listed_rule_and_an_established_fact(self):
        c = council("dogso_card", {})
        a = c.advocates[2]  # Harbor City
        valid = c.check(a, "pen_red", 0, "VETO", "LOTG-12.3 F2 - He made a genuine attempt to play the ball.")
        self.assertEqual((valid.kind, valid.rule, valid.fact), ("VETO", "LOTG-12.3", "F2"))
        self.assertEqual(valid.reason, "He made a genuine attempt to play the ball.")
        disputed = c.check(a, "pen_red", 0, "VETO", "LOTG-12.3 D1 - Varga would have lost the ball.")
        self.assertEqual(disputed.kind, "REJECT")
        self.assertIn("D# points are disputed", disputed.note)
        invented = c.check(a, "pen_red", 0, "VETO", "LOTG-99.9 F2 - Made-up rule.")
        self.assertEqual(invented.kind, "REJECT")
        self.assertIn("veto struck", invented.note)

    def test_invented_citations_are_struck_from_arguments(self):
        replies = {n: ("ARGUMENT: Under LOTG-12.3 and LOTG-99.1, F2 and F9 show it.\nRULING: no_foul", "")
                   for n in ("Referee & VAR", "Riverside FC", "Harbor City")}
        c = council("dogso_card", replies)
        arg = c.argue(c.advocates[2], 0)
        self.assertEqual(arg.struck, ["LOTG-99.1", "F9"])
        self.assertEqual(arg.ruling, "no_foul")
        officials = c.argue(c.advocates[0], 0)  # the officials can't abandon their call in round 1
        self.assertEqual(officials.ruling, "pen_red")
        self.assertIn("can't concede no_foul yet", officials.note)

    def test_call_stands_when_officials_hold_on_a_judgment_call(self):
        replies = {
            "Referee & VAR": ("ARGUMENT: LOTG-11.DP and F2.\nRULING: goal", "no_goal: REJECT - D1 is a judgment call."),
            "Northgate United": ("ARGUMENT: F2.\nRULING: goal", "no_goal: REJECT - Deliberate play."),
            "Eastport Athletic": ("ARGUMENT: D1.\nRULING: no_goal", "goal: VETO LOTG-11.2 F1 - Moreno was offside."),
        }
        r = council("deflection_offside", replies).run()
        self.assertEqual((r["status"], r["choice"]), ("CALL_STANDS", "goal"))
        self.assertTrue(any(d.startswith("Eastport Athletic: VETO (LOTG-11.2 + F1)") for d in r["dissent"]))

    def test_grounded_objection_to_a_result_deciding_call_escalates(self):
        replies = {
            "Referee & VAR": ("ARGUMENT: LOTG-11.DP.\nRULING: goal", "no_goal: REJECT - Judgment call."),
            "Northgate United": ("ARGUMENT: F2.\nRULING: goal", "no_goal: REJECT - Deliberate play."),
            "Eastport Athletic": ("ARGUMENT: D1.\nRULING: no_goal", "goal: VETO LOTG-11.2 F1 - Moreno was offside."),
        }
        r = council("deflection_offside", replies, high_stakes=True).run()
        self.assertEqual(r["status"], "ESCALATE")
        self.assertIn("Eastport Athletic", r["reason"])

    def test_officials_giving_up_the_call_with_no_replacement_escalates(self):
        no_pen = "two_strokes: REJECT - Not virtually certain.\none_stroke: VETO GOLF-9.4 F5 - Not a valid ruling."
        replies = {
            "Rules Committee": (["ARGUMENT: F6.\nRULING: two_strokes", "ARGUMENT: F6.\nRULING: two_strokes",
                                 "ARGUMENT: GOLF-20.2c and F6: only slow motion shows it.\nRULING: no_penalty"],
                                ["no_penalty: REJECT - F6.\none_stroke: REJECT - No.", no_pen]),
            "Dana Hale": ("ARGUMENT: GOLF-9.2b, F4.\nRULING: no_penalty", no_pen),
            "Lucia Ortiz": ("ARGUMENT: F2, F3.\nRULING: two_strokes",
                            "no_penalty: APPROVE - Fine.\none_stroke: REJECT - No."),
            "Ebba Lindqvist": ("ARGUMENT: F2, F3.\nRULING: two_strokes",
                               "no_penalty: REJECT - F3.\none_stroke: REJECT - No."),
        }
        c = council("moved_ball", replies)
        r = c.run()
        self.assertEqual(r["status"], "ESCALATE")
        self.assertIn("no longer backs the call", r["reason"])
        ortiz = c.history[-1]["votes"]["Lucia Ortiz"]["no_penalty"]
        self.assertIn("will never concede no_penalty", ortiz.note)

    def test_a_compromise_nobody_argues_for_cannot_be_adopted(self):
        approve_all = "two_strokes: APPROVE - Fine.\none_stroke: APPROVE - A fair middle ground.\nno_penalty: APPROVE - Fine."
        replies = {name: (f"ARGUMENT: F3.\nRULING: {ruling}", approve_all) for name, ruling in (
            ("Rules Committee", "two_strokes"), ("Dana Hale", "no_penalty"),
            ("Lucia Ortiz", "two_strokes"), ("Ebba Lindqvist", "two_strokes"))}
        c = council("moved_ball", replies, rounds=6)
        r = c.run()
        self.assertEqual(r["status"], "CALL_STANDS")
        self.assertTrue(all("one_stroke" not in rnd["votes"]["Dana Hale"] for rnd in c.history))

    def test_the_merits_vote_is_not_told_the_stakes(self):
        prompts = []
        base = scripted(POINT_TWO)

        def ask(system, user, max_tokens, seed):
            if "ARGUMENT:" not in user:
                prompts.append(user)
            return base(system, user, max_tokens, seed)

        Council("point_two", CASES["point_two"], ask, 6, out=lambda _: None).run()
        self.assertTrue(prompts)
        for p in prompts:
            self.assertNotRegex(p, r"(?i)side (could|can|can't|won't) accept")

    def test_vote_lines_tolerate_formatting_and_missing_votes(self):
        reply = "**pen_yellow**: approve - Attempt to play the ball.\n1. fk_red (free kick): VETO LOTG-1.2 F1 - On the line."
        found = parse_votes(reply, ["pen_yellow", "fk_red", "no_foul"])
        self.assertEqual(found["pen_yellow"], ("APPROVE", "Attempt to play the ball."))
        self.assertEqual(found["fk_red"][0], "VETO")
        self.assertNotIn("no_foul", found)
        c = council("dogso_card", {})
        self.assertIn("no clear vote", c.check(c.advocates[1], "no_foul", 0, "", "").note)


if __name__ == "__main__":
    unittest.main()
