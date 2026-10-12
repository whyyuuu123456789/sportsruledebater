"""Builds a case from what the user says and what the video shows, in five small model calls the code checks:

  1. understand  the sport, the incident, the call and who made it, and every party the call affects
  2. rules       pick the sport's rulebooks in rules/, shortlist clauses by keyword search, let the model choose
  3. rulings     the call, plus the other rulings those rules allow
  4. facts       established facts (said by the user, or plainly visible) and disputed points (everything that
                 turns on judgement). Each fact keeps its source, so people can check it.
  5. positions   each party's advocate ranks the rulings for its side: where it starts the discussion
"""

from __future__ import annotations

import json
import re
from typing import Callable

from cases import Advocate, Case
from evidence import Observation
from library import Rule, load_rulebook, rulebooks, search, title_of
from llm import Chat

SYSTEM = ("You turn a contested sports officiating decision into a case file for a rules review. Use only what the "
          "user says and what the video observations show. Never invent names, scores, times or events; if something "
          "isn't known, leave it empty. Answer in JSON.")
SHORTLIST = 15
EXAMPLE = ('EXAMPLE from a different case, to show the shape only: {"sport": "basketball", "title": "Catch-and-shoot '
           'with 0.2 seconds left", "officials": "Game officials", "call": "No basket", "parties": [{"name": "Lakeside '
           'Lynx", "wants": "the basket to count", "stake": "they win 101-100 if it counts"}, {"name": "Metro Monarchs", '
           '"wants": "the basket waved off", "stake": "they win 100-99 if it is waved off"}]}')


class IntakeError(Exception):
    """The case can't be built without more from the user."""


def slug(text: str) -> str:
    return re.sub(r"[^a-z0-9]+", "_", str(text).lower()).strip("_")[:30].strip("_")


def text(value) -> str:
    """A model's answer as one string, even when it sent a list."""
    return " ".join(map(str, value)).strip() if isinstance(value, list) else str(value or "").strip()


def snippet(text: str, words: int) -> str:
    w = text.split()
    return " ".join(w[:words]) + (" ..." if len(w) > words else "")


