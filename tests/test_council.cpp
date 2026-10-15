#include <map>
#include <regex>
#include <set>

#include "council.hpp"
#include "test.hpp"

namespace {

using Prompts = std::vector<std::pair<std::string, std::string>>;  // (system, user)
// replies[advocate] = (its turns, one per round, the last one repeating; its closing vote)
using Replies = std::map<std::string, std::pair<std::vector<std::string>, std::string>>;

// A stand-in model for the advocates.
llm::Chat scripted(Replies replies, Prompts* prompts = nullptr) {
    return [replies, prompts](const std::string& system, const std::string& user, int, int, const llm::Images&,
                              bool) -> std::string {
        static const std::regex NAME("advocate for (.+?) on a sports rules council"), ROUND(R"(Round (\d+) of)");
        if (prompts) prompts->emplace_back(system, user);
        std::smatch m;
        std::regex_search(system, m, NAME);
        const auto& [turns, vote] = replies.at(m[1].str());
        if (user.find("Cast your final vote") != std::string::npos) return vote;
        std::smatch r;
        if (turns.size() == 1 || !std::regex_search(user, r, ROUND)) return turns.front();
        return turns[std::min(static_cast<size_t>(std::stoi(r[1].str()) - 1), turns.size() - 1)];
    };
}

void quiet(const std::string&) {}

council::Council make(const std::string& name, Replies replies, int rounds = 4, Prompts* prompts = nullptr,
                      std::function<void(cases::Case&)> change = nullptr) {
    cases::Case c = cases::CASES().at(name);
    if (change) change(c);
    return council::Council(name, c, scripted(std::move(replies), prompts), rounds, quiet);
}

std::string say(const std::string& position, const std::string& message = "I stand by that.") {
    return "MESSAGE: " + message + "\nPOSITION: " + position;
}

std::string vote(const std::string& position, const std::string& reason) {
    return "REASON: " + reason + "\nPOSITION: " + position;
}

std::set<std::string> key_set(const Dict<double>& d) {
    auto k = d.keys();
    return {k.begin(), k.end()};
}

std::set<std::string> key_set(const Dict<std::string>& d) {
    auto k = d.keys();
    return {k.begin(), k.end()};
}

bool any_line(const std::vector<std::string>& lines, const std::string& part) {
    for (const auto& l : lines)
        if (l.find(part) != std::string::npos) return true;
    return false;
}

}  // namespace

TEST(CaseTests, every_case_is_well_formed) {
    static const std::regex FACT(R"(F\d+)"), POINT(R"(D\d+)");
    for (const auto& [name, c] : cases::CASES()) {
        CHECK(c.rulings.contains(c.call));
        for (const auto& [f, _] : c.facts) CHECK(std::regex_match(f, FACT));
        for (const auto& [d, _] : c.disputed) CHECK(std::regex_match(d, POINT));
        auto book = library::load_rulebook(std::get<std::string>(c.rulebook));
        for (const auto& r : c.rules) CHECK(book.contains(r));
        for (const auto& a : c.advocates()) CHECK_EQ(key_set(a.likes), key_set(c.rulings));
        CHECK(c.parties.size() >= 2);
    }
}

TEST(CaseTests, rulebook_reads_ids_titles_sources_and_text) {
    auto rule = library::load_rulebook("soccer").at("LOTG-11.2");
    CHECK_EQ(rule.title, "Offside offence");
    CHECK_EQ(rule.source, "Law 11 (Offside), section 2");
    CHECK_IN("deliberate save", rule.text);
}

TEST(DiscussionTests, speakers_hear_everything_said_so_far_including_this_round) {
    Prompts prompts;
    Replies replies = {{"Game officials", {{say("no_basket", "NBA-TIME.1 decides it.")}, vote("no_basket", "Too late.")}},
                       {"Lakeside Lynx", {{say("basket_counts", "It was one motion.")}, vote("basket_counts", "One motion.")}},
                       {"Metro Monarchs", {{say("no_basket", "The clock hit zero.")}, vote("no_basket", "Too late.")}}};
    make("point_two", replies, 2, &prompts).run();
    std::vector<std::pair<std::string, std::string>> turns;
    for (const auto& [s, u] : prompts)
        if (u.find("MESSAGE:") != std::string::npos)
            turns.emplace_back(py::removeprefix(s.substr(0, s.find(" on a sports")), "You are the advocate for "), u);
    Dict<std::string> first;  // round 1, in speaking order
    for (size_t i = 0; i < 3; ++i) first[turns[i].first] = turns[i].second;
    CHECK_IN("Nobody has spoken yet.", first.at("Game officials"));
    CHECK_IN("Open by explaining the call (no_basket)", first.at("Game officials"));
    CHECK_IN("Open by making the case for basket_counts", first.at("Lakeside Lynx"));
    CHECK_IN("- Game officials (backs no_basket): NBA-TIME.1 decides it.", first.at("Lakeside Lynx"));
    CHECK_IN("- Lakeside Lynx (backs basket_counts): It was one motion.", first.at("Metro Monarchs"));
    std::string officials_again;
    for (size_t i = 3; i < turns.size() && officials_again.empty(); ++i)
        if (turns[i].first == "Game officials") officials_again = turns[i].second;
    CHECK_IN("- You (backing no_basket): NBA-TIME.1 decides it.", officials_again);
}

