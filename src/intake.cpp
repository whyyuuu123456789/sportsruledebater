#include "intake.hpp"

#include <algorithm>
#include <set>

namespace intake {

const std::string SYSTEM =
    "You turn a contested sports officiating decision into a case file for a rules review. Use only what the user says "
    "and what the video observations show. Never invent names, scores, times or events; if something isn't known, "
    "leave it empty. Answer in JSON.";

namespace {

const std::string EXAMPLE =
    R"(EXAMPLE from a different case, to show the shape only: {"sport": "basketball", "title": "Catch-and-shoot )"
    R"(with 0.2 seconds left", "officials": "Game officials", "call": "No basket", "parties": [{"name": "Lakeside )"
    R"(Lynx", "wants": "the basket to count", "stake": "they win 101-100 if it counts"}, {"name": "Metro Monarchs", )"
    R"("wants": "the basket waved off", "stake": "they win 100-99 if it is waved off"}]})";

// `a or b`, for values from the model's JSON.
const json::Value& either(const json::Value& a, const json::Value& b) { return json::truthy(a) ? a : b; }

bool in(const std::vector<std::string>& list, const std::string& item) {
    return std::find(list.begin(), list.end(), item) != list.end();
}

}  // namespace

std::string slug(const std::string& text) {
    std::string out;
    bool gap = false;
    for (char c : py::lower(text)) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            if (gap) out += '_';
            out += c;
            gap = false;
        } else {
            gap = true;
        }
    }
    if (gap) out += '_';
    return py::strip(py::strip(out, "_").substr(0, 30), "_");
}

std::string slug(const json::Value& value) { return slug(json::str(value)); }

std::string text(const json::Value& value) {
    if (value.is_array()) {
        std::vector<std::string> parts;
        for (const auto& item : value.as_array()) parts.push_back(json::str(item));
        return py::strip(py::join(parts, " "));
    }
    return json::truthy(value) ? py::strip(json::str(value)) : "";
}

std::string snippet(const std::string& text, size_t words) {
    auto w = py::split(text);
    std::vector<std::string> kept(w.begin(), w.begin() + static_cast<long>(std::min(words, w.size())));
    return py::join(kept, " ") + (w.size() > words ? " ..." : "");
}

Intake::Intake(llm::Chat chat_, const std::string& context_, std::vector<evidence::Observation> evidence_, Log log_)
    : chat(std::move(chat_)), context(py::strip(context_)), evidence(std::move(evidence_)), log(log_ ? std::move(log_) : print_line) {}

json::Value Intake::ask(const std::string& user, int max_tokens, const std::string& system) {
    ++calls;
    std::string reply = chat(system, user, max_tokens, 100 + calls, {}, true);
    size_t open = reply.find('{'), close = reply.rfind('}');  // the outermost {...}, as re.search(r"\{.*\}") finds it
    std::string candidate = open != std::string::npos && close != std::string::npos && close > open
                                ? reply.substr(open, close - open + 1)
                                : reply;
    try {
        json::Value data = json::Value::parse(candidate);
        return data.is_object() ? data : json::Value(json::Object{});
    } catch (const json::ParseError&) {
        return json::Object{};
    }
}

std::string Intake::known() const {
    std::vector<std::string> observed;
    for (const auto& o : evidence) observed.push_back(o.str());
    return "WHAT THE USER SAYS:\n" + (context.empty() ? std::string("nothing") : context) + "\n\nVIDEO OBSERVATIONS:\n" +
           (observed.empty() ? std::string("no video") : py::join(observed, "\n"));
}

