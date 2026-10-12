"""Sports rules council: advocates for the officials and for every party argue over a contested call.

Built on the Lungo Council's rules. One advocate defends the call the officials made, and every party involved,
however many there are, gets its own advocate. Every round, each advocate argues for one ruling, citing the
rulebook and the facts, then votes on every ruling someone argued for (and the call): APPROVE, REJECT or VETO.
As in Lungo, where agents vote only on proposals, a ruling nobody argues for can't be adopted.

The code, not the model, enforces the council's rules:
  * A side's stake decides what its advocate MAY concede. Each round it can accept a little more, never below
    its floor. An approval it can't give yet is blocked and counted as REJECT.
  * The rules and the facts decide what it DOES concede: the model votes on the merits, without being told
    which rulings its side can accept.
  * A veto must cite a listed rule and an established fact (F#), or it's struck and counted as REJECT.
  * Citations of rules or facts that aren't in the case are struck from the record.

No one is in charge. The council ends in one of three ways, checked against the public record:
  CONSENSUS     every advocate approves the same ruling: the call is upheld or overturned
  CALL_STANDS   no consensus, but the officials' advocate still backs the call, so it stands and the dissent
                is recorded (the "clear and obvious error" standard)
  ESCALATE      the officials gave up the call and nothing replaces it, or a grounded rule objection to a call
                that decides a result is unresolved: a human decides

    python3 council.py                  # every case (needs Ollama running, or LLM_MODEL set)
    python3 council.py dogso_card       # one case
    python3 council.py all --rounds 4   # fewer rounds
    python3 council.py --case-file cases/<name>/case.json   # a saved case, such as one review.py built from a video
"""

from __future__ import annotations

import argparse
import re
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

from cases import CASES, Advocate, Case, load_case
from library import load_rulebook
from llm import MODEL, connect

EPS = 1e-9
MAX_ROUNDS = 6
HERE = Path(__file__).resolve().parent
FACT_ID = re.compile(r"\b[FD]\d+\b")
VOTE_LINE = re.compile(r"^[^A-Za-z0-9]*(?:\d+[.)]\s*)?([A-Za-z][A-Za-z0-9_]*)\b.{0,40}?\b(APPROVE|REJECT|VETO)\b(.*)$",
                       re.IGNORECASE)

Ask = Callable[[str, str, int, int], str]  # (system, user, max_tokens, seed) -> reply


def bar(a: Advocate, rnd: int) -> float:
    return max(a.floor, 1 - a.concession * rnd)


def can_accept(a: Advocate, ruling: str, rnd: int) -> bool:
    return a.likes[ruling] >= bar(a, rnd) - EPS


def refusal(a: Advocate, ruling: str) -> str:
    return f"{a.name} will never concede {ruling}" if a.likes[ruling] < a.floor - EPS else \
        f"{a.name} can't concede {ruling} yet"


@dataclass
class Vote:
    kind: str           # APPROVE, REJECT or VETO
    reason: str = ""
    rule: str = ""      # a veto's grounds: a listed rule and an established fact
    fact: str = ""
    note: str = ""      # what the code changed, and why

    def __str__(self) -> str:
        grounds = f" ({self.rule} + {self.fact})" if self.kind == "VETO" else ""
        return f"{self.kind}{grounds}" + (f": {self.reason}" if self.reason else "") + \
            (f" [{self.note}]" if self.note else "")


@dataclass
class Argument:
    ruling: str
    text: str
    struck: list[str]
    note: str = ""


def parse_votes(reply: str, rulings: list[str]) -> dict[str, tuple[str, str]]:
    """One line per ruling, e.g. 'pen_red: VETO LOTG-12.3 F2 - ...'. Returns {ruling: (kind, rest of line)}."""
    found: dict[str, tuple[str, str]] = {}
    for line in reply.splitlines():
        m = VOTE_LINE.match(line.strip())
        if m and m.group(1).lower() in rulings and m.group(1).lower() not in found:
            found[m.group(1).lower()] = (m.group(2).upper(), m.group(3).strip(" -:–—*"))
    return found


