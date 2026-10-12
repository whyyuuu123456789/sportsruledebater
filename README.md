# Sports Rules Council: advocates who don't agree about a call

A council that reviews a contested officiating decision, built on the Lungo Council's rules
(`aiagentdisagreethingy`). One advocate defends the call the officials made. Every party the call affects gets
its own advocate, however many there are: two teams, or three golfers whose finishing places depend on one
ruling. Nobody is in charge. The advocates argue from a public rulebook and a list of facts everyone accepts,
vote on every possible ruling, and the outcome follows from the votes.

```
python3 council.py                    # every case (needs Ollama running, or LLM_MODEL set)
python3 council.py dogso_card         # one case
python3 council.py all --rounds 4     # fewer rounds
python3 -m unittest discover -s tests -t .   # protocol tests, with a scripted model (no LLM needed)
```

Standard-library Python 3.12+. The model comes from `LLM_MODEL` (default `ollama_chat/llama3.1:8b`). If
litellm is installed it is used, so any litellm model works (for example `anthropic/claude-sonnet-5-5`);
otherwise the council talks to Ollama directly. Transcripts are saved to `transcripts/`.

## From the Lungo Council to a sports council

| Lungo Council | Sports council |
|---|---|
| Plans proposed each round | Rulings argued for each round, plus the call (from `rulings`) |
| Four stakeholder agents | The officials' advocate, plus one advocate per party (`parties`, any number) |
| Public scoring formulas | A public rulebook (`rules/*.md`), established facts (F1, F2…) and disputed points (D1, D2…) |
| Private floor and concession rate | Each side's stake in each ruling (`likes`), `concession` and `floor`: what its advocate *may* concede, and when |
| A veto reveals a hard limit | A veto must cite a listed rule **and** an established fact; otherwise the code strikes it |
| PROCEED_WITH_DISSENT for small, reversible orders | CALL_STANDS: the call stands and the dissent is recorded |
| ESCALATE for colliding limits or big orders | ESCALATE to the league, panel or governing body named in the case |
| The LLM narrates, the code decides | The model argues and judges the merits; the code enforces stakes, citations and the stopping rules |

The last row is the one real departure. In the coffee council, preferences are numbers, so code makes every
decision. Whether a trip was "an attempt to play the ball" can only be judged by reading the rules, so here the
model votes on the merits, and the code keeps it honest:

* **Stakes decide what an advocate may concede.** Each round, an advocate's bar drops by its concession rate
  until it reaches its floor, as in Lungo. An APPROVE for a ruling its side can't accept yet is blocked and
  counted as REJECT, so an advocate can't fold in round 1, and a side whose floor rules a ruling out never
  approves it.
* **The rules decide what it does concede.** The vote is a merits vote: the model is asked to judge each ruling
  as a neutral official would and isn't told which rulings its side can accept. The code applies the stakes
  afterwards, so a blocked approval in the transcript means "persuaded, but its side can't concede yet".
* **No ruling without an argument.** Only rulings some advocate argued for this round, plus the call, are put to
  a vote, as in Lungo, where agents vote only on proposals. A compromise nobody makes the case for can't be
  adopted.
