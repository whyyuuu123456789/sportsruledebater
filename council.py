"""Sports rules council: advocates for the officials and for every party discuss a contested call, and a neutral
panel decides it.

One advocate defends the call the officials made, and every party involved, however many there are, gets its own
advocate. They hold an actual discussion: they speak in turn, each one seeing everything said so far, answer each
other by name, concede points, and change their minds when they're persuaded. After every turn the speaker says
which ruling it backs now. When the discussion ends, each advocate gives the panel a closing statement.

The neutral panel decides. Its members have no side, and they're played by a different model from the advocates,
one that can see images. When the case comes from a video, each member watches the clip before the discussion and
writes down what it sees, so its view of the evidence comes from the clip and not from either side. After the
discussion, each member looks at the frames again and votes on its own:

  CONSENSUS   the panel is unanimous
  VOTE        the panel's majority wins; the dissent is recorded
  TIE         the panel is split evenly: the advocates' final positions break the tie, and failing that the call
              stands

There's no answer key: the panel decides, and the transcript shows why. With --panel 0 the advocates decide by
themselves instead: a consensus during the discussion, or else a closing vote among them.

    python3 council.py                  # every demo case (needs Ollama running, or LLM_MODEL set)
    python3 council.py dogso_card       # one case
    python3 council.py all --rounds 3   # a shorter discussion
    python3 council.py all --panel 0    # no panel: the advocates decide among themselves
    python3 council.py --case-file cases/<name>/case.json   # a saved case, such as one review.py built from a video
"""

from __future__ import annotations

import argparse
import re
import sys
import time
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

from cases import CASES, Advocate, Case, load_case
from evidence import stamp
from library import load_rulebook
from llm import MODEL, VISION_MODEL, Chat, connect

MAX_ROUNDS = 4
HISTORY = 16       # how many of the latest turns a speaker is shown
PANEL_FRAMES = 6   # how many stills from the clip each panel member looks at
PANEL = [("Panelist A", "You pay most attention to what the clip and the established facts actually show."),
         ("Panelist B", "You pay most attention to the exact wording of the rules."),
         ("Panelist C", "You pay most attention to fairness and the purpose behind the rules.")]
HERE = Path(__file__).resolve().parent
FACT_ID = re.compile(r"\b[FD]\d+\b")
POSITION = re.compile(r"^\W*POSITION\W*?[:\-]\s*\W*([A-Za-z0-9_]+)", re.IGNORECASE | re.MULTILINE)

Ask = Callable[[str, str, int, int], str]  # (system, user, max_tokens, seed) -> reply


def favorite(a: Advocate) -> str:
    return max(a.likes, key=a.likes.get)


@dataclass
class Turn:
    round: int
    speaker: str
    message: str
    before: str       # the ruling the speaker backed going into the turn
    position: str     # and the one it backs after it
    struck: list[str]
    note: str = ""


@dataclass
class Panelist:
    name: str
    lens: str         # what it pays most attention to
    notes: str = ""   # what it saw in the clip before the discussion


def parse(reply: str) -> tuple[str, str]:
    """Splits a reply into what was said and the ruling on its 'POSITION:' line ('' if there isn't one)."""
    named = POSITION.findall(reply)
    body = POSITION.sub("", reply)
    text = " ".join(re.sub(r"^\W*(MESSAGE|REASON|STATEMENT|NOTES)\s*[:\-]\s*", "", body.strip(),
                           flags=re.IGNORECASE).split())
    return text.strip('"* '), (named[-1].lower() if named else "")