// 1
Info Intake::understand() {
    Dict<std::string> books;
    for (const auto& [name, path] : library::rulebooks()) books[name] = library::title_of(path);
    std::vector<std::string> listing;
    for (const auto& [n, t] : books) listing.push_back("- " + n + ": " + t);
    json::Value d = ask(
        known() + "\n\nRULEBOOKS AVAILABLE (name: title):\n" + py::join(listing, "\n") + "\n\n" +
            "Return JSON with these keys:\n"
            "\"sport\": the sport\n"
            "\"rulebooks\": the names of the rulebooks above that cover this sport (a list; empty if none do)\n"
            "\"title\": a one-line title for the case\n"
            "\"incident\": two or three neutral sentences on what happened\n"
            "\"officials\": who made the call, for example \"Referee & VAR\"\n"
            "\"call\": what the officials ruled, in a few words\n"
            "\"call_reason\": the reason the officials gave, or \"\" if unknown\n"
            "\"parties\": every team or player the call affects (at least two), as a list of {\"name\": ..., "
            "\"wants\": the outcome they want, in a few words, \"stake\": what they gain or lose}\n\n" +
            EXAMPLE,
        800);
    Info info;
    info.sport = text(d.get("sport"));
    info.title = text(d.get("title"));
    info.incident = text(d.get("incident"));
    info.officials = text(d.get("officials"));
    info.call = text(d.get("call"));
    info.call_reason = text(d.get("call_reason"));
    if (info.call.empty()) throw IntakeError("I couldn't tell what the officials ruled. Say it in --context.");
    std::set<std::string> seen;
    for (json::Value p : json::iterate(d.get("parties"))) {
        if (!p.is_object()) p = json::Object{{"name", p}};
        std::string name = text(p.get("name"));
        if (!name.empty() && !seen.count(py::lower(name))) {
            seen.insert(py::lower(name));
            info.parties.push_back({name, text(p.get("wants")), text(p.get("stake"))});
        }
    }
    if (info.parties.size() < 2)
        throw IntakeError("I couldn't identify at least two parties. Name the teams or players in --context.");
    for (const auto& b : json::iterate(d.get("rulebooks")))
        if (b.is_string() && books.contains(b.as_string())) info.rulebooks.push_back(b.as_string());
    if (info.rulebooks.empty() && !info.sport.empty()) {  // fall back to rulebooks whose name or title mentions the sport
        for (const auto& [n, t] : books)
            if (py::contains(py::lower(n + " " + t), py::lower(info.sport))) info.rulebooks.push_back(n);
    }
    if (info.rulebooks.empty())
        throw IntakeError("No rulebook in rules/ covers " + (info.sport.empty() ? std::string("this sport") : info.sport) +
                          ". Add one (markdown or PDF) and run it again.");
    if (info.officials.empty()) info.officials = "Officials";
    if (info.title.empty()) info.title = info.call;
    return info;
}

// 2
Dict<library::Rule> Intake::find_rules(const Info& info) {
    Dict<library::Rule> clauses;
    for (const auto& name : info.rulebooks)
        for (const auto& [id, rule] : library::load_rulebook(name)) clauses[id] = rule;
    std::string query = py::join({info.incident, info.call, info.call_reason, context}, " ");
    std::vector<library::Rule> shortlist =
        clauses.size() <= SHORTLIST ? clauses.values() : library::search(clauses, query, SHORTLIST);
    if (shortlist.empty())
        throw IntakeError("Nothing in " + py::join(info.rulebooks, ", ") + " matches this incident.");
    std::vector<std::string> candidates, ids;
    for (const auto& r : shortlist) {
        candidates.push_back("[" + r.id + "] " + r.title + ": " + snippet(r.text, 45));
        ids.push_back(r.id);
    }
    json::Value d = ask("INCIDENT: " + info.incident + "\nTHE CALL: " + info.officials + " ruled: " + info.call + ". " +
                            info.call_reason + "\n\n" + "CANDIDATE CLAUSES:\n" + py::join(candidates, "\n") +
                            "\n\nWhich of these clauses could decide this case? Return JSON {\"rules\": [clause IDs]} "
                            "with 3 to 8 IDs, most relevant first.",
                        200);
    std::vector<std::string> picked;
    for (const auto& x : json::iterate(d.get("rules"))) {
        std::string id = py::strip(json::str(x), "[] ");
        if (in(ids, id) && !in(picked, id)) picked.push_back(id);
    }
    if (picked.size() < 2) picked.assign(ids.begin(), ids.begin() + static_cast<long>(std::min<size_t>(6, ids.size())));
    Dict<library::Rule> chosen;
    for (size_t i = 0; i < std::min<size_t>(8, picked.size()); ++i) chosen[picked[i]] = clauses.at(picked[i]);
    return chosen;
}

