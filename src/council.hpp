// Sports rules council: advocates for the officials and for every party discuss a contested call, and a neutral
// panel decides it.
//
// One advocate defends the call the officials made, and every party involved, however many there are, gets its own
// advocate. They hold an actual discussion: they speak in turn, each one seeing everything said so far, answer each
// other by name, concede points, and change their minds when they're persuaded. After every turn the speaker says
// which ruling it backs now. When the discussion ends, each advocate gives the panel a closing statement.
//
// The neutral panel decides. Its members have no side, and they're played by a different model from the advocates,
// one that can see images. When the case comes from a video, each member watches the clip before the discussion and
// writes down what it sees, so its view of the evidence comes from the clip and not from either side. After the
// discussion, each member looks at the frames again and votes on its own:
//
//   CONSENSUS   the panel is unanimous
//   VOTE        the panel's majority wins; the dissent is recorded
//   TIE         the panel is split evenly: the advocates' final positions break the tie, and failing that the call
//               stands
//
// There's no answer key: the panel decides, and the transcript shows why. With --panel 0 the advocates decide by
// themselves instead: a consensus during the discussion, or else a closing vote among them.

#pragma once

#include <functional>
#include <regex>
#include <string>
#include <utility>
#include <vector>

#include "cases.hpp"
#include "json.hpp"
#include "llm.hpp"
#include "util.hpp"

namespace council {

constexpr int MAX_ROUNDS = 4;
constexpr size_t HISTORY = 16;      // how many of the latest turns a speaker is shown
constexpr size_t PANEL_FRAMES = 6;  // how many stills from the clip each panel member looks at
extern const std::vector<std::pair<std::string, std::string>> PANEL;  // (name, what it pays most attention to)
extern const std::string DEMO_HEADER;
extern const std::string SAVED_HEADER;

// Where cases/ and transcripts/ live. Tests point it at a temporary folder.
extern fs::path HERE;

std::string favorite(const cases::Advocate& a);

struct Turn {
    int round = 0;
    std::string speaker;
    std::string message;
    std::string before;    // the ruling the speaker backed going into the turn
    std::string position;  // and the one it backs after it
    std::vector<std::string> struck;
    std::string note;
};

struct Panelist {
    std::string name;
    std::string lens;   // what it pays most attention to
    std::string notes;  // what it saw in the clip before the discussion
};

// Splits a reply into what was said and the ruling on its 'POSITION:' line ("" if there isn't one).
std::pair<std::string, std::string> parse(const std::string& reply);

using Votes = Dict<std::pair<std::string, std::string>>;  // voter -> (ruling, reason); "" is no valid vote

struct Result {
    std::string status;  // CONSENSUS, VOTE or TIE
    std::string choice;
    Dict<int> tally;
    Votes votes;
    std::vector<std::string> dissent;
    std::string how;
    std::string decided_by;  // "panel" or "advocates"
    int rounds = 0;
    Dict<std::string> positions;
    bool upheld = false;
    Dict<std::string> notes;
    Dict<std::string> statements;
    std::vector<double> frames_seen;
    json::Value to_json() const;
};

using Out = std::function<void(const std::string&)>;

class Council {
public:
    // ask is the advocates' model; look, the panel's (no look: no panel).
    Council(std::string name, cases::Case c, llm::Chat ask, int rounds = MAX_ROUNDS, Out out = nullptr,
            llm::Chat look = nullptr, int panel = static_cast<int>(PANEL.size()));

    void out(const std::string& text = "");
    // Rule and fact IDs cited in text, split into (listed in this case, struck).
    std::pair<std::vector<std::string>, std::vector<std::string>> ids(const std::string& text) const;
    std::vector<std::pair<double, std::string>> load_frames() const;

    // ---- what everyone is told ----
    std::string brief() const;
    std::string system(const cases::Advocate& a) const;
    std::string panel_system(const Panelist& p) const;
    std::string discussion(const cases::Advocate* viewer) const;

    // ---- the advocates ----
    Turn speak(const cases::Advocate& a, int rnd);
    void closing_statements();
    Votes closing_vote();
    Result decide(const Votes& votes) const;

    // ---- the panel ----
    std::string first_look(Panelist& p);
    std::pair<std::string, std::string> panel_vote(const Panelist& p);
    Result panel_decide(const Votes& votes) const;

    Result run();

    std::string name;
    cases::Case case_;
    llm::Chat ask;
    int rounds;
    llm::Chat look;
    std::vector<cases::Advocate> advocates;  // the officials' advocate first
    Dict<library::Rule> rules;
    std::vector<Panelist> panel;
    std::vector<std::pair<double, std::string>> frames;  // (seconds into the clip, JPEG bytes)
    std::vector<std::string> lines;
    int calls = 0;
    Dict<std::string> position;            // where each advocate stands now
    std::vector<Turn> turns;               // the discussion, in order
    Dict<std::string> statements;          // closing statements to the panel

private:
    const cases::Advocate& officials() const { return advocates.front(); }
    std::string ask_(const cases::Advocate& a, const std::string& user, int max_tokens);
    std::string look_(const Panelist& p, const std::string& user, int max_tokens);
    Out print_;
    std::regex rule_id_;
};

// Runs the council on one case and saves the transcript. out gets each transcript line (default: print).
Result debate(const std::string& name, const cases::Case& c, const llm::Chat& ask, int rounds, const fs::path& transcript,
              const std::string& header, const llm::Chat& look = nullptr, int panel = static_cast<int>(PANEL.size()),
              Out out = nullptr);

int main(const std::vector<std::string>& args);

}  // namespace council
