"""The rulebook library: every rulebook in rules/, as markdown or PDF, and a keyword search over its clauses.

Markdown rulebooks use '## ID Title' headings, so clauses keep the rulebook's own numbering (LOTG-11.2).
A PDF is split into short clauses with page-based IDs (IFAB-p87.2 is the second clause on page 87), because
every rulebook lays out its numbering differently. Reading PDFs needs pypdf (pip install pypdf); the split is
cached in rules/.cache/.
"""

from __future__ import annotations

import json
import math
import re
from collections import Counter
from dataclasses import asdict, dataclass
from pathlib import Path

RULES_DIR = Path(__file__).resolve().parent / "rules"
CLAUSE_WORDS = 180  # roughly how long a PDF clause is
STOPWORDS = set("""a an and are as at be by for from has have if in into is it its may must no not of on or that the
their them then there these this to was were when where which who will with within without""".split())


@dataclass
class Rule:
    id: str
    title: str
    text: str = ""
    source: str = ""


def rulebooks(rules_dir: Path = RULES_DIR) -> dict[str, Path]:
    """Every rulebook by name (the file name without its extension). A markdown file wins over a PDF of the
    same name, since it's the reviewed version."""
    found = {p.stem: p for p in sorted(rules_dir.glob("*.pdf"))}
    found.update({p.stem: p for p in sorted(rules_dir.glob("*.md"))})
    return found


def title_of(path: Path) -> str:
    """A markdown rulebook's '# ' heading, or a PDF's file name."""
    if path.suffix == ".md":
        for line in path.read_text().splitlines():
            if line.startswith("# "):
                return line[2:].strip()
    return path.stem.replace("-", " ").replace("_", " ")


def load_rulebook(name: str, rules_dir: Path = RULES_DIR) -> dict[str, Rule]:
    path = rulebooks(rules_dir).get(name)
    if path is None:
        raise FileNotFoundError(f"No rulebook called {name!r} in {rules_dir} (looked for {name}.md and {name}.pdf)")
    return read_markdown(path) if path.suffix == ".md" else read_pdf(path)


def read_markdown(path: Path) -> dict[str, Rule]:
    """Each clause is a '## ID Title' heading, an optional 'Source:' line and its text."""
    rules: dict[str, Rule] = {}
    current = None
    for line in path.read_text().splitlines():
        if line.startswith("## "):
            rid, _, title = line[3:].strip().partition(" ")
            current = rules[rid] = Rule(rid, title.strip())
        elif current and line.startswith("Source:"):
            current.source = line.removeprefix("Source:").strip()
        elif current:
            current.text = (current.text + "\n" + line).strip()
    return rules


def pdf_prefix(path: Path) -> str:
    """'ifab-laws-2025.pdf' -> 'IFAB'."""
    return re.sub(r"[^A-Z0-9]", "", re.split(r"[-_ ]", path.stem)[0].upper())[:8] or "DOC"


def read_pdf(path: Path) -> dict[str, Rule]:
    cache = path.parent / ".cache" / f"{path.stem}.json"
    stamp = path.stat().st_mtime
    if cache.exists():
        saved = json.loads(cache.read_text())
        if saved.get("mtime") == stamp:
            return {r["id"]: Rule(**r) for r in saved["rules"]}
    try:
        from pypdf import PdfReader
    except ImportError as exc:
        raise RuntimeError(f"Reading {path.name} needs pypdf: pip install pypdf") from exc
    prefix, rules = pdf_prefix(path), {}
    for page_no, page in enumerate(PdfReader(path).pages, 1):
        lines = [" ".join(l.split()) for l in (page.extract_text() or "").splitlines()]
        for k, chunk in enumerate(split_clauses([l for l in lines if l]), 1):
            rid = f"{prefix}-p{page_no}.{k}"
            rules[rid] = Rule(rid, chunk[0][:80], "\n".join(chunk), f"{path.name}, page {page_no}")
    cache.parent.mkdir(exist_ok=True)
    cache.write_text(json.dumps({"mtime": stamp, "rules": [asdict(r) for r in rules.values()]}))
    return rules


HEADING = re.compile(r"^(?:(?:law|rule|section|article|item)\b|\d+(?:\.\d+)*[a-z]?[.)]?\s+[A-Z])", re.IGNORECASE)


def split_clauses(lines: list[str]) -> list[list[str]]:
    """Groups a page's lines into clauses of about CLAUSE_WORDS words, starting a new one at a heading-like line
    (Law 11, Rule 9.2b, 2. Offside offence) once the current one is half full."""
    chunks: list[list[str]] = []
    words = 0
    for line in lines:
        n = len(line.split())
        if chunks and (words + n > CLAUSE_WORDS or (HEADING.match(line) and words > CLAUSE_WORDS // 2)):
            chunks.append([])
            words = 0
        if not chunks:
            chunks.append([])
        chunks[-1].append(line)
        words += n
    return [c for c in chunks if sum(len(l.split()) for l in c) >= 8]


def tokens(text: str) -> list[str]:
    """Words cut to their first five letters, a crude stem: collided and collision both become 'colli'."""
    return [t[:5] for t in re.findall(r"[a-z0-9]+", text.lower()) if t not in STOPWORDS and len(t) > 1]


def search(rules: dict[str, Rule], query: str, k: int = 10) -> list[Rule]:
    """The k clauses that best match the query (BM25)."""
    docs = {rid: tokens(f"{r.title} {r.text}") for rid, r in rules.items()}
    if not docs:
        return []
    avg = sum(map(len, docs.values())) / len(docs) or 1
    df = Counter(t for toks in docs.values() for t in set(toks))
    query_terms = set(tokens(query))

    def score(toks: list[str]) -> float:
        tf = Counter(toks)
        return sum(math.log(1 + (len(docs) - df[t] + 0.5) / (df[t] + 0.5))
                   * tf[t] * 2.2 / (tf[t] + 1.2 * (0.25 + 0.75 * len(toks) / avg))
                   for t in query_terms if t in tf)

    ranked = sorted(docs, key=lambda rid: -score(docs[rid]))
    return [rules[rid] for rid in ranked[:k] if score(docs[rid]) > 0]