class Council:
    def __init__(self, name: str, case: Case, ask: Ask, rounds: int = MAX_ROUNDS,
                 out: Callable[[str], None] = lambda text: print(text, flush=True),
                 look: Chat | None = None, panel: int = len(PANEL)):
        self.name, self.case, self.ask, self.rounds, self._print, self.look = name, case, ask, rounds, out, look
        self.advocates = case.advocates()
        self.officials = self.advocates[0]
        book = {}
        for rulebook in case.rulebooks:
            book.update(load_rulebook(rulebook))
        missing = [r for r in case.rules if r not in book]
        if missing:
            raise ValueError(f"{', '.join(case.rulebooks)} has no clause {', '.join(missing)}")
        self.rules = {r: book[r] for r in case.rules}
        prefixes = sorted({r.split("-")[0] for r in book}, key=len, reverse=True)
        self.rule_id = re.compile(r"\b(?:" + "|".join(map(re.escape, prefixes)) + r")-[A-Za-z0-9.]*[A-Za-z0-9]")
        self.panel = [Panelist(n, lens) for n, lens in PANEL[:panel]] if look else []
        self.frames = self.load_frames() if self.panel else []
        self.lines: list[str] = []
        self.calls = 0
        self.position = {a.name: favorite(a) for a in self.advocates}  # where each advocate stands now
        self.turns: list[Turn] = []                                     # the discussion, in order
        self.statements: dict[str, str] = {}                            # closing statements to the panel

    def out(self, text: str = "") -> None:
        self._print(text)
        self.lines.append(text)

    def ids(self, text: str) -> tuple[list[str], list[str]]:
        """Rule and fact IDs cited in text, split into (listed in this case, struck)."""
        found = dict.fromkeys(self.rule_id.findall(text) + FACT_ID.findall(text))
        listed = [i for i in found if i in self.rules or i in self.case.facts or i in self.case.disputed]
        return listed, [i for i in found if i not in listed]

    def load_frames(self) -> list[tuple[float, bytes]]:
        """Up to PANEL_FRAMES of the case's stills, spread evenly, as (seconds into the clip, JPEG bytes)."""
        paths = [p if p.is_absolute() else HERE / p for p in map(Path, self.case.frames)]
        paths = [p for p in paths if p.exists()]
        if len(paths) > PANEL_FRAMES:
            paths = [paths[round(i * (len(paths) - 1) / (PANEL_FRAMES - 1))] for i in range(PANEL_FRAMES)]
        found = [re.search(r"(\d+(?:\.\d+)?)$", p.stem) for p in paths]
        return [(float(m.group(1)) if m else 0.0, p.read_bytes()) for m, p in zip(found, paths)]

    # ---- what everyone is told ----
    def brief(self) -> str:
        c = self.case
        facts = "\n".join(f"{k}: {v}" for k, v in c.facts.items())
        disputed = "\n".join(f"{k}: {v}" for k, v in c.disputed.items()) or "none"
        rules = "\n".join(f"[{r.id}] {r.title}: {' '.join(r.text.split())}" for r in self.rules.values())
        rulings = "\n".join(f"- {k}: {v}" + (" (the call)" if k == c.call else "") for k, v in c.rulings.items())
        return (f"SPORT: {c.sport}\nINCIDENT: {c.incident}\n"
                f"THE CALL: {c.officials} ruled: {c.rulings[c.call]}. {c.call_reason}\n\n"
                f"ESTABLISHED FACTS (everyone accepts these; cite them by ID):\n{facts}\n\n"
                f"DISPUTED POINTS (the sides see these differently):\n{disputed}\n\n"
                f"RULES (the only rules anyone may cite; cite them by ID):\n{rules}\n\n"
                f"POSSIBLE RULINGS (refer to them by ID):\n{rulings}")

    def system(self, a: Advocate) -> str:
        others = ", ".join(b.name for b in self.advocates if b is not a)
        decides = (("then a neutral panel" + (", who also watch the clip," if self.frames else "") + " decides it, so "
                    "argue to persuade them") if self.panel else
                   "then decide it together; if they don't all agree by the end, it's decided by a vote")
        return (f"You are the advocate for {a.name} on a sports rules council discussing a contested call with "
                f"{others}. {a.side}\n\n{self.brief()}\n\n"
                f"How the council works: the advocates discuss the call in turn, {decides}. Use only the facts and "
                "rules above, citing them by ID; never invent facts, rules, replays, quotes or statistics (a citation of "
                "anything not listed is struck from the record). Speak up for your side, but this is a real discussion: "
                "listen to the others, and change your position only if an argument actually convinces you, not just "
                "to be agreeable. Disagree with arguments, not people.")

    def panel_system(self, p: Panelist) -> str:
        return (f"You are {p.name}, a neutral member of the panel deciding a contested sports call. You don't represent "
                "the officials or any team or player, and nothing rides on the result for you. "
                f"{p.lens} Advocates for the officials and for each party discuss the call; then each panel member "
                "votes on the ruling, independently. Decide on the evidence and the rules, not on who argued hardest. "
                "Use only the facts and rules below" + (", and when you look at the clip, describe only what it actually "
                "shows" if self.frames else "") + ".\n\n" + self.brief())

    def _ask(self, a: Advocate, user: str, max_tokens: int) -> str:
        self.calls += 1
        return self.ask(self.system(a), user, max_tokens, 7 + self.calls)

    def _look(self, p: Panelist, user: str, max_tokens: int) -> str:
        self.calls += 1
        return self.look(self.panel_system(p), user, max_tokens, 7 + self.calls,
                         images=tuple(jpeg for _, jpeg in self.frames))

    def discussion(self, viewer: Advocate | None) -> str:
        """The latest turns as the viewer sees them: its own are labelled 'You', so it doesn't answer itself."""
        return "\n".join(f"- You (backing {t.position}): {t.message}" if viewer and t.speaker == viewer.name else
                         f"- {t.speaker} (backs {t.position}): {t.message}" for t in self.turns[-HISTORY:])

    # ---- the advocates ----
    def speak(self, a: Advocate, rnd: int) -> Turn:
        """One turn: the advocate answers the discussion so far and says which ruling it backs now."""
        c = self.case
        wishes = sorted(c.rulings, key=lambda r: -a.likes[r])
        lines = [f"Round {rnd + 1} of {self.rounds}.",
                 f"What your side would like, best first: {', '.join(wishes)}. You currently back: "
                 f"{self.position[a.name]}."]
        lines += ["THE DISCUSSION SO FAR:", self.discussion(a)] if self.turns else ["Nobody has spoken yet."]
        if not any(t.speaker == a.name for t in self.turns):
            instruction = (f"Open by explaining the call ({c.call}) and why it's right." if a is self.officials else
                           f"Open by making the case for {favorite(a)}, the ruling your side wants.")
        else:
            instruction = ("Respond to what the others have said since you last spoke: answer them by name, accept the "
                           "points you think are right and push back on the ones you think are wrong. Make a new point; "
                           "don't repeat what you've already said. If an argument has changed your mind, change your "
                           "position and say which argument did it.")
        if rnd == self.rounds - 1:
            instruction += " This is the last round of discussion."
        lines += [instruction, "Reply in exactly this format:",
                  "MESSAGE: <two to four sentences, speaking to the others>",
                  f"POSITION: <the ruling you back now: one of {', '.join(c.rulings)}>"]
        reply = self._ask(a, "\n".join(lines), 230)
        message, said = parse(reply)
        before, note = self.position[a.name], ""
        if said not in c.rulings:
            said, note = before, "no valid POSITION line, so its position is unchanged"
        self.position[a.name] = said
        turn = Turn(rnd, a.name, message, before, said, self.ids(reply)[1], note)
        self.turns.append(turn)
        return turn

    def closing_statements(self) -> None:
        for a in self.advocates:
            reply = self._ask(a, "The discussion is over. Here is how it went:\n" + self.discussion(a)
                              + f"\n\nA neutral panel{', who have also watched the clip,' if self.frames else ''} will now "
                              "decide. Give them your closing statement: one or two sentences on why they should rule "
                              f"{self.position[a.name]}. Reply in exactly this format:\nSTATEMENT: <your statement>", 150)
            self.statements[a.name] = parse(reply)[0]

    def closing_vote(self) -> dict[str, tuple[str, str]]:
        """Without a panel: each advocate votes on its own, having heard the discussion but not the other votes."""
        c, votes = self.case, {}
        for a in self.advocates:
            reply = self._ask(a, "The discussion is over. Here is how it went:\n" + self.discussion(a)
                              + f"\n\nYou currently back {self.position[a.name]}. Cast your final vote for the ruling the "
                              "council should make, having heard everyone. Reply in exactly this format:\n"
                              "REASON: <one sentence>\n"
                              f"POSITION: <one of {', '.join(c.rulings)}>", 120)
            reason, said = parse(reply)
            votes[a.name] = (said if said in c.rulings else self.position[a.name], reason)
        return votes

    def decide(self, votes: dict[str, tuple[str, str]]) -> dict:
        """Without a panel: the advocates' closing vote. A tie goes the officials' way, or else the call stands."""
        c = self.case
        tally = Counter(v for v, _ in votes.values())
        leaders = [r for r in c.rulings if tally[r] == max(tally.values())]
        status, choice, how = "VOTE", leaders[0], ""
        if len(leaders) > 1:
            status, officials_vote = "TIE", votes[self.officials.name][0]
            choice, how = (officials_vote, "the officials' vote breaks it") if officials_vote in leaders else \
                (c.call, "the call stands")
        dissent = [f"{n} ({v}): {reason}" for n, (v, reason) in votes.items() if v != choice]
        return {"status": status, "choice": choice, "tally": dict(tally), "votes": votes, "dissent": dissent,
                "how": how, "decided_by": "advocates"}

    # ---- the panel ----
    def first_look(self, p: Panelist) -> str:
        """Before the discussion, the panel member watches the clip and notes what it sees, without deciding."""
        times = ", ".join(stamp(t) for t, _ in self.frames)
        p.notes = parse(self._look(
            p, f"Before hearing the advocates, watch the clip. These {len(self.frames)} frames are from it, at {times}, "
               "in order. Write down what you see that matters for the ruling, and say so if the frames don't show "
               "something important. Don't decide yet. Reply in two to five sentences, starting with 'NOTES:'.", 220))[0]
        return p.notes

    def panel_vote(self, p: Panelist) -> tuple[str, str]:
        """After the discussion: the panel member looks at the frames again and votes. ('', reason) is no valid vote."""
        c = self.case
        closing = "\n".join(f"- {n} (backs {self.position[n]}): {s}" for n, s in self.statements.items())
        clip = (f"YOUR NOTES FROM WATCHING THE CLIP BEFORE THE DISCUSSION: {p.notes}\nThe frames are attached again; "
                "look at them before you decide.\n\n" if self.frames else "")
        reason, said = parse(self._look(
            p, f"THE ADVOCATES' DISCUSSION:\n{self.discussion(None)}\n\nTHEIR CLOSING STATEMENTS:\n{closing}\n\n{clip}"
               "Now vote for the ruling the evidence and the rules support, whichever side that favours. Reply in "
               "exactly this format:\n"
               "REASON: <one or two sentences: what in the clip, the facts or the arguments decided it for you>\n"
               f"POSITION: <one of {', '.join(c.rulings)}>", 200))
        return (said if said in c.rulings else "", reason)

    def panel_decide(self, votes: dict[str, tuple[str, str]]) -> dict:
        """The panel's majority wins. A tie goes to whichever tied ruling more advocates ended up backing; failing
        that, the call stands."""
        c = self.case
        tally = Counter(v for v, _ in votes.values() if v)
        leaders = [r for r in c.rulings if tally and tally[r] == max(tally.values())]
        if len(leaders) == 1:
            status = "CONSENSUS" if tally[leaders[0]] == len(votes) else "VOTE"
            choice, how = leaders[0], ""
        else:
            status, leaders = "TIE", leaders or list(c.rulings)
            backing = Counter(self.position.values())
            favored = [r for r in leaders if backing[r] == max(backing[x] for x in leaders)]
            choice, how = (favored[0], "the advocates' final positions break the tie") if len(favored) == 1 else \
                (c.call, "the call stands")
        dissent = [f"{n} ({v or 'no valid vote'}): {reason}" for n, (v, reason) in votes.items() if v != choice]
        return {"status": status, "choice": choice, "tally": dict(tally), "votes": votes, "dissent": dissent,
                "how": how, "decided_by": "panel"}

    # ---- the whole review ----
    def run(self) -> dict:
        c = self.case
        self.out(f"=== {self.name} ({c.level}): {c.sport} ===")
        self.out(c.title)
        self.out(c.incident)
        self.out(f"The call: {c.officials} ruled: {c.rulings[c.call]}. {c.call_reason}")
        if c.note:
            self.out(c.note)
        for a in self.advocates:
            self.out(f"  {a.name} starts out backing {self.position[a.name]}"
                     + (" (the call)." if a is self.officials else "."))
        if self.panel:
            self.out(f"  Neutral panel: {', '.join(p.name for p in self.panel)}.")
        self.out()
        if self.panel and c.frames and not self.frames:
            self.out("  (The case lists frames from the clip, but they weren't found, so the panel decides without them.)")
        if self.frames:
            self.out(f"  The panel watches the clip first ({len(self.frames)} frames, {stamp(self.frames[0][0])} to "
                     f"{stamp(self.frames[-1][0])})")
            for p in self.panel:
                self.out(f"    {p.name}: {self.first_look(p)}")

        consensus, n = "", len(self.advocates)
        for rnd in range(self.rounds):
            self.out(f"  Round {rnd + 1}")
            changed = False
            for a in self.advocates[rnd % n:] + self.advocates[:rnd % n]:  # a different advocate opens each round
                t = self.speak(a, rnd)
                changed |= t.position != t.before
                stance = f"{t.before} -> {t.position}" if t.position != t.before else t.position
                self.out(f"    {a.name} [{stance}]: {t.message}" + (f" [{t.note}]" if t.note else "")
                         + (f" [struck: {', '.join(t.struck)}, not in this case]" if t.struck else ""))
                if rnd >= 1 and len(set(self.position.values())) == 1:
                    consensus = t.position
                    break
            if consensus:
                self.out(f"    (The advocates all back {consensus}, so the discussion ends here.)")
                break
            if rnd >= 2 and not changed and rnd < self.rounds - 1:
                self.out("    (Nobody changed position this round, so the discussion ends here.)")
                break
        rounds = rnd + 1

        if self.panel:
            self.out("  Closing statements to the panel")
            self.closing_statements()
            for name, statement in self.statements.items():
                self.out(f"    {name} ({self.position[name]}): {statement}")
            self.out("  Panel vote")
            votes = {p.name: self.panel_vote(p) for p in self.panel}
            for name, (vote, reason) in votes.items():
                self.out(f"    {name}: {vote or 'no valid vote'}. {reason}")
            result = self.panel_decide(votes)
        elif consensus:
            result = {"status": "CONSENSUS", "choice": consensus, "tally": {}, "votes": {}, "dissent": [], "how": "",
                      "decided_by": "advocates"}
        else:
            self.out("  Closing vote")
            votes = self.closing_vote()
            for name, (vote, reason) in votes.items():
                self.out(f"    {name}: {vote}. {reason}")
            result = self.decide(votes)
        result.update(rounds=rounds, positions=dict(self.position), upheld=result["choice"] == c.call,
                      notes={p.name: p.notes for p in self.panel if p.notes}, statements=dict(self.statements),
                      frames_seen=[t for t, _ in self.frames])

        r, when = result, f"after {rounds} round{'s' if rounds > 1 else ''}"
        who, size = ("the panel", len(self.panel)) if r["decided_by"] == "panel" else ("the advocates", n)
        verdict = f"{c.rulings[r['choice']].rstrip('.')}. The call is {'upheld' if r['upheld'] else 'overturned'}."
        if r["status"] == "CONSENSUS":
            self.out(f"  DECISION {when}: CONSENSUS, {who} all back {r['choice']}: {verdict}")
        elif r["status"] == "VOTE":
            self.out(f"  DECISION {when}, by {who} ({r['tally'][r['choice']]} of {size}): {verdict}")
        else:
            split = ", ".join(f"{k} {v}" for k, v in r["tally"].items()) or "no valid votes"
            self.out(f"  DECISION {when}: {who} are split ({split}), so {r['how']}: {verdict}")
        for d in r["dissent"]:
            self.out(f"    Dissent: {d}")
        if r["decided_by"] == "panel":
            self.out("    The advocates' final positions: " + "; ".join(f"{k} {v}" for k, v in self.position.items()))
        return result