class Intake:
    def __init__(self, chat: Chat, context: str, evidence: list[Observation],
                 log: Callable[[str], None] | None = None):
        self.chat, self.context, self.evidence, self.log = chat, context.strip(), evidence, log or print
        self.calls = 0
        self.warnings: list[str] = []  # things the user should check before the debate

    def ask(self, user: str, max_tokens: int, system: str = SYSTEM) -> dict:
        self.calls += 1
        reply = self.chat(system, user, max_tokens, 100 + self.calls, json_mode=True)
        m = re.search(r"\{.*\}", reply, re.DOTALL)
        try:
            data = json.loads(m.group(0) if m else reply)
        except json.JSONDecodeError:
            return {}
        return data if isinstance(data, dict) else {}

    def known(self) -> str:
        observed = "\n".join(map(str, self.evidence)) or "no video"
        return f"WHAT THE USER SAYS:\n{self.context or 'nothing'}\n\nVIDEO OBSERVATIONS:\n{observed}"

    # 1
    def understand(self) -> dict:
        books = {name: title_of(path) for name, path in rulebooks().items()}
        d = self.ask(
            f"{self.known()}\n\nRULEBOOKS AVAILABLE (name: title):\n"
            + "\n".join(f"- {n}: {t}" for n, t in books.items()) + "\n\n"
            "Return JSON with these keys:\n"
            '"sport": the sport\n'
            '"rulebooks": the names of the rulebooks above that cover this sport (a list; empty if none do)\n'
            '"title": a one-line title for the case\n'
            '"incident": two or three neutral sentences on what happened\n'
            '"officials": who made the call, for example "Referee & VAR"\n'
            '"call": what the officials ruled, in a few words\n'
            '"call_reason": the reason the officials gave, or "" if unknown\n'
            '"parties": every team or player the call affects (at least two), as a list of {"name": ..., '
            '"wants": the outcome they want, in a few words, "stake": what they gain or lose}\n\n'
            + EXAMPLE, 800)
        info = {k: text(d.get(k)) for k in ("sport", "title", "incident", "officials", "call", "call_reason")}
        if not info["call"]:
            raise IntakeError("I couldn't tell what the officials ruled. Say it in --context.")
        parties, seen = [], set()
        for p in d.get("parties") or []:
            p = p if isinstance(p, dict) else {"name": p}
            name = text(p.get("name"))
            if name and name.lower() not in seen:
                seen.add(name.lower())
                parties.append({"name": name, "wants": text(p.get("wants")), "stake": text(p.get("stake"))})
        if len(parties) < 2:
            raise IntakeError("I couldn't identify at least two parties. Name the teams or players in --context.")
        picked = [b for b in d.get("rulebooks") or [] if isinstance(b, str) and b in books]
        if not picked and info["sport"]:  # fall back to rulebooks whose name or title mentions the sport
            picked = [n for n, t in books.items() if info["sport"].lower() in f"{n} {t}".lower()]
        if not picked:
            raise IntakeError(f"No rulebook in rules/ covers {info['sport'] or 'this sport'}. Add one (markdown or "
                              "PDF) and run it again.")
        return {**info, "parties": parties, "rulebooks": picked, "officials": info["officials"] or "Officials",
                "title": info["title"] or info["call"]}

    # 2
    def find_rules(self, info: dict) -> dict[str, Rule]:
        clauses: dict[str, Rule] = {}
        for name in info["rulebooks"]:
            clauses.update(load_rulebook(name))
        query = " ".join([info["incident"], info["call"], info["call_reason"], self.context])
        shortlist = list(clauses.values()) if len(clauses) <= SHORTLIST else search(clauses, query, SHORTLIST)
        if not shortlist:
            raise IntakeError(f"Nothing in {', '.join(info['rulebooks'])} matches this incident.")
        d = self.ask(
            f"INCIDENT: {info['incident']}\nTHE CALL: {info['officials']} ruled: {info['call']}. {info['call_reason']}\n\n"
            "CANDIDATE CLAUSES:\n" + "\n".join(f"[{r.id}] {r.title}: {snippet(r.text, 45)}" for r in shortlist) +
            '\n\nWhich of these clauses could decide this case? Return JSON {"rules": [clause IDs]} with 3 to 8 IDs, '
            "most relevant first.", 200)
        ids = {r.id for r in shortlist}
        picked = list(dict.fromkeys(i for i in (str(x).strip("[] ") for x in d.get("rules") or []) if i in ids))
        if len(picked) < 2:
            picked = [r.id for r in shortlist[:6]]
        return {i: clauses[i] for i in picked[:8]}

    # 3
    def find_rulings(self, info: dict, rules: dict[str, Rule]) -> tuple[dict[str, str], str]:
        d = self.ask(
            f"INCIDENT: {info['incident']}\nTHE CALL: {info['officials']} ruled: {info['call']}. {info['call_reason']}\n\n"
            "RULES:\n" + "\n".join(f"[{r.id}] {r.title}: {snippet(r.text, 80)}" for r in rules.values()) + "\n\n"
            "WHAT EACH PARTY WANTS: " + "; ".join(f"{p['name']} wants {p['wants'] or '?'}" for p in info["parties"])
            + "\n\nList the rulings a review could reach, 2 to 4 in all. A ruling is an outcome the officials could give "
            "(what happens in the game), not a reason or a finding. The first ruling must be the call itself, and the "
            "list must include the outcome each party wants. "
            'Return JSON {"rulings": [{"id": a short snake_case id, "text": the outcome and what it means for each '
            'side}], "call_id": the id of the call}. Example from a different case: {"rulings": [{"id": "no_basket", '
            '"text": "No basket: the Monarchs win 100-99"}, {"id": "basket_counts", "text": "The basket counts: the '
            'Lynx win 101-100"}], "call_id": "no_basket"}', 400)
        rulings: dict[str, str] = {}
        for r in d.get("rulings") or []:
            if isinstance(r, dict) and (r.get("text") or r.get("id")):
                rid = slug(r.get("id") or r.get("text")) or f"ruling_{len(rulings) + 1}"
                rulings.setdefault(rid, str(r.get("text") or r.get("id")).strip())
        call = slug(d.get("call_id", ""))
        if call not in rulings and rulings:  # the call is meant to come first
            call = next(iter(rulings))
            self.warnings.append(f"Check that {call!r} is the call the officials made.")
        if call not in rulings:
            call = "call"
            rulings = {"call": info["call"], **rulings}
        if len(rulings) < 2:
            raise IntakeError("I could only find the call itself, no other ruling. Say in --context what each side "
                              "wants instead.")
        return dict(list(rulings.items())[:5]) | {call: rulings[call]}, call

    # 4
    def find_facts(self, info: dict, rulings: dict[str, str], rules: dict[str, Rule]) -> tuple[dict, dict]:
        d = self.ask(
            f"{self.known()}\n\nINCIDENT: {info['incident']}\nRULINGS IN QUESTION: "
            + "; ".join(rulings.values()) + "\nRULES THAT APPLY: " + "; ".join(r.title for r in rules.values()) + "\n\n"
            'Split what is known into facts and disputed points. Return JSON {"facts": [{"text": ..., "source": '
            '"context" or "video m:ss.s"}], "disputed": [{"text": ...}]}. A fact is something everyone would accept: '
            "said by the user, or plainly visible in the video. Anything that turns on fine judgement (exact "
            "positions or timing, intent, what might have happened) is a disputed point. Use a video observation only "
            "if it matters for the rulings, and never one that contradicts what the user says. Give 3 to 8 facts and "
            "1 to 4 disputed points, one short sentence each.", 700)
        facts: dict[str, str] = {}
        for f in d.get("facts") or []:
            f = f if isinstance(f, dict) else {"text": f}
            fact, source = text(f.get("text")), text(f.get("source")).lower()
            if fact:
                where = f"video {source.removeprefix('video').strip()}" if source.startswith("video") else "your context"
                facts[f"F{len(facts) + 1}"] = f"{fact} [{where}]"
        if not facts:
            raise IntakeError("I couldn't establish any facts. Add what everyone agrees on to --context.")
        disputed = {f"D{i}": text(p.get("text") if isinstance(p, dict) else p)
                    for i, p in enumerate(d.get("disputed") or [], 1)}
        return facts, {k: v for k, v in disputed.items() if v}

    # 5
    def take_positions(self, info: dict, rulings: dict[str, str]) -> list[Advocate]:
        advocates = []
        for p in info["parties"]:
            wants = f"{p['name']} wants {p['wants']}. " if p["wants"] else ""
            d = self.ask(
                f"INCIDENT: {info['incident']}\n{wants}{p['stake']}\n\nPOSSIBLE RULINGS:\n"
                + "\n".join(f"- {k}: {v}" for k, v in rulings.items())
                + f"\n\nRank every ruling from best to worst for {p['name']}: first the ruling that gives {p['name']} "
                "what it wants, last the one that hurts it most. "
                'Return JSON {"ranking": [ruling ids, best first]}', 150,
                system=f"You are the advocate for {p['name']} in a sports rules review. Answer in JSON.")
            ranking = list(dict.fromkeys(slug(r) for r in d.get("ranking") or [] if slug(r) in rulings))
            ranking += [r for r in rulings if r not in ranking]
            likes = {r: round(1 - i / (len(ranking) - 1), 2) for i, r in enumerate(ranking)}
            side = f"You represent {p['name']}. {wants}{p['stake']}".strip()
            advocates.append(Advocate(p["name"], side, likes))
            self.log(f"  {p['name']}: wants {ranking[0]}")
        favorites = {max(a.likes, key=a.likes.get) for a in advocates}
        if len(favorites) == 1:
            self.warnings.append(f"Every party's first choice is {favorites.pop()}. In a real dispute the parties want "
                                 "different rulings, so check each party's likes in case.json.")
        return advocates

    def build(self, level: str) -> Case:
        info = self.understand()
        self.log(f"  {info['sport']}: {info['title']}")
        rules = self.find_rules(info)
        self.log(f"  rules: {', '.join(rules)}")
        rulings, call = self.find_rulings(info, rules)
        facts, disputed = self.find_facts(info, rulings, rules)
        parties = self.take_positions(info, rulings)
        return Case(sport=info["sport"], level=level, title=info["title"], incident=info["incident"],
                    officials=info["officials"], call=call, call_reason=info["call_reason"], rulings=rulings,
                    facts=facts, disputed=disputed, rulebook=info["rulebooks"], rules=tuple(rules), parties=parties)