// 3
std::pair<Dict<std::string>, std::string> Intake::find_rulings(const Info& info, const Dict<library::Rule>& rules) {
    std::vector<std::string> rule_lines, wants;
    for (const auto& [_, r] : rules) rule_lines.push_back("[" + r.id + "] " + r.title + ": " + snippet(r.text, 80));
    for (const auto& p : info.parties) wants.push_back(p.name + " wants " + (p.wants.empty() ? "?" : p.wants));
    json::Value d = ask(
        "INCIDENT: " + info.incident + "\nTHE CALL: " + info.officials + " ruled: " + info.call + ". " + info.call_reason +
            "\n\n" + "RULES:\n" + py::join(rule_lines, "\n") + "\n\n" + "WHAT EACH PARTY WANTS: " + py::join(wants, "; ") +
            "\n\nList the rulings a review could reach, 2 to 4 in all. A ruling is an outcome the officials could give "
            "(what happens in the game), not a reason or a finding. The first ruling must be the call itself, and the "
            "list must include the outcome each party wants. "
            R"(Return JSON {"rulings": [{"id": a short snake_case id, "text": the outcome and what it means for each )"
            R"(side}], "call_id": the id of the call}. Example from a different case: {"rulings": [{"id": "no_basket", )"
            R"("text": "No basket: the Monarchs win 100-99"}, {"id": "basket_counts", "text": "The basket counts: the )"
            R"(Lynx win 101-100"}], "call_id": "no_basket"})",
        400);
    Dict<std::string> rulings;
    for (const auto& r : json::iterate(d.get("rulings"))) {
        if (!r.is_object() || !(json::truthy(r.get("text")) || json::truthy(r.get("id")))) continue;
        std::string rid = slug(either(r.get("id"), r.get("text")));
        if (rid.empty()) rid = "ruling_" + std::to_string(rulings.size() + 1);
        rulings.setdefault(rid, py::strip(json::str(either(r.get("text"), r.get("id")))));
    }
    std::string call = d.contains("call_id") ? slug(d.get("call_id")) : "";
    if (!rulings.contains(call) && !rulings.empty()) {  // the call is meant to come first
        call = rulings.front().first;
        warnings.push_back("Check that " + py::repr(call) + " is the call the officials made.");
    }
    if (!rulings.contains(call)) {
        call = "call";
        Dict<std::string> with_call{{"call", info.call}};
        for (const auto& [k, v] : rulings) with_call[k] = v;
        rulings = with_call;
    }
    if (rulings.size() < 2)
        throw IntakeError("I could only find the call itself, no other ruling. Say in --context what each side wants "
                          "instead.");
    Dict<std::string> kept;
    size_t i = 0;
    for (const auto& [k, v] : rulings)
        if (i++ < 5) kept[k] = v;
    kept[call] = rulings.at(call);
    return {kept, call};
}

