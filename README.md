# Sports Rules Council: advocates discuss a contested call, a neutral panel decides it

A council of AI agents that reviews a contested officiating decision. One advocate defends the call the officials
made. Every party the call affects gets its own advocate, however many there are: two teams, or three golfers whose
finishing places depend on one ruling. The advocates discuss the call with each other, citing a rulebook and a list
of facts everyone accepts. Then a neutral panel, which has no side and watches the clip for itself, decides it.
There's no answer key: what counts as the right call is often subjective, so the decision is whatever the evidence
and the discussion persuade the panel of, with the transcript to show why.

It can also build a case itself: give it a video and what you know, and the agents work out the incident, who the
parties are, which rules apply and where each side starts, then discuss it.

```
make                                   # build everything into build/
build/council                          # every demo case (needs Ollama running, or LLM_MODEL set)
build/council dogso_card               # one case
build/council all --rounds 3           # a shorter discussion
build/council all --panel 0            # no panel: the advocates decide among themselves
build/web                              # the website: open http://localhost:8000
build/review clip.mp4 --at 0:42 --context "..."   # build a case from a video, then discuss it
build/council --case-file cases/<name>/case.json  # discuss a saved case again
make test                              # tests, with a scripted model (no LLM needed)
```

C++20 and libcurl, nothing else to install: on macOS the Xcode command line tools have both (on Linux, a C++20
compiler and `libcurl4-openssl-dev` or your distribution's equivalent). Reviewing video also needs ffmpeg, and PDF
rulebooks need pdftotext (`brew install poppler`). The advocates use `LLM_MODEL` (default `ollama_chat/llama3.1:8b`).
The panel, and the step that watches the video, use `VISION_MODEL` (default `ollama_chat/qwen2.5vl:7b`; `ollama pull
qwen2.5vl:7b`), so the panel is a different model from the advocates and can see images. Ollama models are called
through Ollama's own API; Claude models work too, as `anthropic/<model>` (for example `anthropic/claude-sonnet-5-5`),
with your key in `ANTHROPIC_API_KEY`. Transcripts of the demo cases are saved to `transcripts/`.

## How a review works

1. **The panel watches the clip first.** When the case comes from a video, each panel member looks at up to six
   frames from it and writes down what it sees that matters, before hearing a word from the advocates and without
   deciding anything. Its view of the evidence comes from the clip, not from either side.
2. **The advocates start from their sides.** The officials' advocate starts out backing the call; each party's
   advocate starts on the ruling its side wants most (`likes` in the case).
3. **They talk, in turn.** Each round, every advocate speaks once, and a different one opens each round. A speaker
   sees everything said so far, including earlier speakers in the same round. It answers the others by name,
   accepts the points it thinks are right, pushes back on the rest, and ends by saying which ruling it backs now.
   The advocates are told to change position only when an argument actually convinces them, not to be agreeable.
   The discussion ends when they all back the same ruling, when the rounds run out (4 by default), or when a round
   after the second passes with nobody changing position.
4. **Closing statements.** Each advocate tells the panel, in a sentence or two, why it should rule its way.
5. **The panel decides.** Each member looks at the frames again, rereads its own notes from step 1, weighs the
   discussion and the closing statements, and votes on its own, with a reason.

| The panel's vote | Decision |
|---|---|
| Unanimous | **CONSENSUS** |
| A majority | **VOTE**: the majority's ruling, with each dissent recorded |
| Tied | **TIE**: the tied ruling more advocates ended up backing; failing that, the call stands |

The three panel members (`Panelist A`, `B`, `C`) have no side, but each weighs things a little differently so they
don't all think alike: A pays most attention to what the clip and the established facts show, B to the exact wording
of the rules, C to fairness and the purpose behind them. Cases without a clip (like the four demo cases) still get
the panel, judging from the case and the discussion.

With `--panel 0` there is no panel and the advocates decide among themselves: a consensus during the discussion, or
else a closing vote among them, with ties going the officials' way.

The code keeps the discussion honest without steering it. Advocates may cite only the rules and facts in the case,
and any citation of something else is struck from the record and marked in the transcript. The code doesn't decide
when anyone may change their mind, and it doesn't compare the decision with an expected answer.

This grew out of the Lungo Council (`aiagentdisagreethingy`), where agents with numeric preferences negotiate a
coffee order with no supervisor. The first versions here kept that machinery: private "bars" that dropped each round,
vetoes that had to cite a rule and a fact, and an expected outcome for each case. In practice the stakes, not the
arguments, decided the verdicts: an advocate approved whatever its side's numbers allowed in that round. So the
numbers now only set where each advocate starts, and persuasion does the rest. The panel came next: with one
advocate for the officials and one per party, the call's side starts with a majority in a two-party case, so when
the advocates decided by themselves and nobody was persuaded, the call always stood.

## Cases

| Case | Sport | Advocates | What's in dispute |
|---|---|---|---|
| `point_two` | Basketball | Officials, Lynx, Monarchs | A catch-and-shoot buzzer-beater with 0.2 seconds left: does it count? |
| `dogso_card` | Soccer | Referee & VAR, Riverside, Harbor City | A trip on the penalty-area line: penalty or free kick, and red card or yellow? |
| `deflection_offside` | Soccer | Referee & VAR, Northgate, Eastport | A late goal after a defender's touch: deliberate play (goal) or deflection (offside)? |
| `moved_ball` | Golf | Rules Committee + 3 players | A ball moved at address on the 72nd hole: two strokes, one, or none, with the title riding on it |

The teams and players are fictional. To add a case, copy one in `src/cases.cpp` and run `make`: describe the incident, list the
rulings, facts and disputed points, pick the rule IDs it needs, and give each party its `likes`. The officials'
advocate is built from `officials` and `call`.

## Reviewing a video

```
build/review clip.mp4 --at 0:42 --context "League match, Northgate (red) v Eastport (blue), 88th minute, 1-1.
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
5. **Starting positions.** Each party's advocate ranks the rulings for its side. The officials' advocate starts on
   the call.

Then it stops and shows you the case, saved to `cases/<name>/case.json`, with the frames it looked at in
`cases/<name>/frames/` (listed under `frames` in the case). Every advocate treats the established facts as true, so
check them. Fix anything in `case.json` and press Enter: the advocates discuss the corrected case, and the neutral
panel, looking at those frames itself, decides it. The transcript goes in the same folder.
Lines marked `CHECK:` are answers the builder doubts, such as every party wanting the same ruling. `--yes` skips the
pause. With no video, `--context` alone builds the case from your description.

* **The vision model describes; it doesn't measure.** On real F1 footage, qwen2.5vl read sponsor logos on a car
  close to the camera, but at grandstand distance it invented car numbers and described cars in an empty frame.
  The panel is the same model looking at stills, not the moving clip, so the same limits apply to what it sees.
  Facts that decide a case, like who touched whom or who was ahead at the apex, should come from your context.
* **Your context does most of the work.** Name the teams or players, the call, and anything already settled.
* **It stops instead of guessing** when it can't tell what the call was, can't find two parties, or has no
  rulebook for the sport.

## The website

```
build/web        # then open http://localhost:8000
```

The same review in a browser. It runs on this computer and only listens on localhost.

1. **The clip.** Paste a link to it, or upload one. A preview plays it, and "Use player time" fills in when the
   incident starts and ends.
2. **What happened.** Describe the call, then press **Get a ruling**.
3. **Progress.** Watch it get the clip, read the frames and build the case, then follow the discussion as it happens.
4. **Your check.** Edit the facts every advocate will treat as true, the rulings, the call and what each party
   wants most (or all of it as JSON), then start the discussion. Untick "Let me check the case" to skip this.
5. **The ruling.** Upheld or overturned; each panel member's vote, its reason and what it saw in the clip before the
   discussion; the frames the panel looked at; the advocates' closing statements; and the full transcript.

A direct link to a video file (`.mp4`, `.webm`, `.mov` and so on) is downloaded as it is. A link to a page, such as
YouTube, needs yt-dlp (`brew install yt-dlp`); with a start time set, it downloads just the part around the
incident. Downloading from YouTube is against its terms and match footage is usually copyrighted, so whether to do
that is up to you; uploading a clip you have always works. One review runs at a time, and others wait their turn.
Clips and frames stay on this computer, in `cases/` and `uploads/`, and out of git.

## Rulebooks

The advocates may cite only the clauses a case lists from its rulebook in `rules/`. Each clause is a section:

```markdown
## LOTG-11.2 Offside offence
Source: Law 11 (Offside), section 2
A player in an offside position at the moment the ball is played ...
```

The soccer, basketball and golf rulebooks are **paraphrased from memory for the demo cases, not official text**.
Before relying on a decision, replace each excerpt with the official wording (IFAB Laws of the Game, the NBA Official
Rules, the R&A/USGA Rules of Golf), keeping the `## ID Title` headings.

The Formula 1 rulebook (`rules/formula1.md`) was written from the official FIA PDFs: the 2026 F1 Sporting
Regulations (Section B, Issue 09, 1 October 2026) and Chapter IV of Appendix L to the International Sporting Code
(2026, published 23 June 2026). It keeps their article numbers and quotes the key sentences, such as the stewards'
test that no penalty is imposed unless a driver was "wholly or predominantly to blame". It covers driving conduct,
overtaking and defending, track limits, flags, the pit lane, the VSC and Safety Car, red flags, and penalties. The
FIA reissues the Sporting Regulations during the season, so check for a newer issue.

You can paste in whole rulebooks: only the clauses a case lists are sent to the model. That matters because the
default model has an 8k-token context, and a full rulebook runs to tens of thousands of tokens. Most cases need 3 to
9 clauses. Rule IDs start with a prefix for the rulebook (`LOTG-`, `NBA-`, `GOLF-`, `FIA-`), which is how the council
tells a citation from ordinary text. A prefix can't look like a fact ID: Formula 1 uses `FIA-` because `F1-` would
read as "fact F1".

You can also drop in a rulebook as a PDF, such as `rules/ifab-laws.pdf` (needs pdftotext: `brew install poppler`). It's split into
clauses of about 180 words with page-based IDs: `IFAB-p87.2` is the second clause on page 87. Rule-numbered IDs read
better, so a markdown rulebook with the same name takes precedence. PDFs and their cached split (`rules/.cache/`) are
kept out of git, because official rulebooks are copyrighted.

## Results

Advocates on `llama3.1:8b`, panel on `qwen2.5vl:7b`.

| Case | Decision | The advocates ended on |
|---|---|---|
| `point_two` | Panel unanimous: no basket (the call is upheld) | 2-1 for no basket |
| `dogso_card` | Panel unanimous: penalty and red card (upheld) | 2-1 for the red card |
| `deflection_offside` | Panel unanimous: the goal stands (upheld) | 2-1 for the goal |
| `moved_ball` | Panel unanimous: two-stroke penalty (upheld) | three of four on a one-stroke compromise |
| Pit-stop clip, command line | Panel 3-0: the penalty is overturned | 2-1 to keep the penalty |
| Pit-stop clip, website | Panel 2-1: the penalty is overturned | 2-1 to keep the penalty |

The last two rows are a made-up "unsafe release" call laid over real footage: Red Bull's 2015 pit-stop practice
([CC BY-SA, Wikimedia Commons](https://commons.wikimedia.org/wiki/File:Red_Bull_Racing_Pit_Stop_Practice.webm)).
The description said another car had to brake to avoid the released car, and the advocates argued from that. The
panel watched the clip first, saw one stationary car being serviced and no second car anywhere, and overturned the
penalty: "The video does not show the car being released into the pit lane or any immediate risk of contact with
another car."

In the four demo cases, which have no clip, the panel backed the officials' call every time, even when the
advocates had drifted elsewhere: in `moved_ball`, three of the four advocates moved to a one-stroke compromise during
the discussion, and the panel still chose two strokes.

Getting the advocates to actually argue took a few tries. At first they gave up their own side in their opening
statements (the team that scored argued its goal should be disallowed), agreed with themselves in the third person,
and swung back and forth without saying why. Now an opening statement argues for the advocate's side, a speaker's own
earlier turns are shown to it as "You", and changing position means saying which argument did it.

Transcripts of the demo cases are in `transcripts/`.

## Limitations

* Every advocate is the same model playing a different side, so the discussion is only as sharp as that model.
  `llama3.1:8b` follows the format but often misreads a rule, and small models tend either to dig in or to agree
  too easily. A stronger model, such as a Claude model through `anthropic/`, should argue better. Runs use a fixed seed and low temperature, but
  they aren't guaranteed to repeat word for word.
* The panel is a 7B vision model too. Its reasons are sometimes muddled: in one run a member voted to keep a penalty
  while writing that the clip gave no evidence for it. It looks at up to six stills, not the moving clip, and its
  members vote without discussing among themselves.
* The code checks that a citation names a real rule or fact in the case, not that it supports the point being made.
* The case decides what everyone treats as true. Splitting "established" from "disputed" is the most important
  judgment in a case.
* A case built from video is only as good as the models that built it. In testing, the builder sometimes described
  a ruling's consequences wrongly, listed two rulings that mean the same thing, or labelled a detail from your
  description as something the video showed. That's what the pause before the discussion is for.
