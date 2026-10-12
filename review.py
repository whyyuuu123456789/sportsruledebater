"""Review a contested call from a video. The agents watch it, work out the incident and who the parties are, find
the rules in rules/, take their positions, and then the council debates it.

    python3 review.py clip.mp4 --context "Northgate v Eastport, 88th minute. The referee allowed the goal ..."
    python3 review.py clip.mp4 --at 0:42 --context "..."   # look closely around 0:42
    python3 review.py --context "..."                      # no video: build the case from your description
    python3 council.py --case-file cases/<name>/case.json  # debate a saved case again

Before the debate it stops so you can check the case it built: cases/<name>/case.json, with the frames the vision
model looked at in cases/<name>/frames/. Fix anything that's wrong in case.json, then press Enter. --yes skips
the pause.
"""

from __future__ import annotations

import argparse
import re
import sys
import time
from pathlib import Path

from cases import Case, load_case, save_case
from council import HERE, MAX_ROUNDS, SAVED_HEADER, debate
from evidence import seconds, watch
from intake import Intake, IntakeError
from library import load_rulebook
from llm import MODEL, VISION_MODEL, connect


def show(case: Case) -> None:
    rules = {}
    for rulebook in case.rulebooks:
        rules.update(load_rulebook(rulebook))
    print(f"\n{case.title} ({case.sport})\n{case.incident}")
    print(f"The call: {case.officials} ruled {case.call}: {case.rulings[case.call]}"
          + (f" ({case.call_reason})" if case.call_reason else ""))
    print("Possible rulings:")
    for rid, text in case.rulings.items():
        print(f"  {rid}: {text}")
    print("Parties:")
    for a in case.parties:
        ranking = sorted(a.likes, key=lambda r: -a.likes[r])
        print(f"  {a.name}: wants {ranking[0]}, then {', '.join(ranking[1:])}; never concedes "
              f"{', '.join(r for r in ranking if a.likes[r] < a.floor) or 'nothing'}")
    print("Established facts (these can ground a veto, so check them):")
    for fid, text in case.facts.items():
        print(f"  {fid}: {text}")
    print("Disputed points:")
    for did, text in case.disputed.items():
        print(f"  {did}: {text}")
    print(f"Rules ({', '.join(case.rulebooks)}):")
    for rid in case.rules:
        print(f"  {rid}: {rules[rid].title}")
    print(f"High stakes: {'yes' if case.high_stakes else 'no'}. Escalates to {case.escalate_to}.")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("video", nargs="?", type=Path, help="the clip to review")
    ap.add_argument("--context", default="", help="what you know: competition, teams, players, score, and the call")
    ap.add_argument("--at", help="when the incident happens in the clip, like 0:42")
    ap.add_argument("--window", type=float, default=3.0, help="seconds either side of --at to look at (default 3)")
    ap.add_argument("--frames", type=int, default=12, help="how many frames the vision model looks at (default 12)")
    ap.add_argument("--name", help="folder for the case under cases/ (default: the clip's name)")
    ap.add_argument("--rounds", type=int, default=MAX_ROUNDS, help=f"most debate rounds (default {MAX_ROUNDS})")
    ap.add_argument("--yes", action="store_true", help="start the debate without pausing to check the case")
    args = ap.parse_args(argv)
    if not args.video and not args.context:
        ap.error("give a video, --context, or both")
    if args.video and not args.video.exists():
        ap.error(f"{args.video} doesn't exist")

    name = re.sub(r"[^A-Za-z0-9_-]+", "_", args.name or (args.video.stem if args.video else
                                                           time.strftime("case_%Y%m%d_%H%M%S"))).strip("_")
    folder = HERE / "cases" / name
    chat = connect(MODEL)
    evidence = []
    if args.video:
        print(f"Watching {args.video.name} with {VISION_MODEL}")
        evidence = watch(args.video, args.context, connect(VISION_MODEL), folder / "frames", args.frames,
                         seconds(args.at) if args.at else None, args.window)
    print(f"Building the case with {MODEL}")
    intake = Intake(chat, args.context, evidence)
    try:
        case = intake.build("from video" if args.video else "from description")
    except IntakeError as exc:
        print(f"Couldn't build the case: {exc}", file=sys.stderr)
        return 1
    path = folder / "case.json"
    save_case(case, path, context=args.context, video=str(args.video or ""), evidence=list(map(str, evidence)),
              warnings=intake.warnings)
    show(case)
    for warning in intake.warnings:
        print(f"CHECK: {warning}")
    print(f"\nSaved to {path.relative_to(HERE)}")
    if not args.yes:
        try:
            input("Check it, edit case.json if anything is wrong, then press Enter to start the debate "
                  "(Ctrl-C to stop): ")
        except (KeyboardInterrupt, EOFError):
            print(f"\nStopped. Debate it later with: python3 council.py --case-file {path.relative_to(HERE)}")
            return 0
        case = load_case(path)
    print()
    debate(name, case, chat, args.rounds, folder / "transcript.md", SAVED_HEADER)
    return 0


if __name__ == "__main__":
    sys.exit(main())