// 4
std::pair<Dict<std::string>, Dict<std::string>> Intake::find_facts(const Info& info, const Dict<std::string>& rulings,
                                                                   const Dict<library::Rule>& rules) {
    std::vector<std::string> titles;
    for (const auto& [_, r] : rules) titles.push_back(r.title);
    json::Value d = ask(
        known() + "\n\nINCIDENT: " + info.incident + "\nRULINGS IN QUESTION: " + py::join(rulings.values(), "; ") +
            "\nRULES THAT APPLY: " + py::join(titles, "; ") + "\n\n" +
            R"(Split what is known into facts and disputed points. Return JSON {"facts": [{"text": ..., "source": )"
            R"("context" or "video m:ss.s"}], "disputed": [{"text": ...}]}. A fact is something everyone would accept: )"
            "said by the user, or plainly visible in the video. Anything that turns on fine judgement (exact "
            "positions or timing, intent, what might have happened) is a disputed point. Use a video observation only "
            "if it matters for the rulings, and never one that contradicts what the user says. Give 3 to 8 facts and "
            "1 to 4 disputed points, one short sentence each.",
        700);
    Dict<std::string> facts;
    for (json::Value f : json::iterate(d.get("facts"))) {
        if (!f.is_object()) f = json::Object{{"text", f}};
        std::string fact = text(f.get("text")), source = py::lower(text(f.get("source")));
        if (fact.empty()) continue;
        std::string where = source.rfind("video", 0) == 0 ? "video " + py::strip(py::removeprefix(source, "video"))
                                                          : "your context";
        facts["F" + std::to_string(facts.size() + 1)] = fact + " [" + where + "]";
    }
    if (facts.empty()) throw IntakeError("I couldn't establish any facts. Add what everyone agrees on to --context.");
    Dict<std::string> disputed;
    int i = 0;
    for (const auto& p : json::iterate(d.get("disputed"))) {
        ++i;
        std::string point = text(p.is_object() ? p.get("text") : p);
        if (!point.empty()) disputed["D" + std::to_string(i)] = point;
    }
    return {facts, disputed};
}

// 5
std::vector<cases::Advocate> Intake::take_positions(const Info& info, const Dict<std::string>& rulings) {
    std::vector<cases::Advocate> advocates;
    std::vector<std::string> options;
    for (const auto& [k, v] : rulings) options.push_back("- " + k + ": " + v);
    for (const auto& p : info.parties) {
        std::string wants = p.wants.empty() ? "" : p.name + " wants " + p.wants + ". ";
        json::Value d = ask("INCIDENT: " + info.incident + "\n" + wants + p.stake + "\n\nPOSSIBLE RULINGS:\n" +
                                py::join(options, "\n") + "\n\nRank every ruling from best to worst for " + p.name +
                                ": first the ruling that gives " + p.name +
                                " what it wants, last the one that hurts it most. "
                                R"(Return JSON {"ranking": [ruling ids, best first]})",
                            150, "You are the advocate for " + p.name + " in a sports rules review. Answer in JSON.");
        std::vector<std::string> ranking;
        for (const auto& r : json::iterate(d.get("ranking"))) {
            std::string id = slug(r);
            if (rulings.contains(id) && !in(ranking, id)) ranking.push_back(id);
        }
        for (const auto& [r, _] : rulings)
            if (!in(ranking, r)) ranking.push_back(r);
        Dict<double> likes;
        for (size_t i = 0; i < ranking.size(); ++i)
            likes[ranking[i]] = py::round(1 - static_cast<double>(i) / static_cast<double>(ranking.size() - 1), 2);
        std::string side = py::strip("You represent " + p.name + ". " + wants + p.stake);
        advocates.push_back({p.name, side, likes});
        log("  " + p.name + ": wants " + ranking[0]);
    }
    std::set<std::string> favorites;
    for (const auto& a : advocates) {
        std::string best;
        double top = -1e300;
        for (const auto& [r, like] : a.likes)
            if (like > top) best = r, top = like;
        favorites.insert(best);
    }
    if (favorites.size() == 1)
        warnings.push_back("Every party's first choice is " + *favorites.begin() +
                           ". In a real dispute the parties want different rulings, so check each party's likes in "
                           "case.json.");
    return advocates;
}

cases::Case Intake::build(const std::string& level) {
    Info info = understand();
    log("  " + info.sport + ": " + info.title);
    auto rules = find_rules(info);
    log("  rules: " + py::join(rules.keys(), ", "));
    auto [rulings, call] = find_rulings(info, rules);
    auto [facts, disputed] = find_facts(info, rulings, rules);
    auto parties = take_positions(info, rulings);
    cases::Case c;
    c.sport = info.sport;
    c.level = level;
    c.title = info.title;
    c.incident = info.incident;
    c.officials = info.officials;
    c.call = call;
    c.call_reason = info.call_reason;
    c.rulings = rulings;
    c.facts = facts;
    c.disputed = disputed;
    c.rulebook = info.rulebooks;
    c.rules = rules.keys();
    c.parties = parties;
    return c;
}

}  // namespace intake