class Council:
    def __init__(self, name: str, case: Case, ask: Ask, rounds: int = MAX_ROUNDS,
                 out: Callable[[str], None] = lambda text: print(text, flush=True)):
        self.name, self.case, self.ask, self.rounds, self._print = name, case, ask, rounds, out
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
        self.lines: list[str] = []
        self.calls = 0
        self.position: dict[str, str] = {}  # the ruling each advocate argued for last
        self.history: list[dict] = []       # the public record: each round's arguments and votes
        self.last_kinds: dict | None = None

    def out(self, text: str = "") -> None:
        self._print(text)
        self.lines.append(text)

    # ---- citations ----
    def ids(self, text: str) -> tuple[list[str], list[str]]:
        """Rule and fact IDs cited in text, split into (listed in this case, struck)."""
        found = dict.fromkeys(self.rule_id.findall(text) + FACT_ID.findall(text))
        listed = [i for i in found if i in self.rules or i in self.case.facts or i in self.case.disputed]
        return listed, [i for i in found if i not in listed]

    def strip_ids(self, text: str) -> str:
        """'LOTG-12.3 F2 - He went for the ball' -> 'He went for the ball'."""
        if re.match(r"[\s(\[]*(?:" + self.rule_id.pattern + "|" + FACT_ID.pattern + ")", text):
            text = re.sub(r"^.*?[-–—:]\s+", "", text, count=1)
        return text

    # ---- talking to the model ----
    def system(self, a: Advocate) -> str:
        c = self.case
        facts = "\n".join(f"{k}: {v}" for k, v in c.facts.items())
        disputed = "\n".join(f"{k}: {v}" for k, v in c.disputed.items()) or "none"
        rules = "\n".join(f"[{r.id}] {r.title}: {' '.join(r.text.split())}" for r in self.rules.values())
        rulings = "\n".join(f"- {k}: {v}" + (" (the call)" if k == c.call else "") for k, v in c.rulings.items())
        return (f"You are the advocate for {a.name} on a sports rules council reviewing a contested call. {a.side}\n\n"
                f"SPORT: {c.sport}\nINCIDENT: {c.incident}\n"
                f"THE CALL: {c.officials} ruled: {c.rulings[c.call]}. {c.call_reason}\n\n"
                f"ESTABLISHED FACTS (everyone accepts these; cite them by ID):\n{facts}\n\n"
                f"DISPUTED POINTS (you may argue these, but they can't prove a ruling breaks a rule):\n{disputed}\n\n"
                f"RULES (the only rules anyone may cite; cite them by ID):\n{rules}\n\n"
                f"POSSIBLE RULINGS (refer to them by ID):\n{rulings}\n\n"
                "Council rules: use only the facts and rules above. Never invent facts, rules, replays, quotes or "
                "statistics; a citation of a rule or fact that isn't listed is struck from the record. The council can "
                "only adopt a ruling that some advocate argues for. Argue hard for your side, but be honest about what "
                "the rules say. Disagree with arguments, not people.")

    def _ask(self, a: Advocate, user: str, max_tokens: int) -> str:
        self.calls += 1
        return self.ask(self.system(a), user, max_tokens, 7 + self.calls)

    def argue(self, a: Advocate, rnd: int) -> Argument:
        c, last = self.case, (self.history[-1] if self.history else None)
        allowed = [r for r in c.rulings if can_accept(a, r, rnd)]
        refused = [r for r in c.rulings if r not in allowed]
        newly = [r for r in allowed if rnd > 0 and not can_accept(a, r, rnd - 1)]
        prev = self.position.get(a.name) or max(c.rulings, key=lambda r: a.likes[r])
        lines = [f"Round {rnd + 1} of at most {self.rounds}.",
                 f"Your side's first choice: {max(c.rulings, key=lambda r: a.likes[r])}. Rulings your side could "
                 f"accept this round: {', '.join(allowed)}." + (f" It won't accept: {', '.join(refused)}." if refused else "")]
        if last:
            lines.append("Last round's arguments:")
            lines += [f"- {n} (for {arg.ruling}): {arg.text}" for n, arg in last["arguments"].items() if n != a.name]
            lines.append(f"Your last argument (make a new point, don't repeat it): {last['arguments'][a.name].text}")
            objections = [f"- {n}: {vs[prev]}" for n, vs in last["votes"].items()
                          if n != a.name and vs[prev].kind != "APPROVE"]
            if objections:
                lines += [f"Objections to {prev} last round:", *objections]
        if rnd == 0:
            instruction = f"Make your opening argument for {prev}."
        else:
            instruction = "Answer the strongest objection to your ruling with the rules and facts, or make your best new point."
            if newly:
                instruction += (f" Your side can now also accept {', '.join(newly)}. The council only adopts a "
                                f"ruling someone argues for, so if the rules and facts are against {prev}, switch to "
                                "the ruling they support.")
        if rnd == self.rounds - 1:
            instruction += " This is the last round."
        lines += [instruction, "Reply in exactly this format:",
                  "ARGUMENT: <two to four sentences in the first person, citing rule and fact IDs>",
                  f"RULING: <one ID from: {', '.join(allowed)}>"]
        reply = self._ask(a, "\n".join(lines), 220)

        m = re.search(r"RULING\W*([A-Za-z0-9_]+)", reply, re.IGNORECASE)
        said = m.group(1).lower() if m else ""
        body = "\n".join(l for l in reply.splitlines() if not re.match(r"\W*(RULING|CITES)\b", l, re.IGNORECASE))
        text = " ".join(re.sub(r"^\W*ARGUMENT\s*[:\-]\s*", "", body.strip(), flags=re.IGNORECASE).split()).strip('"*')
        struck = self.ids(reply)[1]
        if said in allowed:
            return Argument(said, text, struck)
        why = refusal(a, said) if said in c.rulings else "no valid RULING line"
        return Argument(prev, text, struck, note=f"{why}; still argues for {prev}")

    def vote(self, a: Advocate, rnd: int, arguments: dict[str, Argument], table: list[str]) -> dict[str, Vote]:
        """A merits vote on the rulings on the table. The model isn't told which rulings its side can accept: telling
        it made it approve whatever its side could live with. check() applies the stakes afterwards."""
        c = self.case
        own = arguments[a.name].ruling
        others = [r for r in table if r != own]
        votes = {own: Vote("APPROVE", "the ruling it argued for")}
        if others:
            lines = [f"Round {rnd + 1}. This round's arguments:"]
            lines += [f"- {n} (for {arg.ruling}): {arg.text}" for n, arg in arguments.items()]
            lines.append("Now vote on each ruling below as a neutral rules official would: judge it only by the rules "
                         "and the established facts, not by what is good for your side.")
            lines += [f"- {r}: {c.rulings[r]}" for r in others]
            lines += ["Write exactly one line per ruling and nothing else, each in one of these forms:",
                      "<ruling ID>: APPROVE - <one sentence: why the rules and facts support it>",
                      "<ruling ID>: REJECT - <one sentence: why you're not persuaded>",
                      "<ruling ID>: VETO <rule ID> <fact ID> - <one sentence>",
                      "VETO only when an established fact (F#) and a rule together make the ruling impossible. If it "
                      "turns on a disputed point (D#) or a judgment call, use REJECT."]
            found = parse_votes(self._ask(a, "\n".join(lines), 60 + 70 * len(others)), others)
            for r in others:
                votes[r] = self.check(a, r, rnd, *found.get(r, ("", "")))
        return {r: votes[r] for r in table}

    def check(self, a: Advocate, ruling: str, rnd: int, kind: str, rest: str) -> Vote:
        """Enforces the council's rules on one vote."""
        if not kind:
            return Vote("REJECT", note="no clear vote; counted as REJECT")
        if kind == "VETO":
            cited = self.ids(rest)[0]
            rules = [i for i in cited if i in self.rules]
            facts = [i for i in cited if i in self.case.facts]
            if rules and facts:
                return Vote("VETO", self.strip_ids(rest), rules[0], facts[0])
            why = "a veto must cite a listed rule and an established fact (F#)"
            if any(i in self.case.disputed for i in cited):
                why += "; D# points are disputed"
            return Vote("REJECT", rest, note=f"veto struck: {why}")
        if kind == "APPROVE" and not can_accept(a, ruling, rnd):
            return Vote("REJECT", rest, note=f"approval blocked: {refusal(a, ruling)}")
        return Vote(kind, rest)

    # ---- deciding, from the public record ----
    def decide(self, rnd: int, votes: dict[str, dict[str, Vote]]) -> dict | None:
        c, advs = self.case, self.advocates
        table = list(votes[self.officials.name])
        approvers = {r: [a.name for a in advs if votes[a.name][r].kind == "APPROVE"] for r in table}
        everyone = [r for r in table if len(approvers[r]) == len(advs)]
        if everyone:
            choice = c.call if c.call in everyone else max(
                everyone, key=lambda r: (min(a.likes[r] - bar(a, rnd) for a in advs), sum(a.likes[r] for a in advs)))
            return {"status": "CONSENSUS", "choice": choice, "rounds": rnd + 1,
                    "reason": "the call is upheld" if choice == c.call else "the call is overturned"}
        kinds = {(n, r): v.kind for n, vs in votes.items() for r, v in vs.items()}
        settled = all(bar(a, rnd) <= a.floor + EPS for a in advs) and kinds == self.last_kinds
        self.last_kinds = kinds
        if settled or rnd == self.rounds - 1:
            return self.stalemate(rnd, votes)
        return None

    def stalemate(self, rnd: int, votes: dict[str, dict[str, Vote]]) -> dict:
        c = self.case
        on_call = {n: vs[c.call] for n, vs in votes.items()}
        dissent = [f"{n}: {v}" for n, v in on_call.items() if v.kind != "APPROVE"]
        grounded = [n for n, v in on_call.items() if v.kind == "VETO"]
        result = {"rounds": rnd + 1, "dissent": dissent}
        if on_call[self.officials.name].kind == "APPROVE" and not (grounded and c.high_stakes):
            why = "No ruling has everyone's approval, but the officials still back the call, and " + (
                "nobody showed it breaks a rule" if not grounded else
                "the grounded objections to it are on record, but it doesn't decide a result")
            return {**result, "status": "CALL_STANDS", "choice": c.call, "reason": why}
        if on_call[self.officials.name].kind != "APPROVE":
            why = "The officials' advocate no longer backs the call, and no other ruling has everyone's approval"
        else:
            why = (f"{' and '.join(grounded)} raised a rule objection grounded in an established fact that the "
                   "officials haven't conceded, and the call decides a result")
        return {**result, "status": "ESCALATE", "reason": why}

    # ---- the transcript ----
    def run(self) -> dict:
        c = self.case
        self.out(f"=== {self.name} ({c.level}): {c.sport} ===")
        self.out(c.title)
        self.out(c.incident)
        self.out(f"The call: {c.officials} ruled: {c.rulings[c.call]}. {c.call_reason}")
        if c.note:
            self.out(c.note)
        for a in self.advocates:
            fav = max(c.rulings, key=lambda r: a.likes[r])
            settle = [r for r in c.rulings if r != fav and a.likes[r] >= a.floor - EPS]
            verb = "defends" if a is self.officials else "wants"
            self.out(f"  {a.name} {verb} {fav}" + (f"; could settle for {', '.join(settle)}" if settle else "") + ".")
        self.out()

        result: dict = {}
        for rnd in range(self.rounds):
            self.out(f"  Round {rnd + 1}")
            arguments: dict[str, Argument] = {}
            for a in self.advocates:
                arg = arguments[a.name] = self.argue(a, rnd)
                self.position[a.name] = arg.ruling
                self.out(f"    {a.name}, for {arg.ruling}: {arg.text}" + (f" [{arg.note}]" if arg.note else "")
                         + (f" [struck: {', '.join(arg.struck)}, not in this case]" if arg.struck else ""))
            argued = {arg.ruling for arg in arguments.values()}
            table = [r for r in c.rulings if r == c.call or r in argued]
            votes = {a.name: self.vote(a, rnd, arguments, table) for a in self.advocates}
            self.history.append({"arguments": arguments, "votes": votes})
            for r in table:
                yes = [n for n, vs in votes.items() if vs[r].kind == "APPROVE"]
                label = r + (" (the call)" if r == c.call else "")
                self.out(f"    Votes on {label}: " + (f"approved by {', '.join(yes)}" if yes else "nobody approves"))
                for n, vs in votes.items():
                    if vs[r].kind != "APPROVE":
                        self.out(f"      {n} {vs[r]}")
            result = self.decide(rnd, votes) or {}
            if result:
                break

        r = result
        when = f"after {r['rounds']} round{'s' if r['rounds'] > 1 else ''}"
        if r["status"] == "CONSENSUS":
            self.out(f"  DECISION {when}: CONSENSUS, {r['reason']}: {c.rulings[r['choice']].rstrip('.')}.")
        elif r["status"] == "CALL_STANDS":
            self.out(f"  DECISION {when}: CALL STANDS: {c.rulings[c.call].rstrip('.')}. {r['reason']}.")
            for d in r["dissent"]:
                self.out(f"    Dissent on record: {d}")
        else:
            self.out(f"  DECISION {when}: ESCALATE to {c.escalate_to}. {r['reason']}.")
            last = self.history[-1]["votes"]
            self.out("  Options for the human to weigh (final round's votes):")
            for ruling in sorted(last[self.officials.name], key=lambda x: -sum(vs[x].kind == "APPROVE" for vs in last.values())):
                yes = [n for n, vs in last.items() if vs[ruling].kind == "APPROVE"]
                no = [f"{n} ({vs[ruling].kind}" + (f" {vs[ruling].rule} + {vs[ruling].fact}" if vs[ruling].kind == "VETO"
                                                   else "") + ")" for n, vs in last.items() if vs[ruling].kind != "APPROVE"]
                self.out(f"    - {c.rulings[ruling]}: backed by {', '.join(yes) or 'nobody'}; opposed by {', '.join(no)}")
        return result