* **A veto must cite a listed rule and an established fact (F#).** A veto that rests on a disputed point (D#),
  or on a rule that isn't in the case, is struck and counted as REJECT.
* **Citations are checked.** Any rule or fact ID in an argument that isn't in the case is struck from the record.

## Each round

1. **Argue.** Every advocate argues for one ruling it can accept, citing rule and fact IDs. It sees last round's
   arguments and the objections to its ruling.
2. **Vote.** Every advocate votes on every ruling on the table (each ruling argued for this round, plus the
   call): `APPROVE`, `REJECT` with a reason, or `VETO` with a rule and an established fact. It automatically
   approves the ruling it argued for. To concede, an advocate approves another ruling or switches its argument.
3. **Decide**, from the public record, by the same rule for everyone:

| Situation | Outcome |
|---|---|
| Every advocate approves the same ruling | **CONSENSUS**: the call is upheld or overturned |
| Everyone is at their floor and no vote changed since last round, or the last round ends, and the officials' advocate still backs the call | **CALL_STANDS**, with each dissent on record. This is the real-world standard: VAR's "clear and obvious error" and replay's "clear and conclusive evidence" leave the call alone unless it is shown to be wrong. |
| The same, but a grounded veto (rule + established fact) against the call is still standing and the case is `high_stakes` (it decides a game, a title or a medal) | **ESCALATE**: overruling that objection is not the council's call |
| The officials' advocate has given up the call, and no other ruling has everyone's approval | **ESCALATE**, with each ruling, who backs it and who objects |

## Cases

| Case | Sport | Level | Advocates | A rules expert would expect |
|---|---|---|---|---|
| `point_two` | Basketball | Easy | Officials, Lynx, Monarchs | CONSENSUS: uphold (no catch-and-shoot with 0.2 s, and the clock hit 0.0 before the release) |
| `dogso_card` | Soccer | In between | Referee & VAR, Riverside, Harbor City | CONSENSUS: overturn to penalty + yellow (a genuine attempt to play the ball in the area is a caution, not a red) |
| `deflection_offside` | Soccer | Impossible | Referee & VAR, Northgate, Eastport | CALL_STANDS: whether the defender deliberately played the ball is a judgment call, and neither side will concede |
| `moved_ball` | Golf | Impossible | Rules Committee + 3 players | ESCALATE: the title turns on a 95% standard and slow-motion video. The tempting compromise, a one-stroke penalty, isn't a ruling the Rules allow on these facts. |

The teams and players are fictional. To add a case, copy one in `cases.py`: describe the incident, list the
rulings, facts and disputed points, pick the rule IDs it needs, and give each party its `likes`, `concession` and
`floor`. The officials' advocate is built from `officials` and `call`.

## Rulebooks

The advocates may cite only the clauses a case lists from its rulebook in `rules/`. Each clause is a section:

```markdown
## LOTG-11.2 Offside offence
Source: Law 11 (Offside), section 2
A player in an offside position at the moment the ball is played ...
```

The three rulebooks here are **paraphrased from memory for the demo cases, not official text**. Before relying
on a verdict, replace each excerpt with the official wording (IFAB Laws of the Game, the NBA Official Rules, the
R&A/USGA Rules of Golf), keeping the `## ID Title` headings.

You can paste in whole rulebooks: only the clauses a case lists are sent to the model. That matters because
the default model has an 8k-token context, and a full rulebook runs to tens of thousands of tokens. Most cases
need 3 to 9 clauses. Rule IDs start with a prefix for the rulebook (`LOTG-`, `NBA-`, `GOLF-`), which is how the
council tells a citation from ordinary text.

## What the first run taught

The first version put every ruling to a vote every round and told each advocate which rulings its side could
accept. With `llama3.1:8b`, 3 of 4 verdicts matched a rules expert, but the stakes, not the rules, were deciding:

* In `moved_ball`, nobody ever argued for the one-stroke penalty, yet all four advocates approved it in round 6,
  each in the first round its side's stake allowed. The council adopted a ruling the Rules don't allow on those
  facts: the split-the-difference failure the case was built to catch.
* In `dogso_card`, both teams *vetoed* penalty + yellow in round 2 and approved it in round 3, again exactly when
  their stakes first allowed it. The verdict was right for the wrong reason.

The model read "your side could accept this" as "approve this". Two changes fixed it: the vote no longer mentions
the stakes, and only rulings someone argues for go to a vote. `tests/test_council.py` checks both.

## Limitations

* The verdict is only as good as the model's reading of the rules. `llama3.1:8b` follows the protocol but often
  misreads a rule, for example treating a slide that missed the ball as "no attempt to play the ball". Runs use a
  fixed seed and low temperature, but unlike the coffee council they aren't guaranteed to repeat byte for byte.
* The code checks that a veto cites a real rule and an established fact, not that the rule actually applies to
  that fact.
* The case author writes the facts. Splitting "established" from "disputed" is the most important judgment in a
  case: it decides what can ground a veto.
