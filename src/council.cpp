#include "council.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <set>

#include "cli.hpp"
#include "evidence.hpp"

namespace council {

const std::vector<std::pair<std::string, std::string>> PANEL = {
    {"Panelist A", "You pay most attention to what the clip and the established facts actually show."},
    {"Panelist B", "You pay most attention to the exact wording of the rules."},
    {"Panelist C", "You pay most attention to fairness and the purpose behind the rules."},
};

const std::string DEMO_HEADER =
    "> **Simulation.** Every advocate and panel member is played by an AI model. The teams and players are fictional, "
    "and the soccer, basketball and golf rule excerpts in rules/ are paraphrased.";
const std::string SAVED_HEADER =
    "> Every advocate and panel member is played by an AI model. The case was built by AI models from a video and the "
    "user's description; check its facts in case.json.";

fs::path HERE = project_root();

namespace {

const std::regex FACT_ID(R"(\b[FD]\d+\b)");
const std::regex POSITION(R"(^\W*POSITION\W*?[:\-]\s*\W*([A-Za-z0-9_]+))",
                          std::regex::ECMAScript | std::regex::icase | std::regex::multiline);
const std::regex LABEL(R"(^\W*(MESSAGE|REASON|STATEMENT|NOTES)\s*[:\-]\s*)", std::regex::ECMAScript | std::regex::icase);
const std::regex TRAILING_NUMBER(R"((\d+(?:\.\d+)?)$)");

std::vector<std::string> findall(const std::regex& re, const std::string& text, int group = 0) {
    std::vector<std::string> found;
    for (std::sregex_iterator it(text.begin(), text.end(), re), end; it != end; ++it) found.push_back((*it)[group].str());
    return found;
}

std::string escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (std::string_view("\\^$.|?*+()[]{}-/").find(c) != std::string_view::npos) out += '\\';
        out += c;
    }
    return out;
}

int count(const Dict<int>& tally, const std::string& key) {
    const int* n = tally.find(key);
    return n ? *n : 0;
}

int most(const Dict<int>& tally) {
    int best = 0;
    for (const auto& [_, n] : tally) best = std::max(best, n);
    return best;
}

std::string plural_rounds(int rounds) { return "after " + std::to_string(rounds) + " round" + (rounds > 1 ? "s" : ""); }

}  // namespace

std::string favorite(const cases::Advocate& a) {
    std::string best;
    double top = 0;
    bool first = true;
    for (const auto& [r, like] : a.likes) {
        if (first || like > top) {
            best = r;
            top = like;
            first = false;
        }
    }
    if (first) throw ValueError(a.name + " has no preferences");
    return best;
}

std::pair<std::string, std::string> parse(const std::string& reply) {
    auto named = findall(POSITION, reply, 1);
    std::string body = std::regex_replace(reply, POSITION, "");
    std::string text =
        py::squash(std::regex_replace(py::strip(body), LABEL, "", std::regex_constants::format_first_only));
    return {py::strip(text, "\"* "), named.empty() ? "" : py::lower(named.back())};
}

json::Value Result::to_json() const {
    return json::Object{
        {"status", status},
        {"choice", choice},
        {"tally", tally},
        {"votes", votes},
        {"dissent", dissent},
        {"how", how},
        {"decided_by", decided_by},
        {"rounds", rounds},
        {"positions", positions},
        {"upheld", upheld},
        {"notes", notes},
        {"statements", statements},
        {"frames_seen", frames_seen},
    };
}