def debate(name: str, case: Case, ask: Ask, rounds: int, transcript: Path, header: str) -> dict:
    """Runs the council on one case and saves the transcript."""
    started = time.time()
    council = Council(name, case, ask, rounds)
    result = council.run()
    council.out(f"  ({MODEL}, {council.calls} model calls, {time.time() - started:.0f}s)")
    transcript.parent.mkdir(parents=True, exist_ok=True)
    transcript.write_text(header + "\n\n" + "\n".join(council.lines) + "\n")
    return result


DEMO_HEADER = ("> **Simulation.** Every advocate is played by an AI model. The teams and players are fictional, and "
               "the rule excerpts in rules/ are paraphrased.")
SAVED_HEADER = ("> Every advocate is played by an AI model. The case was built by AI models from a video and the "
                "user's description; check its facts in case.json before trusting the outcome.")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("case", nargs="?", default="all", choices=[*CASES, "all"])
    ap.add_argument("--case-file", type=Path, help="debate a saved case instead, such as one review.py built")
    ap.add_argument("--rounds", type=int, default=MAX_ROUNDS, help=f"most rounds per case (default {MAX_ROUNDS})")
    args = ap.parse_args(argv)
    ask = connect(MODEL)

    if args.case_file:
        name = args.case_file.parent.name if args.case_file.stem == "case" else args.case_file.stem
        debate(name, load_case(args.case_file), ask, args.rounds, args.case_file.with_name("transcript.md"),
               SAVED_HEADER)
        return 0

    rows = []
    for name in (list(CASES) if args.case == "all" else [args.case]):
        result = debate(name, CASES[name], ask, args.rounds, HERE / "transcripts" / f"{name}.md", DEMO_HEADER)
        rows.append((name, CASES[name], result))
        print()

    print(f"{'case':<20}{'level':<12}{'result':<28}{'rounds':>6}  rules expert would expect")
    for name, case, r in rows:
        got = r["status"] + (f": {r['choice']}" if r["status"] == "CONSENSUS" else "")
        want = case.expect + (f": {case.expect_ruling}" if case.expect_ruling else "")
        print(f"{name:<20}{case.level:<12}{got:<28}{r['rounds']:>6}  {'same' if got == want else want}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