TEST(DiscussionTests, being_persuaded_ends_the_discussion_in_consensus) {
    Replies replies = {{"Game officials", {{say("no_basket")}, vote("no_basket", "")}},
                       {"Lakeside Lynx", {{say("basket_counts"), say("no_basket", "Fair point, Monarchs: F3 settles it.")},
                                          vote("no_basket", "")}},
                       {"Metro Monarchs", {{say("no_basket")}, vote("no_basket", "")}}};
    auto c = make("point_two", replies);
    auto r = c.run();
    CHECK_EQ(r.status, "CONSENSUS");
    CHECK_EQ(r.choice, "no_basket");
    CHECK_EQ(r.rounds, 2);
    CHECK(r.upheld);
    std::vector<council::Turn> lynx;
    for (const auto& t : c.turns)
        if (t.speaker == "Lakeside Lynx") lynx.push_back(t);
    CHECK_EQ(lynx.back().before, "basket_counts");
    CHECK_EQ(lynx.back().position, "no_basket");
}

TEST(DiscussionTests, without_consensus_a_closing_vote_decides) {
    Replies replies = {
        {"Referee & VAR", {{say("pen_red")}, vote("pen_yellow", "He tried to play the ball, so it's a caution.")}},
        {"Riverside FC", {{say("pen_red")}, vote("pen_red", "He denied a clear chance.")}},
        {"Harbor City", {{say("no_foul")}, vote("pen_yellow", "A caution at most.")}}};
    auto r = make("dogso_card", replies).run();
    CHECK_EQ(r.status, "VOTE");
    CHECK_EQ(r.choice, "pen_yellow");
    CHECK(!r.upheld);
    CHECK_EQ(r.tally, (Dict<int>{{"pen_yellow", 2}, {"pen_red", 1}}));
    CHECK_EQ(r.dissent, std::vector<std::string>{"Riverside FC (pen_red): He denied a clear chance."});
    CHECK_EQ(r.rounds, 3);  // nobody moved in round 3, so the discussion ended before round 4
}

TEST(DiscussionTests, a_tied_vote_goes_the_officials_way) {
    Replies replies = {{"Rules Committee", {{say("two_strokes")}, vote("two_strokes", "She caused it.")}},
                       {"Dana Hale", {{say("no_penalty")}, vote("no_penalty", "The wind moved it.")}},
                       {"Lucia Ortiz", {{say("two_strokes")}, vote("two_strokes", "The replay is clear.")}},
                       {"Ebba Lindqvist", {{say("two_strokes")}, vote("no_penalty", "It isn't 95% likely.")}}};
    auto r = make("moved_ball", replies).run();
    CHECK_EQ(r.status, "TIE");
    CHECK_EQ(r.choice, "two_strokes");
}

TEST(DiscussionTests, a_tie_the_officials_didnt_vote_for_leaves_the_call_standing) {
    cases::Advocate ruiz{"Ana Ruiz", "You represent Ana Ruiz.", {{"two_strokes", 1.0}, {"one_stroke", 0.5}, {"no_penalty", 0.0}}};
    Replies replies = {{"Rules Committee", {{say("two_strokes")}, vote("one_stroke", "A middle way.")}},
                       {"Dana Hale", {{say("no_penalty")}, vote("no_penalty", "The wind.")}},
                       {"Lucia Ortiz", {{say("two_strokes")}, vote("two_strokes", "The replay.")}},
                       {"Ebba Lindqvist", {{say("two_strokes")}, vote("no_penalty", "Doubt.")}},
                       {"Ana Ruiz", {{say("two_strokes")}, vote("two_strokes", "The replay.")}}};
    auto c = make("moved_ball", replies, 4, nullptr, [&](cases::Case& k) { k.parties.push_back(ruiz); });
    auto r = c.run();
    CHECK_EQ(r.status, "TIE");
    CHECK_EQ(r.choice, "two_strokes");
    CHECK(any_line(c.lines, "so the call stands"));
}

TEST(DiscussionTests, citations_outside_the_case_are_struck) {
    Replies replies;
    for (const char* n : {"Referee & VAR", "Riverside FC", "Harbor City"})
        replies[n] = {{say("no_foul", "Under LOTG-12.3 and LOTG-99.1, F2 and F9 show it.")}, vote("no_foul", "")};
    auto c = make("dogso_card", replies, 1);
    CHECK_EQ(c.speak(c.advocates[2], 0).struck, (std::vector<std::string>{"LOTG-99.1", "F9"}));
}

TEST(DiscussionTests, a_turn_without_a_position_keeps_the_last_one) {
    Replies replies;
    for (const char* n : {"Referee & VAR", "Northgate United", "Eastport Athletic"})
        replies[n] = {{"MESSAGE: I'm not sure yet."}, vote("goal", "")};
    auto c = make("deflection_offside", replies, 1);
    auto turn = c.speak(c.advocates[2], 0);  // Eastport starts out backing no_goal
    CHECK_EQ(turn.position, "no_goal");
    CHECK_EQ(turn.note, "no valid POSITION line, so its position is unchanged");
}