Council::Council(std::string name_, cases::Case c, llm::Chat ask_fn, int rounds_, Out out_fn, llm::Chat look_fn,
                 int panel_size)
    : name(std::move(name_)),
      case_(std::move(c)),
      ask(std::move(ask_fn)),
      rounds(rounds_),
      look(std::move(look_fn)),
      print_(out_fn ? std::move(out_fn) : Out([](const std::string& text) { std::cout << text << std::endl; })) {
    advocates = case_.advocates();
    Dict<library::Rule> book;
    for (const auto& rulebook : case_.rulebooks())
        for (const auto& [id, rule] : library::load_rulebook(rulebook)) book[id] = rule;
    std::vector<std::string> missing;
    for (const auto& r : case_.rules)
        if (!book.contains(r)) missing.push_back(r);
    if (!missing.empty())
        throw ValueError(py::join(case_.rulebooks(), ", ") + " has no clause " + py::join(missing, ", "));
    for (const auto& r : case_.rules) rules[r] = book.at(r);

    std::set<std::string> prefix_set;
    for (const auto& [id, _] : book) prefix_set.insert(id.substr(0, id.find('-')));
    std::vector<std::string> prefixes(prefix_set.begin(), prefix_set.end());
    std::stable_sort(prefixes.begin(), prefixes.end(), [](const auto& a, const auto& b) { return a.size() > b.size(); });
    std::vector<std::string> escaped;
    for (const auto& p : prefixes) escaped.push_back(escape(p));
    rule_id_ = std::regex(R"(\b(?:)" + py::join(escaped, "|") + R"()-[A-Za-z0-9.]*[A-Za-z0-9])");

    if (look) {
        size_t seats = static_cast<size_t>(std::clamp(panel_size, 0, static_cast<int>(PANEL.size())));
        for (size_t i = 0; i < seats; ++i) panel.push_back({PANEL[i].first, PANEL[i].second, ""});
    }
    if (!panel.empty()) frames = load_frames();
    for (const auto& a : advocates) position[a.name] = favorite(a);
}

void Council::out(const std::string& text) {
    print_(text);
    lines.push_back(text);
}

std::pair<std::vector<std::string>, std::vector<std::string>> Council::ids(const std::string& text) const {
    std::vector<std::string> found;
    for (const auto& list : {findall(rule_id_, text), findall(FACT_ID, text)})
        for (const auto& id : list)
            if (std::find(found.begin(), found.end(), id) == found.end()) found.push_back(id);
    std::vector<std::string> listed, struck;
    for (const auto& id : found) {
        bool known = rules.contains(id) || case_.facts.contains(id) || case_.disputed.contains(id);
        (known ? listed : struck).push_back(id);
    }
    return {listed, struck};
}

std::vector<std::pair<double, std::string>> Council::load_frames() const {
    // Up to PANEL_FRAMES of the case's stills, spread evenly.
    std::vector<fs::path> paths;
    std::error_code ec;
    for (const auto& f : case_.frames) {
        fs::path p(f);
        if (!p.is_absolute()) p = HERE / p;
        if (fs::exists(p, ec)) paths.push_back(p);
    }
    if (paths.size() > PANEL_FRAMES) {
        std::vector<fs::path> picked;
        for (size_t i = 0; i < PANEL_FRAMES; ++i)
            picked.push_back(paths[static_cast<size_t>(std::nearbyint(static_cast<double>(i) * (paths.size() - 1) /
                                                                      (PANEL_FRAMES - 1)))]);
        paths = picked;
    }
    std::vector<std::pair<double, std::string>> found;
    for (const auto& p : paths) {
        std::smatch m;
        std::string stem = p.stem().string();
        double t = std::regex_search(stem, m, TRAILING_NUMBER) ? std::stod(m[1].str()) : 0.0;
        found.emplace_back(t, read_file(p));
    }
    return found;
}

// ---- what everyone is told ----

std::string Council::brief() const {
    const auto& c = case_;
    std::vector<std::string> facts, disputed, rule_lines, rulings;
    for (const auto& [k, v] : c.facts) facts.push_back(k + ": " + v);
    for (const auto& [k, v] : c.disputed) disputed.push_back(k + ": " + v);
    for (const auto& [_, r] : rules) rule_lines.push_back("[" + r.id + "] " + r.title + ": " + py::squash(r.text));
    for (const auto& [k, v] : c.rulings) rulings.push_back("- " + k + ": " + v + (k == c.call ? " (the call)" : ""));
    std::string disputed_text = disputed.empty() ? "none" : py::join(disputed, "\n");
    return "SPORT: " + c.sport + "\nINCIDENT: " + c.incident + "\n" + "THE CALL: " + c.officials +
           " ruled: " + c.rulings.at(c.call) + ". " + c.call_reason + "\n\n" +
           "ESTABLISHED FACTS (everyone accepts these; cite them by ID):\n" + py::join(facts, "\n") + "\n\n" +
           "DISPUTED POINTS (the sides see these differently):\n" + disputed_text + "\n\n" +
           "RULES (the only rules anyone may cite; cite them by ID):\n" + py::join(rule_lines, "\n") + "\n\n" +
           "POSSIBLE RULINGS (refer to them by ID):\n" + py::join(rulings, "\n");
}

