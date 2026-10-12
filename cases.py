"""Contested calls for the sports rules council, from easy to impossible. Add your own by copying one.

Every case has:
  call        the ruling the officials made; one advocate defends it
  rulings     every ruling the council may reach, including the call
  facts       established facts everyone accepts (F1, F2, ...); a veto must cite one
  disputed    points the sides disagree on (D1, D2, ...); arguable, but they can't ground a veto
  rulebook    which file in rules/ to read, and rules: the clauses (by ID) the advocates may cite
  parties     one advocate per party involved, as many as there are
  high_stakes the call decides a result (a game, a title, a medal): an unresolved rule objection
              to it goes to a human instead of letting the call stand

For each advocate:
  likes       how good each ruling is for its side, from 0 (worst) to 1 (best)
  concession  how much it lowers its standards each round; small means it fights longer
  floor       the lowest "likes" it will ever concede to; rulings below it are never approved
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

RULES_DIR = Path(__file__).resolve().parent / "rules"


@dataclass
class Advocate:
    name: str
    side: str                   # who it speaks for and what's at stake, in the advocate's voice
    likes: dict[str, float]
    concession: float = 0.15
    floor: float = 0.5


@dataclass
class Rule:
    id: str
    title: str
    text: str = ""
    source: str = ""


@dataclass
class Case:
    sport: str
    level: str                  # easy, in-between, long or impossible
    title: str
    incident: str
    officials: str              # who made the call; also the name of the advocate who defends it
    call: str
    call_reason: str
    rulings: dict[str, str]
    facts: dict[str, str]
    disputed: dict[str, str]
    rulebook: str
    rules: tuple[str, ...]
    parties: list[Advocate]
    expect: str                 # what a rules expert would expect: CONSENSUS, CALL_STANDS or ESCALATE
    expect_ruling: str = ""     # for CONSENSUS, the ruling expected
    high_stakes: bool = False
    escalate_to: str = "the league's referee review panel"
    officials_concession: float = 0.25  # how soon the officials' advocate will consider other rulings
    note: str = ""
    officials_side: str = ""   # overrides the officials' advocate's default brief

    def officials_advocate(self) -> Advocate:
        side = self.officials_side or (
            f"You represent {self.officials}, who made the call. Defend it with the rules and the facts. Officials "
            "care most about getting it right: if the rules and the established facts clearly show the call was "
            "wrong, concede.")
        likes = {r: 1.0 if r == self.call else 0.5 for r in self.rulings}
        return Advocate(self.officials, side, likes, concession=self.officials_concession, floor=0.5)

    def advocates(self) -> list[Advocate]:
        return [self.officials_advocate(), *self.parties]


def load_rulebook(name: str) -> dict[str, Rule]:
    """Reads rules/<name>.md: each clause is a '## ID Title' heading, an optional 'Source:' line and its text."""
    rules: dict[str, Rule] = {}
    current = None
    for line in (RULES_DIR / f"{name}.md").read_text().splitlines():
        if line.startswith("## "):
            rid, _, title = line[3:].strip().partition(" ")
            current = rules[rid] = Rule(rid, title.strip())
        elif current and line.startswith("Source:"):
            current.source = line.removeprefix("Source:").strip()
        elif current:
            current.text = (current.text + "\n" + line).strip()
    return rules


CASES: dict[str, Case] = {
    "point_two": Case(
        sport="Basketball (NBA rules)",
        level="easy",
        title="A catch-and-shoot buzzer-beater with 0.2 seconds left",
        incident="Metro Monarchs lead the Lakeside Lynx 100-99 with 0.2 seconds left. The Lynx inbound to guard "
                 "Dre Carter, who catches the pass, turns and hits a jump shot as the horn sounds.",
        officials="Game officials",
        call="no_basket",
        call_reason="On the floor they counted the basket. After an instant replay review they waved it off: with 0.2 "
                    "seconds on the clock a player can't catch and shoot, and the clock hit 0.0 before the release.",
        rulings={"no_basket": "No basket: the Monarchs win 100-99",
                 "basket_counts": "The basket counts: the Lynx win 101-100"},
        facts={
            "F1": "The Lynx inbounded from the sideline with 0.2 seconds on the game clock, trailing 100-99.",
            "F2": "Carter caught the pass with both hands at the top of the key, turned, and released a jump shot. "
                  "It went in.",
            "F3": "The replay shows the clock started when Carter first touched the ball, and it reached 0.0 before "
                  "the ball left his hand.",
            "F4": "The officials signaled a made basket on the floor, then waved it off after the replay review.",
        },
        disputed={
            "D1": "The Lynx say Carter's catch and release was one quick motion, closer to a tap than a catch.",
        },
        rulebook="basketball",
        rules=("NBA-TIME.1", "NBA-TIME.2", "NBA-REPLAY"),
        parties=[
            Advocate("Lakeside Lynx", "You represent the Lakeside Lynx. If Carter's shot counts, the Lynx win 101-100.",
                     {"basket_counts": 1.0, "no_basket": 0.3}, concession=0.35, floor=0.3),
            Advocate("Metro Monarchs", "You represent the Metro Monarchs. If the basket stays waved off, the Monarchs "
                     "win 100-99.", {"no_basket": 1.0, "basket_counts": 0.0}, concession=0.2, floor=0.5),
        ],
        expect="CONSENSUS",
        expect_ruling="no_basket",
        high_stakes=True,
        escalate_to="the league office",
    ),
    "dogso_card": Case(
        sport="Soccer (IFAB Laws of the Game)",
        level="in-between",
        title="Penalty and red card for a sliding trip on the penalty-area line",
        incident="55th minute, 0-0. Riverside FC striker Leo Varga runs onto a through ball and is clean through on "
                 "goal. Harbor City defender Sam Okafor, chasing from the side, slides in, misses the ball, and his "
                 "trailing leg trips Varga.",
        officials="Referee & VAR",
        call="pen_red",
        call_reason="The referee awarded a penalty kick and sent Okafor off for denying an obvious goal-scoring "
                    "opportunity. The VAR checked and did not recommend a review.",
        rulings={"pen_red": "Penalty kick to Riverside, and Okafor sent off (red card)",
                 "pen_yellow": "Penalty kick to Riverside, and Okafor cautioned (yellow card) instead of sent off",
                 "fk_red": "Direct free kick to Riverside just outside the penalty area, and Okafor sent off",
                 "no_foul": "No foul: play restarts with a dropped ball, and Okafor stays on with no card"},
        facts={
            "F1": "The VAR freeze-frame shows the trip (contact on Varga's standing foot) happened on the "
                  "penalty-area line.",
            "F2": "Okafor's slide was low and at normal speed, with his leading foot aimed at the ball. He missed the "
                  "ball by about 20 cm. He did not hold, pull or push Varga.",
            "F3": "When he was tripped, Varga was 11 m from goal, central, running toward goal with the ball about a "
                  "metre in front of him. Only the goalkeeper was between him and the goal; the nearest other "
                  "defender was 8 m behind.",
        },
        disputed={
            "D1": "Harbor City says the ball was running toward the goalkeeper, so Varga might not have kept control. "
                  "Riverside says he was about to shoot.",
        },
        rulebook="soccer",
        rules=("LOTG-1.2", "LOTG-12.1", "LOTG-12.3", "LOTG-VAR"),
        parties=[
            Advocate("Riverside FC", "You represent Riverside FC, whose striker was tripped. A penalty plus a red card "
                     "would leave Harbor City with ten men for 35 minutes.",
                     {"pen_red": 1.0, "pen_yellow": 0.7, "fk_red": 0.4, "no_foul": 0.0}, concession=0.15, floor=0.5),
            Advocate("Harbor City", "You represent Harbor City, whose defender Sam Okafor was sent off.",
                     {"no_foul": 1.0, "fk_red": 0.6, "pen_yellow": 0.6, "pen_red": 0.0}, concession=0.2, floor=0.5),
        ],
        expect="CONSENSUS",
        expect_ruling="pen_yellow",
        escalate_to="the competition's disciplinary panel",
    ),
    "deflection_offside": Case(
        sport="Soccer (IFAB Laws of the Game)",
        level="impossible",
        title="Late goal after a defender's touch: deliberate play or deflection?",
        incident="88th minute, 1-1. Northgate United's goalkeeper launches a long kick upfield. Eastport Athletic "
                 "centre-back Tom Reyes stretches for the dropping ball and touches it; it loops forward to Northgate "
                 "striker Kai Moreno, who was in an offside position when the ball was kicked. Moreno scores.",
        officials="Referee & VAR",
        call="goal",
        call_reason="The referee ruled that Reyes deliberately played the ball, so Moreno's offside position doesn't "
                    "count. The VAR found no clear and obvious error.",
        rulings={"goal": "Goal stands: Northgate lead 2-1",
                 "no_goal": "Goal disallowed for offside: indirect free kick to Eastport, still 1-1"},
        facts={
            "F1": "When the goalkeeper kicked the ball, Moreno's torso and left foot were about 40 cm nearer "
                  "Eastport's goal line than the second-last defender, in Eastport's half: an offside position.",
            "F2": "The ball travelled about 45 m in a high arc. Reyes watched it the whole way and had about two "
                  "seconds to adjust his position.",
            "F3": "Reyes stepped toward the ball and stretched his right leg at hip height. The ball came off his "
                  "shin and looped about 15 m forward to Moreno.",
            "F4": "Moreno controlled the ball and scored. No other Eastport player touched it.",
        },
        disputed={
            "D1": "Eastport says Reyes's touch was an instinctive stretch that achieved only limited contact (a "
                  "deflection), so Moreno's offside still counts. Northgate says Reyes had time and a clear view, and "
                  "tried to clear the ball (a deliberate play).",
            "D2": "How fast the ball was dropping when it reached Reyes.",
        },
        rulebook="soccer",
        rules=("LOTG-5.2", "LOTG-VAR", "LOTG-11.1", "LOTG-11.2", "LOTG-11.DP", "LOTG-11.4"),
        parties=[
            Advocate("Northgate United", "You represent Northgate United. Moreno's goal puts them ahead 2-1 in the "
                     "88th minute.", {"goal": 1.0, "no_goal": 0.0}),
            Advocate("Eastport Athletic", "You represent Eastport Athletic. If the goal is disallowed, the match stays "
                     "1-1.", {"no_goal": 1.0, "goal": 0.0}),
        ],
        expect="CALL_STANDS",
        note="A judgment call on a disputed point (D1): neither side will concede, so the on-field call stands unless "
             "the officials' own advocate gives it up.",
    ),
    "moved_ball": Case(
        sport="Golf (Rules of Golf)",
        level="impossible",
        title="A ball that moved at address decides a tournament: three players, one ruling",
        incident="Final round, 72nd hole. Leader Dana Hale grounds her putter behind her ball on the fringe, and a "
                 "second later the ball rolls a few centimetres. She says the wind moved it, plays it from where it "
                 "stopped, and holes the putt to win by one.",
        officials="Rules Committee",
        call="two_strokes",
        call_reason="After the round the Committee ruled from TV footage that Hale caused the ball to move. Because "
                    "she didn't replace it, she played from a wrong place, so it applied the two-stroke general "
                    "penalty.",
        rulings={"two_strokes": "Two-stroke penalty: Hale finishes 8 under in third; Ortiz and Lindqvist go to a "
                                "playoff",
                 "one_stroke": "One-stroke penalty: Hale finishes 9 under, in a three-way playoff with Ortiz and "
                               "Lindqvist",
                 "no_penalty": "No penalty (natural forces moved the ball): Hale wins at 10 under"},
        facts={
            "F1": "Hale's ball lay on the fringe, about 15 cm off the putting green. No part of it touched the "
                  "putting green.",
            "F2": "Hale grounded her putter lightly about 2 cm behind the ball. The putter never touched the ball.",
            "F3": "About one second after she grounded the putter, the ball rolled about 5 cm, down the fringe's "
                  "slope and away from the hole.",
            "F4": "The weather station on that hole logged gusts of 40 km/h (25 mph) at that minute.",
            "F5": "Hale told the walking official she believed the wind had moved the ball. She played it from its "
                  "new spot and holed the putt to finish 10 under par. Ortiz and Lindqvist had already finished at 9 "
                  "under.",
            "F6": "At normal speed, the TV footage shows the putter being grounded and the ball moving a second "
                  "later. Only a super-slow-motion replay shows the grass behind the ball flattening as the putter "
                  "was grounded.",
        },
        disputed={
            "D1": "Whether it is at least 95% likely that grounding the putter, rather than the wind and the slope, "
                  "made the ball move.",
            "D2": "Whether the Committee could rely on the super-slow-motion replay, or only on what could be seen "
                  "with the naked eye.",
        },
        rulebook="golf",
        rules=("GOLF-KVC", "GOLF-PG", "GOLF-9.2b", "GOLF-9.3", "GOLF-9.4b", "GOLF-9.4", "GOLF-13.1d",
               "GOLF-14.7a", "GOLF-20.2c"),
        parties=[
            Advocate("Dana Hale", "You represent Dana Hale. With no penalty she wins the tournament outright.",
                     {"no_penalty": 1.0, "one_stroke": 0.5, "two_strokes": 0.0}, concession=0.15, floor=0.4),
            Advocate("Lucia Ortiz", "You represent Lucia Ortiz, who finished 9 under. With the two-stroke penalty "
                     "she's in a two-player playoff for the title; with one stroke it's a three-way playoff; with no "
                     "penalty she finishes tied second.",
                     {"two_strokes": 1.0, "one_stroke": 0.5, "no_penalty": 0.0}, concession=0.15, floor=0.4),
            Advocate("Ebba Lindqvist", "You represent Ebba Lindqvist, who finished 9 under. With the two-stroke "
                     "penalty she's in a two-player playoff for the title; with one stroke it's a three-way playoff; "
                     "with no penalty she finishes tied second.",
                     {"two_strokes": 1.0, "one_stroke": 0.5, "no_penalty": 0.0}, concession=0.1, floor=0.4),
        ],
        expect="ESCALATE",
        high_stakes=True,
        escalate_to="the R&A and USGA rules officials, for a formal interpretation",
        note="Four advocates. 'one_stroke' looks like a fair compromise, but it isn't a ruling the Rules allow on "
             "these facts: she either caused the ball to move and played from a wrong place, or she didn't.",
    ),
}
