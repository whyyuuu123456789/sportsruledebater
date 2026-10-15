#include "library.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <regex>
#include <unordered_map>
#include <unordered_set>

#include "json.hpp"

namespace library {

namespace {

const std::unordered_set<std::string> STOPWORDS = {
    "a",    "an",    "and",   "are",  "as",    "at",     "be",   "by",   "for",   "from",   "has",    "have",
    "if",   "in",    "into",  "is",   "it",    "its",    "may",  "must", "no",    "not",    "of",     "on",
    "or",   "that",  "the",   "their", "them", "then",   "there", "these", "this", "to",    "was",    "were",
    "when", "where", "which", "who",  "will",  "with",   "within", "without"};

const std::regex HEADING(R"(^(?:(?:law|rule|section|article|item)\b|\d+(?:\.\d+)*[a-z]?[.)]?\s+[A-Z]))",
                         std::regex::ECMAScript | std::regex::icase);

// The modification time as Python's os.stat reports it, so a cache written by either version still matches.
double mtime(const fs::path& path) {
    struct stat st {};
    if (stat(path.c_str(), &st) != 0) throw OSError("can't read " + path.string());
#ifdef __APPLE__
    return static_cast<double>(st.st_mtimespec.tv_sec) + static_cast<double>(st.st_mtimespec.tv_nsec) * 1e-9;
#else
    return static_cast<double>(st.st_mtim.tv_sec) + static_cast<double>(st.st_mtim.tv_nsec) * 1e-9;
#endif
}

json::Value rule_json(const Rule& r) {
    return json::Object{{"id", r.id}, {"title", r.title}, {"text", r.text}, {"source", r.source}};
}

}  // namespace

const fs::path& rules_dir() {
    static const fs::path dir = project_root() / "rules";
    return dir;
}

Dict<fs::path> rulebooks(const fs::path& dir) {
    Dict<fs::path> found;
    for (const auto& p : glob(dir, "", ".pdf")) found[p.stem().string()] = p;
    for (const auto& p : glob(dir, "", ".md")) found[p.stem().string()] = p;
    return found;
}

std::string title_of(const fs::path& path) {
    if (path.extension() == ".md") {
        for (const auto& line : py::splitlines(read_file(path)))
            if (line.rfind("# ", 0) == 0) return py::strip(line.substr(2));
    }
    return py::replace(py::replace(path.stem().string(), "-", " "), "_", " ");
}

Dict<Rule> load_rulebook(const std::string& name, const fs::path& dir) {
    auto books = rulebooks(dir);
    if (!books.contains(name))
        throw OSError("No rulebook called " + py::repr(name) + " in " + dir.string() + " (looked for " + name + ".md and " +
                      name + ".pdf)");
    const fs::path& found = books.at(name);
    return found.extension() == ".md" ? read_markdown(found) : read_pdf(found);
}

Dict<Rule> read_markdown(const fs::path& path) {
    Dict<Rule> rules;
    std::string current;
    bool have = false;
    for (const auto& line : py::splitlines(read_file(path))) {
        if (line.rfind("## ", 0) == 0) {
            std::string rest = py::strip(line.substr(3));
            size_t space = rest.find(' ');
            std::string rid = rest.substr(0, space);
            rules[rid] = Rule{rid, space == std::string::npos ? "" : py::strip(rest.substr(space + 1)), "", ""};
            current = rid;
            have = true;
        } else if (have && line.rfind("Source:", 0) == 0) {
            rules[current].source = py::strip(py::removeprefix(line, "Source:"));
        } else if (have) {
            Rule& rule = rules[current];
            rule.text = py::strip(rule.text + "\n" + line);
        }
    }
    return rules;
}

std::string pdf_prefix(const fs::path& path) {
    std::string stem = path.stem().string();
    std::string first = stem.substr(0, stem.find_first_of("-_ "));
    std::string prefix;
    for (char c : py::upper(first))
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) prefix += c;
    prefix = prefix.substr(0, 8);
    return prefix.empty() ? "DOC" : prefix;
}

