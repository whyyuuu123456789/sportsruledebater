// Contested calls for the sports rules council to discuss. Add your own by copying one in cases.cpp.
//
// Every case has:
//   call        the ruling the officials made; one advocate defends it
//   rulings     every ruling the council may reach, including the call
//   facts       established facts everyone accepts (F1, F2, ...)
//   disputed    points the sides disagree on (D1, D2, ...)
//   rulebook    which rulebook in rules/ to read (or a list of them), and rules: the clauses (by ID) the
//               advocates may cite
//   parties     one advocate per party involved, as many as there are
//   frames      stills from the clip, if the case came from one: the neutral panel looks at them
//
// For each advocate, likes says how good each ruling is for its side, from 0 (worst) to 1 (best). It sets the
// advocate's starting position and tells it what its side wants; where it ends up depends on the discussion.

#pragma once

#include <string>
#include <variant>
#include <vector>

#include "json.hpp"
#include "library.hpp"
#include "util.hpp"

namespace cases {

using library::Rule;

struct Advocate {
    std::string name;
    std::string side;     // who it speaks for and what's at stake, in the advocate's voice
    Dict<double> likes;   // how good each ruling is for its side: its starting position and preferences
    bool operator==(const Advocate&) const = default;
};

// One rulebook's name, or a list of them.
using Rulebook = std::variant<std::string, std::vector<std::string>>;

struct Case {
    std::string sport;
    std::string level;        // a few words on what kind of dispute it is
    std::string title;
    std::string incident;
    std::string officials;    // who made the call; also the name of the advocate who defends it
    std::string call;
    std::string call_reason;
    Dict<std::string> rulings;
    Dict<std::string> facts;
    Dict<std::string> disputed;
    Rulebook rulebook;
    std::vector<std::string> rules;
    std::vector<Advocate> parties;
    std::string note;
    std::string officials_side;       // overrides the officials' advocate's default brief
    std::string video;                // the clip the case came from, if any
    std::vector<std::string> frames;  // stills from the clip for the panel (paths from the project folder)

    Advocate officials_advocate() const;
    std::vector<Advocate> advocates() const;
    std::vector<std::string> rulebooks() const;
    bool operator==(const Case&) const = default;
};

json::Value to_json(const Case& c);  // dataclasses.asdict, in the same field order
// Writes a case as JSON. Extra keys (like the video evidence) are kept in the file for people to read.
void save_case(const Case& c, const fs::path& path, const json::Object& extra = {});
Case load_case(const fs::path& path);
// A case from its JSON form. Unknown keys are ignored; a missing or inconsistent field raises ValueError.
Case case_from_dict(const json::Value& data);

// The demo cases, in order.
const Dict<Case>& CASES();

}  // namespace cases