def debate(name: str, case: Case, ask: Ask, rounds: int, transcript: Path, header: str,
           look: Chat | None = None, panel: int = len(PANEL), out: Callable[[str], None] | None = None) -> dict:
    """Runs the council on one case and saves the transcript. out gets each transcript line (default: print)."""
    started = time.time()
    council = Council(name, case, ask, rounds, look=look, panel=panel, **({"out": out} if out else {}))
    result = council.run()
    models = MODEL + (f" for the advocates, {VISION_MODEL} for the panel" if council.panel else "")
    council.out(f"  ({models}; {council.calls} model calls, {time.time() - started:.0f}s)")
    transcript.parent.mkdir(parents=True, exist_ok=True)
    transcript.write_text(header + "\n\n" + "\n".join(council.lines) + "\n")
    return result


DEMO_HEADER = ("> **Simulation.** Every advocate and panel member is played by an AI model. The teams and players are "
               "fictional, and the soccer, basketball and golf rule excerpts in rules/ are paraphrased.")
SAVED_HEADER = ("> Every advocate and panel member is played by an AI model. The case was built by AI models from a "
                "video and the user's description; check its facts in case.json.")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("case", nargs="?", default="all", choices=[*CASES, "all"])
    ap.add_argument("--case-file", type=Path, help="discuss a saved case instead, such as one review.py built")
    ap.add_argument("--rounds", type=int, default=MAX_ROUNDS, help=f"most rounds of discussion (default {MAX_ROUNDS})")
    ap.add_argument("--panel", type=int, default=len(PANEL), choices=range(len(PANEL) + 1),
                    help=f"neutral panel members who decide (default {len(PANEL)}; 0: the advocates decide)")
    args = ap.parse_args(argv)
    ask = connect(MODEL)
    look = connect(VISION_MODEL) if args.panel else None

    if args.case_file:
        name = args.case_file.parent.name if args.case_file.stem == "case" else args.case_file.stem
        debate(name, load_case(args.case_file), ask, args.rounds, args.case_file.with_name("transcript.md"),
               SAVED_HEADER, look, args.panel)
        return 0

    rows = []
    for name in (list(CASES) if args.case == "all" else [args.case]):
        rows.append((name, CASES[name], debate(name, CASES[name], ask, args.rounds,
                                               HERE / "transcripts" / f"{name}.md", DEMO_HEADER, look, args.panel)))
        print()
    print(f"{'case':<20}{'kind':<16}{'decision':<28}{'by':<14}{'rounds':>6}  the call")
    for name, case, r in rows:
        print(f"{name:<20}{case.level:<16}{r['status'] + ': ' + r['choice']:<28}{r['decided_by']:<14}{r['rounds']:>6}  "
              f"{'upheld' if r['upheld'] else 'overturned'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