std::string Council::system(const cases::Advocate& a) const {
    std::vector<std::string> others;
    for (const auto& b : advocates)
        if (&b != &a) others.push_back(b.name);
    std::string decides =
        !panel.empty() ? "then a neutral panel" + std::string(!frames.empty() ? ", who also watch the clip," : "") +
                             " decides it, so argue to persuade them"
                       : "then decide it together; if they don't all agree by the end, it's decided by a vote";
    return "You are the advocate for " + a.name + " on a sports rules council discussing a contested call with " +
           py::join(others, ", ") + ". " + a.side + "\n\n" + brief() + "\n\n" +
           "How the council works: the advocates discuss the call in turn, " + decides +
           ". Use only the facts and rules above, citing them by ID; never invent facts, rules, replays, quotes or "
           "statistics (a citation of anything not listed is struck from the record). Speak up for your side, but this "
           "is a real discussion: listen to the others, and change your position only if an argument actually "
           "convinces you, not just to be agreeable. Disagree with arguments, not people.";
}

std::string Council::panel_system(const Panelist& p) const {
    return "You are " + p.name +
           ", a neutral member of the panel deciding a contested sports call. You don't represent the officials or any "
           "team or player, and nothing rides on the result for you. " +
           p.lens +
           " Advocates for the officials and for each party discuss the call; then each panel member votes on the "
           "ruling, independently. Decide on the evidence and the rules, not on who argued hardest. Use only the facts "
           "and rules below" +
           (!frames.empty() ? ", and when you look at the clip, describe only what it actually shows" : "") + ".\n\n" +
           brief();
}

std::string Council::ask_(const cases::Advocate& a, const std::string& user, int max_tokens) {
    ++calls;
    return ask(system(a), user, max_tokens, 7 + calls, {}, false);
}

std::string Council::look_(const Panelist& p, const std::string& user, int max_tokens) {
    ++calls;
    llm::Images images;
    for (const auto& [_, jpeg] : frames) images.push_back(jpeg);
    return look(panel_system(p), user, max_tokens, 7 + calls, images, false);
}

std::string Council::discussion(const cases::Advocate* viewer) const {
    // The latest turns as the viewer sees them: its own are labelled 'You', so it doesn't answer itself.
    std::vector<std::string> shown;
    size_t from = turns.size() > HISTORY ? turns.size() - HISTORY : 0;
    for (size_t i = from; i < turns.size(); ++i) {
        const Turn& t = turns[i];
        shown.push_back(viewer && t.speaker == viewer->name ? "- You (backing " + t.position + "): " + t.message
                                                            : "- " + t.speaker + " (backs " + t.position + "): " + t.message);
    }
    return py::join(shown, "\n");
}

// ---- the advocates ----