TEST(DiscussionTests, replies_are_parsed_in_any_reasonable_shape) {
    using P = std::pair<std::string, std::string>;
    CHECK_EQ(council::parse("MESSAGE: My position is firm.\n**POSITION:** pen_yellow"), (P{"My position is firm.", "pen_yellow"}));
    CHECK_EQ(council::parse("Fair enough.\nPosition - NO_FOUL"), (P{"Fair enough.", "no_foul"}));
    CHECK_EQ(council::parse("No position line here."), (P{"No position line here.", ""}));
}

namespace {

struct Call {
    std::string panelist, user;
    size_t images;
};

// A stand-in vision model for the panel. votes[panelist] = the ruling it votes for.
llm::Chat panel_model(std::map<std::string, std::string> votes, std::vector<Call>* calls) {
    return [votes, calls](const std::string& system, const std::string& user, int, int, const llm::Images& images,
                          bool) -> std::string {
        static const std::regex NAME(R"(^You are (Panelist \w))");
        std::smatch m;
        std::regex_search(system, m, NAME);
        std::string name = m[1].str();
        calls->push_back({name, user, images.size()});
        if (user.find("Before hearing the advocates") != std::string::npos) return "NOTES: " + name + " sees the defender stretch.";
        return "REASON: " + name + " went by the clip.\nPOSITION: " + votes.at(name);
    };
}

struct Reviewed {
    council::Council c;
    council::Result r;
};

Reviewed review(std::map<std::string, std::string> votes, std::vector<Call>& calls, int panel = 3, Prompts* prompts = nullptr) {
    Replies advocates = {{"Referee & VAR", {{say("goal", "Reyes chose to play it.")}, ""}},
                         {"Northgate United", {{say("goal", "He had two seconds.")}, ""}},
                         {"Eastport Athletic", {{say("no_goal", "It was a reflex.")}, ""}}};
    test::TempDir frames;
    cases::Case c = cases::CASES().at("deflection_offside");
    for (double t : {41.5, 42.0}) {
        fs::path p = frames.path / py::format("frame_%07.2f.jpg", t);
        write_file(p, "\xff\xd8 a still");
        c.frames.push_back(p.string());
    }
    council::Council council("t", c, scripted(advocates, prompts), 2, quiet, panel_model(votes, &calls), panel);
    auto r = council.run();
    return {council, r};
}

}  // namespace

TEST(PanelTests, the_panel_watches_the_clip_then_hears_the_discussion_and_decides) {
    Prompts prompts;
    std::vector<Call> calls;
    auto [c, r] = review({{"Panelist A", "no_goal"}, {"Panelist B", "goal"}, {"Panelist C", "no_goal"}}, calls, 3, &prompts);
    std::vector<Call> looks;
    for (const auto& call : calls)
        if (call.user.find("Before hearing the advocates") != std::string::npos) looks.push_back(call);
    std::vector<std::string> names;
    for (const auto& l : looks) names.push_back(l.panelist);
    CHECK_EQ(names, (std::vector<std::string>{"Panelist A", "Panelist B", "Panelist C"}));
    for (const auto& call : calls) CHECK_EQ(call.images, size_t{2});  // every panel call sees the frames
    CHECK_IN("0:41.5, 0:42.0", looks[0].user);
    std::string vote_a;
    for (const auto& call : calls)
        if (call.panelist == "Panelist A" && call.user.find("Now vote") != std::string::npos) vote_a = call.user;
    CHECK_IN("Panelist A sees the defender stretch.", vote_a);  // its own notes from the clip...
    CHECK_NOT_IN("Panelist B sees", vote_a);                    // ...and nobody else's
    CHECK_IN("- Eastport Athletic (backs no_goal): It was a reflex.", vote_a);
    CHECK_IN("THEIR CLOSING STATEMENTS:", vote_a);
    CHECK_IN("a neutral panel, who also watch the clip, decides it", prompts[0].first);
    CHECK_EQ(r.status, "VOTE");
    CHECK_EQ(r.choice, "no_goal");
    CHECK_EQ(r.decided_by, "panel");
    CHECK(!r.upheld);
    CHECK_EQ(r.positions, (Dict<std::string>{{"Referee & VAR", "goal"}, {"Northgate United", "goal"}, {"Eastport Athletic", "no_goal"}}));
    CHECK_EQ(r.dissent, std::vector<std::string>{"Panelist B (goal): Panelist B went by the clip."});
}

TEST(PanelTests, a_tied_panel_goes_with_the_advocates_final_positions) {
    std::vector<Call> calls;
    auto [c, r] = review({{"Panelist A", "no_goal"}, {"Panelist B", "goal"}}, calls, 2);
    CHECK_EQ(r.status, "TIE");
    CHECK_EQ(r.choice, "goal");  // two advocates ended up backing goal
    CHECK(any_line(c.lines, "the advocates' final positions break the tie"));
}
