# Sports Rules Council: advocates who don't agree about a call

A council that reviews a contested officiating decision, built on the Lungo Council's rules
(`aiagentdisagreethingy`). One advocate defends the call the officials made. Every party the call affects gets
its own advocate, however many there are: two teams, or three golfers whose finishing places depend on one
ruling. Nobody is in charge. The advocates argue from a public rulebook and a list of facts everyone accepts,
vote on the rulings argued for, and the outcome follows from the votes.

It can also build a case itself: give it a video and what you know, and the agents work out the incident, who
the parties are, which rules apply and where each side stands, then debate it.

```
python3 council.py                    # every case (needs Ollama running, or LLM_MODEL set)
python3 council.py dogso_card         # one case
python3 council.py all --rounds 4     # fewer rounds
python3 review.py clip.mp4 --at 0:42 --context "..."   # build a case from a video, then debate it
python3 council.py --case-file cases/<name>/case.json  # debate a saved case again
python3 -m unittest discover -s tests -t .   # tests, with a scripted model (no LLM needed)
```

Standard-library Python 3.12+. Reviewing video also needs ffmpeg, and PDF rulebooks need `pip install pypdf`.
The advocates use `LLM_MODEL` (default `ollama_chat/llama3.1:8b`) and the video is watched by `VISION_MODEL`
(default `ollama_chat/qwen2.5vl:7b`; `ollama pull qwen2.5vl:7b`). If litellm is installed it is used, so any
litellm model works (for example `anthropic/claude-sonnet-5-5`); otherwise everything talks to Ollama directly.
Transcripts of the demo cases are saved to `transcripts/`.

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

## Reviewing a video

```
python3 review.py clip.mp4 --at 0:42 --context "League match, Northgate (red) v Eastport (blue), 88th minute, 1-1.
    The referee allowed Northgate's goal, ruling that the defender's touch was a deliberate play."
```

1. **Watch.** ffmpeg pulls 12 frames (`--frames`) from the clip: packed into the 3 seconds either side of `--at`
   (`--window`), or spread over the whole clip. The vision model describes each one: who is involved, the ball,
   officials' signals, and on-screen text such as the score or a VAR caption.
2. **Understand.** The text model works out the sport, the incident, the call and who made it, and every party
   the call affects, with what each one wants.
3. **Find the rules.** It picks the sport's rulebooks in `rules/`, shortlists clauses by keyword search, and
   chooses the ones that could decide the case.
4. **Rulings and facts.** It lists the rulings the rules allow, starting with the call, and splits what's known
   into established facts (each tagged `[your context]` or `[video 0:41.5]`) and disputed points.
5. **Positions.** Each party's advocate ranks the rulings for its side and says how much is at stake; the code
   turns that into `likes`, `concession` and `floor`. The officials' advocate defends the call.

Then it stops and shows you the case, saved to `cases/<name>/case.json`, with the frames it looked at in
`cases/<name>/frames/`. Established facts can ground a veto, so check them. Fix anything in `case.json` and press
Enter, and the council debates the corrected case; the transcript goes in the same folder. Lines marked `CHECK:`
are answers the builder doubts, such as every party wanting the same ruling. `--yes` skips the pause. With no
video, `--context` alone builds the case from your description.

* **The vision model describes; it doesn't measure.** A 7B model can say a defender in blue stretched for the
  ball, not whether an attacker was 10 cm offside. Facts like that should come from your context; otherwise they
  land as disputed points, which is where they belong.
* **Your context does most of the work.** Name the teams or players, the call, and anything already settled.
* **It stops instead of guessing** when it can't tell what the call was, can't find two parties, or has no
  rulebook for the sport.

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

You can also drop in a rulebook as a PDF, such as `rules/ifab-laws.pdf` (needs `pip install pypdf`). It's split into
clauses of about 180 words with page-based IDs: `IFAB-p87.2` is the second clause on page 87. Rule-numbered IDs
read better, so a markdown rulebook with the same name takes precedence. PDFs and their cached split
(`rules/.cache/`) are kept out of git, because official rulebooks are copyrighted.

## Results with `llama3.1:8b`

| Case | Result | A rules expert would expect |
|---|---|---|
| `point_two` | CONSENSUS after 3 rounds: call upheld | same. The Lynx were persuaded in round 2, but their stake let them concede only in round 3. |
| `dogso_card` | CONSENSUS after 3 rounds: penalty + yellow | same. The Referee's advocate gave up the red card once someone argued for the yellow. |
| `deflection_offside` | CALL_STANDS after 5 rounds | same |
| `moved_ball` | CALL_STANDS after 6 rounds | ESCALATE. Hale's best point (the naked-eye rule and F6) came as a REJECT, never a veto citing a rule and a fact, so nothing grounded the escalation. Three advocates approved the one-stroke compromise; Hale's refusal kept it from being adopted. |

Transcripts are in `transcripts/`. Every advocate is the same small model, so read the arguments for their
structure more than their legal reasoning.

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
* A case built from video is only as good as the models that built it. In testing with `llama3.1:8b`, the
  builder got the parties' positions right once it was asked what each party wants, but it still sometimes
  described a ruling's consequences wrongly (a disallowed goal at 1-1 written up as a win for the other side) or
  listed two rulings that mean the same thing. That's what the pause before the debate is for. It has only been
  tested on a synthetic clip so far, not on real match footage.