Turn Council::speak(const cases::Advocate& a, int rnd) {
    // One turn: the advocate answers the discussion so far and says which ruling it backs now.
    const auto& c = case_;
    std::vector<std::string> wishes = c.rulings.keys();
    std::stable_sort(wishes.begin(), wishes.end(),
                     [&](const auto& x, const auto& y) { return a.likes.at(x) > a.likes.at(y); });
    std::vector<std::string> lines_ = {
        "Round " + std::to_string(rnd + 1) + " of " + std::to_string(rounds) + ".",
        "What your side would like, best first: " + py::join(wishes, ", ") + ". You currently back: " +
            position[a.name] + "."};
    if (!turns.empty()) {
        lines_.push_back("THE DISCUSSION SO FAR:");
        lines_.push_back(discussion(&a));
    } else {
        lines_.push_back("Nobody has spoken yet.");
    }
    bool spoken = std::any_of(turns.begin(), turns.end(), [&](const Turn& t) { return t.speaker == a.name; });
    std::string instruction;
    if (!spoken) {
        instruction = &a == &officials() ? "Open by explaining the call (" + c.call + ") and why it's right."
                                         : "Open by making the case for " + favorite(a) + ", the ruling your side wants.";
    } else {
        instruction =
            "Respond to what the others have said since you last spoke: answer them by name, accept the points you "
            "think are right and push back on the ones you think are wrong. Make a new point; don't repeat what you've "
            "already said. If an argument has changed your mind, change your position and say which argument did it.";
    }
    if (rnd == rounds - 1) instruction += " This is the last round of discussion.";
    lines_.push_back(instruction);
    lines_.push_back("Reply in exactly this format:");
    lines_.push_back("MESSAGE: <two to four sentences, speaking to the others>");
    lines_.push_back("POSITION: <the ruling you back now: one of " + py::join(c.rulings.keys(), ", ") + ">");
    std::string reply = ask_(a, py::join(lines_, "\n"), 230);
    auto [message, said] = parse(reply);
    std::string before = position[a.name], note;
    if (!c.rulings.contains(said)) {
        said = before;
        note = "no valid POSITION line, so its position is unchanged";
    }
    position[a.name] = said;
    Turn turn{rnd, a.name, message, before, said, ids(reply).second, note};
    turns.push_back(turn);
    return turn;
}

void Council::closing_statements() {
    for (const auto& a : advocates) {
        std::string reply = ask_(
            a,
            "The discussion is over. Here is how it went:\n" + discussion(&a) + "\n\nA neutral panel" +
                (!frames.empty() ? ", who have also watched the clip," : "") +
                " will now decide. Give them your closing statement: one or two sentences on why they should rule " +
                position[a.name] + ". Reply in exactly this format:\nSTATEMENT: <your statement>",
            150);
        statements[a.name] = parse(reply).first;
    }
}

Votes Council::closing_vote() {
    // Without a panel: each advocate votes on its own, having heard the discussion but not the other votes.
    Votes votes;
    for (const auto& a : advocates) {
        std::string reply = ask_(a,
                                 "The discussion is over. Here is how it went:\n" + discussion(&a) + "\n\nYou currently back " +
                                     position[a.name] +
                                     ". Cast your final vote for the ruling the council should make, having heard "
                                     "everyone. Reply in exactly this format:\nREASON: <one sentence>\nPOSITION: <one of " +
                                     py::join(case_.rulings.keys(), ", ") + ">",
                                 120);
        auto [reason, said] = parse(reply);
        votes[a.name] = {case_.rulings.contains(said) ? said : position[a.name], reason};
    }
    return votes;
}

Result Council::decide(const Votes& votes) const {
    // Without a panel: the advocates' closing vote. A tie goes the officials' way, or else the call stands.
    const auto& c = case_;
    Result r;
    for (const auto& [_, vote] : votes) ++r.tally[vote.first];
    std::vector<std::string> leaders;
    for (const auto& [ruling, _] : c.rulings)
        if (count(r.tally, ruling) == most(r.tally)) leaders.push_back(ruling);
    r.status = "VOTE";
    r.choice = leaders.at(0);
    if (leaders.size() > 1) {
        r.status = "TIE";
        std::string officials_vote = votes.at(officials().name).first;
        if (std::find(leaders.begin(), leaders.end(), officials_vote) != leaders.end()) {
            r.choice = officials_vote;
            r.how = "the officials' vote breaks it";
        } else {
            r.choice = c.call;
            r.how = "the call stands";
        }
    }
    for (const auto& [n, vote] : votes)
        if (vote.first != r.choice) r.dissent.push_back(n + " (" + vote.first + "): " + vote.second);
    r.votes = votes;
    r.decided_by = "advocates";
    return r;
}

// ---- the panel ----

