#include "cases.hpp"

#include <set>

namespace cases {

Advocate Case::officials_advocate() const {
    std::string brief = officials_side.empty()
                            ? "You represent " + officials +
                                  ", who made the call. Explain and defend it with the rules and the facts, but officials "
                                  "care most about getting it right: if the discussion convinces you the call was wrong, "
                                  "say so."
                            : officials_side;
    Dict<double> likes;
    for (const auto& [r, _] : rulings) likes[r] = r == call ? 1.0 : 0.5;
    return Advocate{officials, brief, likes};
}

std::vector<Advocate> Case::advocates() const {
    std::vector<Advocate> all{officials_advocate()};
    all.insert(all.end(), parties.begin(), parties.end());
    return all;
}

std::vector<std::string> Case::rulebooks() const {
    if (const auto* one = std::get_if<std::string>(&rulebook)) return {*one};
    return std::get<std::vector<std::string>>(rulebook);
}

json::Value to_json(const Case& c) {
    json::Array parties;
    for (const auto& a : c.parties) parties.push_back(json::Object{{"name", a.name}, {"side", a.side}, {"likes", a.likes}});
    json::Value rulebook = std::holds_alternative<std::string>(c.rulebook)
                               ? json::Value(std::get<std::string>(c.rulebook))
                               : json::Value(std::get<std::vector<std::string>>(c.rulebook));
    return json::Object{
        {"sport", c.sport},       {"level", c.level},
        {"title", c.title},       {"incident", c.incident},
        {"officials", c.officials}, {"call", c.call},
        {"call_reason", c.call_reason}, {"rulings", c.rulings},
        {"facts", c.facts},       {"disputed", c.disputed},
        {"rulebook", rulebook},   {"rules", c.rules},
        {"parties", parties},     {"note", c.note},
        {"officials_side", c.officials_side}, {"video", c.video},
        {"frames", c.frames},
    };
}

void save_case(const Case& c, const fs::path& path, const json::Object& extra) {
    fs::create_directories(path.parent_path());
    json::Value data = to_json(c);
    for (const auto& [key, value] : extra) data[key] = value;
    write_file(path, data.dump(2, false) + "\n");
}

Case load_case(const fs::path& path) { return case_from_dict(json::Value::parse(read_file(path))); }

namespace {

[[noreturn]] void missing(const std::string& what) { throw ValueError("the case is missing something: " + what); }

const json::Value& field(const json::Value& data, const std::string& key) {
    if (!data.contains(key)) missing(py::repr(key));
    return data.get(key);
}

std::string text_field(const json::Value& data, const std::string& key, bool required = true) {
    if (!required && !data.contains(key)) return "";
    const json::Value& v = field(data, key);
    if (!v.is_string()) missing(py::repr(key) + " should be text");
    return v.as_string();
}

Dict<std::string> texts(const json::Value& data, const std::string& key) {
    const json::Value& v = field(data, key);
    if (!v.is_object()) missing(py::repr(key) + " should be an object");
    Dict<std::string> out;
    for (const auto& [k, item] : v.as_object()) {
        if (!item.is_string()) missing(py::repr(key) + " should map IDs to text");
        out[k] = item.as_string();
    }
    return out;
}

std::vector<std::string> list(const json::Value& data, const std::string& key, bool required = true) {
    if (!required && !data.contains(key)) return {};
    const json::Value& v = field(data, key);
    if (!v.is_array()) missing(py::repr(key) + " should be a list");
    std::vector<std::string> out;
    for (const auto& item : v.as_array()) {
        if (!item.is_string()) missing(py::repr(key) + " should be a list of text");
        out.push_back(item.as_string());
    }
    return out;
}

}  // namespace

Case case_from_dict(const json::Value& data) {
    if (!data.is_object()) missing("the case should be a JSON object");
    Case c;
    c.sport = text_field(data, "sport");
    c.level = text_field(data, "level");
    c.title = text_field(data, "title");
    c.incident = text_field(data, "incident");
    c.officials = text_field(data, "officials");
    c.call = text_field(data, "call");
    c.call_reason = text_field(data, "call_reason");
    c.rulings = texts(data, "rulings");
    c.facts = texts(data, "facts");
    c.disputed = texts(data, "disputed");
    const json::Value& rulebook = field(data, "rulebook");
    if (rulebook.is_string()) c.rulebook = rulebook.as_string();
    else c.rulebook = list(data, "rulebook");
    c.rules = list(data, "rules");
    const json::Value& parties = field(data, "parties");
    if (!parties.is_array()) missing("'parties' should be a list");
    for (const auto& p : parties.as_array()) {  // older case files also have concession and floor: ignored
        if (!p.is_object()) missing("each party should be an object");
        Advocate a{text_field(p, "name"), text_field(p, "side"), {}};
        const json::Value& likes = field(p, "likes");
        if (!likes.is_object()) missing("'likes' should be an object");
        for (const auto& [r, v] : likes.as_object()) {
            if (!v.is_number()) missing("'likes' should map rulings to numbers");
            a.likes[r] = v.as_number();
        }
        c.parties.push_back(a);
    }
    c.note = text_field(data, "note", false);
    c.officials_side = text_field(data, "officials_side", false);
    c.video = text_field(data, "video", false);
    c.frames = list(data, "frames", false);

    if (!c.rulings.contains(c.call)) throw ValueError("the call " + py::repr(c.call) + " isn't one of the rulings");
    auto keys = [](const auto& d) {
        auto k = d.keys();
        return std::set<std::string>(k.begin(), k.end());
    };
    for (const auto& a : c.parties)
        if (keys(a.likes) != keys(c.rulings)) throw ValueError(a.name + "'s preferences don't cover exactly the rulings");
    return c;
}

const Dict<Case>& CASES() {
    static const Dict<Case> cases = {
        {"point_two",
         Case{
             .sport = "Basketball (NBA rules)",
             .level = "clear-cut",
             .title = "A catch-and-shoot buzzer-beater with 0.2 seconds left",
             .incident = "Metro Monarchs lead the Lakeside Lynx 100-99 with 0.2 seconds left. The Lynx inbound to guard "
                         "Dre Carter, who catches the pass, turns and hits a jump shot as the horn sounds.",
             .officials = "Game officials",
             .call = "no_basket",
             .call_reason = "On the floor they counted the basket. After an instant replay review they waved it off: "
                            "with 0.2 seconds on the clock a player can't catch and shoot, and the clock hit 0.0 before "
                            "the release.",
             .rulings = {{"no_basket", "No basket: the Monarchs win 100-99"},
                         {"basket_counts", "The basket counts: the Lynx win 101-100"}},
             .facts =
                 {
                     {"F1", "The Lynx inbounded from the sideline with 0.2 seconds on the game clock, trailing 100-99."},
                     {"F2", "Carter caught the pass with both hands at the top of the key, turned, and released a jump "
                            "shot. It went in."},
                     {"F3", "The replay shows the clock started when Carter first touched the ball, and it reached 0.0 "
                            "before the ball left his hand."},
                     {"F4", "The officials signaled a made basket on the floor, then waved it off after the replay "
                            "review."},
                 },
             .disputed =
                 {
                     {"D1", "The Lynx say Carter's catch and release was one quick motion, closer to a tap than a "
                            "catch."},
                 },
             .rulebook = "basketball",
             .rules = {"NBA-TIME.1", "NBA-TIME.2", "NBA-REPLAY"},
             .parties =
                 {
                     Advocate{"Lakeside Lynx", "You represent the Lakeside Lynx. If Carter's shot counts, the Lynx win "
                                               "101-100.",
                              {{"basket_counts", 1.0}, {"no_basket", 0.3}}},
                     Advocate{"Metro Monarchs", "You represent the Metro Monarchs. If the basket stays waved off, the "
                                                "Monarchs win 100-99.",
                              {{"no_basket", 1.0}, {"basket_counts", 0.0}}},
                 },
         }},
        {"dogso_card",
         Case{
             .sport = "Soccer (IFAB Laws of the Game)",
             .level = "two questions",
             .title = "Penalty and red card for a sliding trip on the penalty-area line",
             .incident = "55th minute, 0-0. Riverside FC striker Leo Varga runs onto a through ball and is clean through "
                         "on goal. Harbor City defender Sam Okafor, chasing from the side, slides in, misses the ball, "
                         "and his trailing leg trips Varga.",
             .officials = "Referee & VAR",
             .call = "pen_red",
             .call_reason = "The referee awarded a penalty kick and sent Okafor off for denying an obvious goal-scoring "
                            "opportunity. The VAR checked and did not recommend a review.",
             .rulings = {{"pen_red", "Penalty kick to Riverside, and Okafor sent off (red card)"},
                         {"pen_yellow", "Penalty kick to Riverside, and Okafor cautioned (yellow card) instead of sent "
                                        "off"},
                         {"fk_red", "Direct free kick to Riverside just outside the penalty area, and Okafor sent off"},
                         {"no_foul", "No foul: play restarts with a dropped ball, and Okafor stays on with no card"}},
             .facts =
                 {
                     {"F1", "The VAR freeze-frame shows the trip (contact on Varga's standing foot) happened on the "
                            "penalty-area line."},
                     {"F2", "Okafor's slide was low and at normal speed, with his leading foot aimed at the ball. He "
                            "missed the ball by about 20 cm. He did not hold, pull or push Varga."},
                     {"F3", "When he was tripped, Varga was 11 m from goal, central, running toward goal with the ball "
                            "about a metre in front of him. Only the goalkeeper was between him and the goal; the "
                            "nearest other defender was 8 m behind."},
                 },
             .disputed =
                 {
                     {"D1", "Harbor City says the ball was running toward the goalkeeper, so Varga might not have kept "
                            "control. Riverside says he was about to shoot."},
                 },
             .rulebook = "soccer",
             .rules = {"LOTG-1.2", "LOTG-12.1", "LOTG-12.3", "LOTG-VAR"},
             .parties =
                 {
                     Advocate{"Riverside FC", "You represent Riverside FC, whose striker was tripped. A penalty plus a "
                                              "red card would leave Harbor City with ten men for 35 minutes.",
                              {{"pen_red", 1.0}, {"pen_yellow", 0.7}, {"fk_red", 0.4}, {"no_foul", 0.0}}},
                     Advocate{"Harbor City", "You represent Harbor City, whose defender Sam Okafor was sent off.",
                              {{"no_foul", 1.0}, {"fk_red", 0.6}, {"pen_yellow", 0.6}, {"pen_red", 0.0}}},
                 },
         }},
        {"deflection_offside",
         Case{
             .sport = "Soccer (IFAB Laws of the Game)",
             .level = "judgment call",
             .title = "Late goal after a defender's touch: deliberate play or deflection?",
             .incident = "88th minute, 1-1. Northgate United's goalkeeper launches a long kick upfield. Eastport "
                         "Athletic centre-back Tom Reyes stretches for the dropping ball and touches it; it loops "
                         "forward to Northgate striker Kai Moreno, who was in an offside position when the ball was "
                         "kicked. Moreno scores.",
             .officials = "Referee & VAR",
             .call = "goal",
             .call_reason = "The referee ruled that Reyes deliberately played the ball, so Moreno's offside position "
                            "doesn't count. The VAR found no clear and obvious error.",
             .rulings = {{"goal", "Goal stands: Northgate lead 2-1"},
                         {"no_goal", "Goal disallowed for offside: indirect free kick to Eastport, still 1-1"}},
             .facts =
                 {
                     {"F1", "When the goalkeeper kicked the ball, Moreno's torso and left foot were about 40 cm nearer "
                            "Eastport's goal line than the second-last defender, in Eastport's half: an offside "
                            "position."},
                     {"F2", "The ball travelled about 45 m in a high arc. Reyes watched it the whole way and had about "
                            "two seconds to adjust his position."},
                     {"F3", "Reyes stepped toward the ball and stretched his right leg at hip height. The ball came off "
                            "his shin and looped about 15 m forward to Moreno."},
                     {"F4", "Moreno controlled the ball and scored. No other Eastport player touched it."},
                 },
             .disputed =
                 {
                     {"D1", "Eastport says Reyes's touch was an instinctive stretch that achieved only limited contact "
                            "(a deflection), so Moreno's offside still counts. Northgate says Reyes had time and a clear "
                            "view, and tried to clear the ball (a deliberate play)."},
                     {"D2", "How fast the ball was dropping when it reached Reyes."},
                 },
             .rulebook = "soccer",
             .rules = {"LOTG-5.2", "LOTG-VAR", "LOTG-11.1", "LOTG-11.2", "LOTG-11.DP", "LOTG-11.4"},
             .parties =
                 {
                     Advocate{"Northgate United", "You represent Northgate United. Moreno's goal puts them ahead 2-1 in "
                                                  "the 88th minute.",
                              {{"goal", 1.0}, {"no_goal", 0.0}}},
                     Advocate{"Eastport Athletic", "You represent Eastport Athletic. If the goal is disallowed, the "
                                                   "match stays 1-1.",
                              {{"no_goal", 1.0}, {"goal", 0.0}}},
                 },
             .note = "It turns on a judgment call (D1): did the defender deliberately play the ball?",
         }},
        {"moved_ball",
         Case{
             .sport = "Golf (Rules of Golf)",
             .level = "four parties",
             .title = "A ball that moved at address decides a tournament: three players, one ruling",
             .incident = "Final round, 72nd hole. Leader Dana Hale grounds her putter behind her ball on the fringe, "
                         "and a second later the ball rolls a few centimetres. She says the wind moved it, plays it "
                         "from where it stopped, and holes the putt to win by one.",
             .officials = "Rules Committee",
             .call = "two_strokes",
             .call_reason = "After the round the Committee ruled from TV footage that Hale caused the ball to move. "
                            "Because she didn't replace it, she played from a wrong place, so it applied the two-stroke "
                            "general penalty.",
             .rulings = {{"two_strokes", "Two-stroke penalty: Hale finishes 8 under in third; Ortiz and Lindqvist go "
                                         "to a playoff"},
                         {"one_stroke", "One-stroke penalty: Hale finishes 9 under, in a three-way playoff with Ortiz "
                                        "and Lindqvist"},
                         {"no_penalty", "No penalty (natural forces moved the ball): Hale wins at 10 under"}},
             .facts =
                 {
                     {"F1", "Hale's ball lay on the fringe, about 15 cm off the putting green. No part of it touched the "
                            "putting green."},
                     {"F2", "Hale grounded her putter lightly about 2 cm behind the ball. The putter never touched the "
                            "ball."},
                     {"F3", "About one second after she grounded the putter, the ball rolled about 5 cm, down the "
                            "fringe's slope and away from the hole."},
                     {"F4", "The weather station on that hole logged gusts of 40 km/h (25 mph) at that minute."},
                     {"F5", "Hale told the walking official she believed the wind had moved the ball. She played it "
                            "from its new spot and holed the putt to finish 10 under par. Ortiz and Lindqvist had "
                            "already finished at 9 under."},
                     {"F6", "At normal speed, the TV footage shows the putter being grounded and the ball moving a "
                            "second later. Only a super-slow-motion replay shows the grass behind the ball flattening "
                            "as the putter was grounded."},
                 },
             .disputed =
                 {
                     {"D1", "Whether it is at least 95% likely that grounding the putter, rather than the wind and the "
                            "slope, made the ball move."},
                     {"D2", "Whether the Committee could rely on the super-slow-motion replay, or only on what could "
                            "be seen with the naked eye."},
                 },
             .rulebook = "golf",
             .rules = {"GOLF-KVC", "GOLF-PG", "GOLF-9.2b", "GOLF-9.3", "GOLF-9.4b", "GOLF-9.4", "GOLF-13.1d",
                       "GOLF-14.7a", "GOLF-20.2c"},
             .parties =
                 {
                     Advocate{"Dana Hale", "You represent Dana Hale. With no penalty she wins the tournament outright.",
                              {{"no_penalty", 1.0}, {"one_stroke", 0.5}, {"two_strokes", 0.0}}},
                     Advocate{"Lucia Ortiz", "You represent Lucia Ortiz, who finished 9 under. With the two-stroke "
                                             "penalty she's in a two-player playoff for the title; with one stroke it's "
                                             "a three-way playoff; with no penalty she finishes tied second.",
                              {{"two_strokes", 1.0}, {"one_stroke", 0.5}, {"no_penalty", 0.0}}},
                     Advocate{"Ebba Lindqvist", "You represent Ebba Lindqvist, who finished 9 under. With the "
                                                "two-stroke penalty she's in a two-player playoff for the title; with one "
                                                "stroke it's a three-way playoff; with no penalty she finishes tied "
                                                "second.",
                              {{"two_strokes", 1.0}, {"one_stroke", 0.5}, {"no_penalty", 0.0}}},
                 },
             .note = "Four advocates: the Rules Committee and three players whose places depend on the ruling.",
         }},
    };
    return cases;
}

}  // namespace cases