Dict<Rule> read_pdf(const fs::path& path) {
    fs::path cache = path.parent_path() / ".cache" / (path.stem().string() + ".json");
    double stamp = mtime(path);
    std::error_code ec;
    if (fs::exists(cache, ec)) {
        try {
            json::Value saved = json::Value::parse(read_file(cache));
            if (saved.get("mtime") == json::Value(stamp)) {
                Dict<Rule> rules;
                for (const auto& r : json::iterate(saved.get("rules")))
                    rules[r.get("id").as_string()] = Rule{r.get("id").as_string(), r.get("title").as_string(),
                                                          r.get("text").as_string(), r.get("source").as_string()};
                return rules;
            }
        } catch (const ValueError&) {  // a damaged cache is read again from the PDF
        }
    }
    std::string name = path.filename().string();
    if (which("pdftotext").empty()) throw RuntimeError("Reading " + name + " needs pdftotext: brew install poppler");
    Completed done = run({"pdftotext", "-enc", "UTF-8", path.string(), "-"});
    if (done.code != 0) {
        auto lines = py::splitlines(py::strip(done.err));
        throw RuntimeError("pdftotext couldn't read " + name + (lines.empty() ? "" : ": " + lines.back()));
    }
    auto pages = py::split(done.out, "\f");  // pdftotext ends every page with a form feed
    if (!pages.empty() && pages.back().empty()) pages.pop_back();
    std::string prefix = pdf_prefix(path);
    Dict<Rule> rules;
    for (size_t page_no = 1; page_no <= pages.size(); ++page_no) {
        std::vector<std::string> lines;
        for (const auto& line : py::splitlines(pages[page_no - 1]))
            if (std::string squashed = py::squash(line); !squashed.empty()) lines.push_back(squashed);
        auto chunks = split_clauses(lines);
        for (size_t k = 1; k <= chunks.size(); ++k) {
            std::string rid = prefix + "-p" + std::to_string(page_no) + "." + std::to_string(k);
            rules[rid] = Rule{rid, py::head(chunks[k - 1][0], 80), py::join(chunks[k - 1], "\n"),
                              name + ", page " + std::to_string(page_no)};
        }
    }
    fs::create_directory(cache.parent_path(), ec);
    json::Array saved_rules;
    for (const auto& [rid, rule] : rules) saved_rules.push_back(rule_json(rule));
    write_file(cache, json::Value(json::Object{{"mtime", stamp}, {"rules", saved_rules}}).dump());
    return rules;
}

std::vector<std::vector<std::string>> split_clauses(const std::vector<std::string>& lines) {
    std::vector<std::vector<std::string>> chunks;
    size_t words = 0;
    for (const auto& line : lines) {
        size_t n = py::split(line).size();
        if (!chunks.empty() &&
            (words + n > CLAUSE_WORDS || (std::regex_search(line, HEADING) && words > CLAUSE_WORDS / 2))) {
            chunks.emplace_back();
            words = 0;
        }
        if (chunks.empty()) chunks.emplace_back();
        chunks.back().push_back(line);
        words += n;
    }
    std::vector<std::vector<std::string>> kept;
    for (auto& chunk : chunks) {
        size_t total = 0;
        for (const auto& line : chunk) total += py::split(line).size();
        if (total >= 8) kept.push_back(std::move(chunk));
    }
    return kept;
}

std::vector<std::string> tokens(std::string_view text) {
    std::vector<std::string> out;
    std::string word;
    auto flush = [&] {
        if (word.size() > 1 && !STOPWORDS.count(word)) out.push_back(word.substr(0, 5));
        word.clear();
    };
    for (char c : py::lower(text)) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) word += c;
        else flush();
    }
    flush();
    return out;
}

std::vector<Rule> search(const Dict<Rule>& rules, const std::string& query, size_t k) {
    std::vector<std::string> ids;
    std::vector<std::vector<std::string>> docs;
    for (const auto& [rid, rule] : rules) {
        ids.push_back(rid);
        docs.push_back(tokens(rule.title + " " + rule.text));
    }
    if (docs.empty()) return {};
    double total = 0;
    for (const auto& d : docs) total += static_cast<double>(d.size());
    double avg = total / static_cast<double>(docs.size());
    if (avg == 0) avg = 1;
    std::unordered_map<std::string, int> df;
    for (const auto& d : docs)
        for (const auto& t : std::unordered_set<std::string>(d.begin(), d.end())) ++df[t];
    std::vector<std::string> query_terms;
    for (const auto& t : tokens(query))
        if (std::find(query_terms.begin(), query_terms.end(), t) == query_terms.end()) query_terms.push_back(t);

    double n = static_cast<double>(docs.size());
    std::vector<double> scores;
    for (const auto& d : docs) {
        std::unordered_map<std::string, int> tf;
        for (const auto& t : d) ++tf[t];
        double score = 0;
        for (const auto& t : query_terms) {
            auto it = tf.find(t);
            if (it == tf.end()) continue;
            double f = it->second, dft = df[t];
            score += std::log(1 + (n - dft + 0.5) / (dft + 0.5)) * f * 2.2 /
                     (f + 1.2 * (0.25 + 0.75 * static_cast<double>(d.size()) / avg));
        }
        scores.push_back(score);
    }
    std::vector<size_t> order(docs.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return scores[a] > scores[b]; });
    std::vector<Rule> best;
    for (size_t i = 0; i < std::min(k, order.size()); ++i)
        if (scores[order[i]] > 0) best.push_back(rules.at(ids[order[i]]));
    return best;
}

}  // namespace library