std::string Council::first_look(Panelist& p) {
    // Before the discussion, the panel member watches the clip and notes what it sees, without deciding.
    std::vector<std::string> times;
    for (const auto& [t, _] : frames) times.push_back(evidence::stamp(t));
    p.notes = parse(look_(p,
                          "Before hearing the advocates, watch the clip. These " + std::to_string(frames.size()) +
                              " frames are from it, at " + py::join(times, ", ") +
                              ", in order. Write down what you see that matters for the ruling, and say so if the frames "
                              "don't show something important. Don't decide yet. Reply in two to five sentences, "
                              "starting with 'NOTES:'.",
                          220))
                  .first;
    return p.notes;
}

std::pair<std::string, std::string> Council::panel_vote(const Panelist& p) {
    // After the discussion: the panel member looks at the frames again and votes. ("", reason) is no valid vote.
    std::vector<std::string> closing;
    for (const auto& [n, s] : statements) closing.push_back("- " + n + " (backs " + position[n] + "): " + s);
    std::string clip = !frames.empty() ? "YOUR NOTES FROM WATCHING THE CLIP BEFORE THE DISCUSSION: " + p.notes +
                                             "\nThe frames are attached again; look at them before you decide.\n\n"
                                       : "";
    auto [reason, said] = parse(look_(
        p,
        "THE ADVOCATES' DISCUSSION:\n" + discussion(nullptr) + "\n\nTHEIR CLOSING STATEMENTS:\n" + py::join(closing, "\n") +
            "\n\n" + clip +
            "Now vote for the ruling the evidence and the rules support, whichever side that favours. Reply in exactly "
            "this format:\n"
            "REASON: <one or two sentences: what in the clip, the facts or the arguments decided it for you>\n"
            "POSITION: <one of " +
            py::join(case_.rulings.keys(), ", ") + ">",
        200));
    return {case_.rulings.contains(said) ? said : "", reason};
}

Result Council::panel_decide(const Votes& votes) const {
    // The panel's majority wins. A tie goes to whichever tied ruling more advocates ended up backing; failing that,
    // the call stands.
    const auto& c = case_;
    Result r;
    for (const auto& [_, vote] : votes)
        if (!vote.first.empty()) ++r.tally[vote.first];
    std::vector<std::string> leaders;
    for (const auto& [ruling, _] : c.rulings)
        if (!r.tally.empty() && count(r.tally, ruling) == most(r.tally)) leaders.push_back(ruling);
    if (leaders.size() == 1) {
        r.status = count(r.tally, leaders[0]) == static_cast<int>(votes.size()) ? "CONSENSUS" : "VOTE";
        r.choice = leaders[0];
    } else {
        r.status = "TIE";
        if (leaders.empty()) leaders = c.rulings.keys();
        Dict<int> backing;
        for (const auto& [_, ruling] : position) ++backing[ruling];
        int top = 0;
        for (const auto& x : leaders) top = std::max(top, count(backing, x));
        std::vector<std::string> favored;
        for (const auto& x : leaders)
            if (count(backing, x) == top) favored.push_back(x);
        if (favored.size() == 1) {
            r.choice = favored[0];
            r.how = "the advocates' final positions break the tie";
        } else {
            r.choice = c.call;
            r.how = "the call stands";
        }
    }
    for (const auto& [n, vote] : votes)
        if (vote.first != r.choice)
            r.dissent.push_back(n + " (" + (vote.first.empty() ? "no valid vote" : vote.first) + "): " + vote.second);
    r.votes = votes;
    r.decided_by = "panel";
    return r;
}

// ---- the whole review ----

