// The rulebook library: every rulebook in rules/, as markdown or PDF, and a keyword search over its clauses.
//
// Markdown rulebooks use '## ID Title' headings, so clauses keep the rulebook's own numbering (LOTG-11.2).
// A PDF is split into short clauses with page-based IDs (IFAB-p87.2 is the second clause on page 87), because
// every rulebook lays out its numbering differently. Reading PDFs needs pdftotext (brew install poppler); the split
// is cached in rules/.cache/.

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "util.hpp"

namespace library {

constexpr size_t CLAUSE_WORDS = 180;  // roughly how long a PDF clause is

struct Rule {
    std::string id, title, text, source;
    bool operator==(const Rule&) const = default;
};

const fs::path& rules_dir();

// Every rulebook by name (the file name without its extension). A markdown file wins over a PDF of the same name,
// since it's the reviewed version.
Dict<fs::path> rulebooks(const fs::path& dir = rules_dir());
// A markdown rulebook's '# ' heading, or a PDF's file name.
std::string title_of(const fs::path& path);
// Raises OSError if there's no such rulebook.
Dict<Rule> load_rulebook(const std::string& name, const fs::path& dir = rules_dir());
// Each clause is a '## ID Title' heading, an optional 'Source:' line and its text.
Dict<Rule> read_markdown(const fs::path& path);
std::string pdf_prefix(const fs::path& path);  // 'ifab-laws-2025.pdf' -> 'IFAB'
Dict<Rule> read_pdf(const fs::path& path);
// Groups a page's lines into clauses of about CLAUSE_WORDS words, starting a new one at a heading-like line
// (Law 11, Rule 9.2b, 2. Offside offence) once the current one is half full.
std::vector<std::vector<std::string>> split_clauses(const std::vector<std::string>& lines);
// Words cut to their first five letters, a crude stem: collided and collision both become 'colli'.
std::vector<std::string> tokens(std::string_view text);
// The k clauses that best match the query (BM25).
std::vector<Rule> search(const Dict<Rule>& rules, const std::string& query, size_t k = 10);

}  // namespace library