Result Council::run() {
    const auto& c = case_;
    out("=== " + name + " (" + c.level + "): " + c.sport + " ===");
    out(c.title);
    out(c.incident);
    out("The call: " + c.officials + " ruled: " + c.rulings.at(c.call) + ". " + c.call_reason);
    if (!c.note.empty()) out(c.note);
    for (const auto& a : advocates)
        out("  " + a.name + " starts out backing " + position[a.name] + (&a == &officials() ? " (the call)." : "."));
    if (!panel.empty()) {
        std::vector<std::string> names;
        for (const auto& p : panel) names.push_back(p.name);
        out("  Neutral panel: " + py::join(names, ", ") + ".");
    }
    out();
    if (!panel.empty() && !c.frames.empty() && frames.empty())
        out("  (The case lists frames from the clip, but they weren't found, so the panel decides without them.)");
    if (!frames.empty()) {
        out("  The panel watches the clip first (" + std::to_string(frames.size()) + " frames, " +
            evidence::stamp(frames.front().first) + " to " + evidence::stamp(frames.back().first) + ")");
        for (auto& p : panel) out("    " + p.name + ": " + first_look(p));
    }

    std::string consensus;
    size_t n = advocates.size();
    int done = 0;
    for (int rnd = 0; rnd < rounds; ++rnd) {
        done = rnd + 1;
        out("  Round " + std::to_string(rnd + 1));
        bool changed = false;
        for (size_t k = 0; k < n; ++k) {  // a different advocate opens each round
            const auto& a = advocates[(rnd % n + k) % n];
            Turn t = speak(a, rnd);
            changed = changed || t.position != t.before;
            std::string stance = t.position != t.before ? t.before + " -> " + t.position : t.position;
            out("    " + a.name + " [" + stance + "]: " + t.message + (t.note.empty() ? "" : " [" + t.note + "]") +
                (t.struck.empty() ? "" : " [struck: " + py::join(t.struck, ", ") + ", not in this case]"));
            std::set<std::string> backed;
            for (const auto& [_, p] : position) backed.insert(p);
            if (rnd >= 1 && backed.size() == 1) {
                consensus = t.position;
                break;
            }
        }
        if (!consensus.empty()) {
            out("    (The advocates all back " + consensus + ", so the discussion ends here.)");
            break;
        }
        if (rnd >= 2 && !changed && rnd < rounds - 1) {
            out("    (Nobody changed position this round, so the discussion ends here.)");
            break;
        }
    }

    Result result;
    if (!panel.empty()) {
        out("  Closing statements to the panel");
        closing_statements();
        for (const auto& [who, statement] : statements) out("    " + who + " (" + position[who] + "): " + statement);
        out("  Panel vote");
        Votes votes;
        for (const auto& p : panel) votes[p.name] = panel_vote(p);
        for (const auto& [who, vote] : votes)
            out("    " + who + ": " + (vote.first.empty() ? "no valid vote" : vote.first) + ". " + vote.second);
        result = panel_decide(votes);
    } else if (!consensus.empty()) {
        result.status = "CONSENSUS";
        result.choice = consensus;
        result.decided_by = "advocates";
    } else {
        out("  Closing vote");
        Votes votes = closing_vote();
        for (const auto& [who, vote] : votes) out("    " + who + ": " + vote.first + ". " + vote.second);
        result = decide(votes);
    }
    result.rounds = done;
    result.positions = position;
    result.upheld = result.choice == c.call;
    for (const auto& p : panel)
        if (!p.notes.empty()) result.notes[p.name] = p.notes;
    result.statements = statements;
    for (const auto& [t, _] : frames) result.frames_seen.push_back(t);

    const Result& r = result;
    std::string when = plural_rounds(done);
    bool by_panel = r.decided_by == "panel";
    std::string who = by_panel ? "the panel" : "the advocates";
    size_t size = by_panel ? panel.size() : n;
    std::string verdict = py::rstrip(c.rulings.at(r.choice), ".") + ". The call is " + (r.upheld ? "upheld" : "overturned") + ".";
    if (r.status == "CONSENSUS") {
        out("  DECISION " + when + ": CONSENSUS, " + who + " all back " + r.choice + ": " + verdict);
    } else if (r.status == "VOTE") {
        out("  DECISION " + when + ", by " + who + " (" + std::to_string(count(r.tally, r.choice)) + " of " +
            std::to_string(size) + "): " + verdict);
    } else {
        std::vector<std::string> parts;
        for (const auto& [k, v] : r.tally) parts.push_back(k + " " + std::to_string(v));
        std::string split = parts.empty() ? "no valid votes" : py::join(parts, ", ");
        out("  DECISION " + when + ": " + who + " are split (" + split + "), so " + r.how + ": " + verdict);
    }
    for (const auto& d : r.dissent) out("    Dissent: " + d);
    if (by_panel) {
        std::vector<std::string> parts;
        for (const auto& [k, v] : position) parts.push_back(k + " " + v);
        out("    The advocates' final positions: " + py::join(parts, "; "));
    }
    return result;
}

Result debate(const std::string& name, const cases::Case& c, const llm::Chat& ask, int rounds, const fs::path& transcript,
              const std::string& header, const llm::Chat& look, int panel, Out out) {
    double started = now();
    Council council(name, c, ask, rounds, out, look, panel);
    Result result = council.run();
    std::string models = llm::MODEL + (!council.panel.empty() ? " for the advocates, " + llm::VISION_MODEL + " for the panel" : "");
    council.out(py::format("  (%s; %d model calls, %.0fs)", models.c_str(), council.calls, now() - started));
    fs::create_directories(transcript.parent_path());
    write_file(transcript, header + "\n\n" + py::join(council.lines, "\n") + "\n");
    return result;
}

namespace {
const char* DESCRIPTION = R"(Sports rules council: advocates for the officials and for every party discuss a contested call, and a neutral
panel decides it.

There's no answer key: the panel decides, and the transcript shows why. With --panel 0 the advocates decide by
themselves instead: a consensus during the discussion, or else a closing vote among them.

    build/council                  # every demo case (needs Ollama running, or LLM_MODEL set)
    build/council dogso_card       # one case
    build/council all --rounds 3   # a shorter discussion
    build/council all --panel 0    # no panel: the advocates decide among themselves
    build/council --case-file cases/<name>/case.json   # a saved case, such as one build/review built from a video)";
}

int main(const std::vector<std::string>& args) {
    std::vector<std::string> choices = cases::CASES().keys();
    choices.push_back("all");
    cli::Parser ap("council", DESCRIPTION);
    ap.positional("case", "", "all", choices)
        .option("--case-file", "discuss a saved case instead, such as one review built")
        .option("--rounds", py::format("most rounds of discussion (default %d)", MAX_ROUNDS), std::to_string(MAX_ROUNDS), "int")
        .option("--panel", py::format("neutral panel members who decide (default %zu; 0: the advocates decide)", PANEL.size()),
                std::to_string(PANEL.size()), "int", {"0", "1", "2", "3"});
    ap.parse(args);
    int rounds = ap.integer("--rounds"), panel = ap.integer("--panel");
    llm::Chat ask = llm::connect(llm::MODEL);
    llm::Chat look = panel ? llm::connect(llm::VISION_MODEL) : nullptr;

    if (ap.given("--case-file")) {
        fs::path file = ap.str("--case-file");
        std::string name = file.stem() == "case" ? file.parent_path().filename().string() : file.stem().string();
        debate(name, cases::load_case(file), ask, rounds, file.parent_path() / "transcript.md", SAVED_HEADER, look, panel);
        return 0;
    }

    std::vector<std::string> names = ap.str("case") == "all" ? cases::CASES().keys() : std::vector<std::string>{ap.str("case")};
    std::vector<std::pair<std::string, Result>> rows;
    for (const auto& name : names) {
        const auto& c = cases::CASES().at(name);
        rows.emplace_back(name, debate(name, c, ask, rounds, HERE / "transcripts" / (name + ".md"), DEMO_HEADER, look, panel));
        std::cout << std::endl;
    }
    std::cout << py::ljust("case", 20) << py::ljust("kind", 16) << py::ljust("decision", 28) << py::ljust("by", 14)
              << py::rjust("rounds", 6) << "  the call" << std::endl;
    for (const auto& [name, r] : rows) {
        std::cout << py::ljust(name, 20) << py::ljust(cases::CASES().at(name).level, 16)
                  << py::ljust(r.status + ": " + r.choice, 28) << py::ljust(r.decided_by, 14)
                  << py::rjust(std::to_string(r.rounds), 6) << "  " << (r.upheld ? "upheld" : "overturned") << std::endl;
    }
    return 0;
}

}  // namespace council
