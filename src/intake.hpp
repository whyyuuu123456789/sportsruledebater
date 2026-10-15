// Builds a case from what the user says and what the video shows, in five small model calls the code checks:
//
//   1. understand  the sport, the incident, the call and who made it, and every party the call affects
//   2. rules       pick the sport's rulebooks in rules/, shortlist clauses by keyword search, let the model choose
//   3. rulings     the call, plus the other rulings those rules allow
//   4. facts       established facts (said by the user, or plainly visible) and disputed points (everything that
//                  turns on judgement). Each fact keeps its source, so people can check it.
//   5. positions   each party's advocate ranks the rulings for its side: where it starts the discussion

#pragma once

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "cases.hpp"
#include "evidence.hpp"
#include "json.hpp"
#include "library.hpp"
#include "llm.hpp"

namespace intake {

extern const std::string SYSTEM;
constexpr size_t SHORTLIST = 15;

// The case can't be built without more from the user.
struct IntakeError : std::runtime_error { using std::runtime_error::runtime_error; };

std::string slug(const std::string& text);
std::string slug(const json::Value& value);
std::string text(const json::Value& value);  // a model's answer as one string, even when it sent a list
std::string snippet(const std::string& text, size_t words);

struct Party {
    std::string name, wants, stake;
};

struct Info {
    std::string sport, title, incident, officials, call, call_reason;
    std::vector<Party> parties;
    std::vector<std::string> rulebooks;
};

class Intake {
public:
    Intake(llm::Chat chat, const std::string& context, std::vector<evidence::Observation> evidence, Log log = nullptr);

    json::Value ask(const std::string& user, int max_tokens, const std::string& system = SYSTEM);  // always an object
    std::string known() const;

    Info understand();                                                                     // 1
    Dict<library::Rule> find_rules(const Info& info);                                      // 2
    std::pair<Dict<std::string>, std::string> find_rulings(const Info& info, const Dict<library::Rule>& rules);  // 3
    std::pair<Dict<std::string>, Dict<std::string>> find_facts(const Info& info, const Dict<std::string>& rulings,
                                                               const Dict<library::Rule>& rules);  // 4
    std::vector<cases::Advocate> take_positions(const Info& info, const Dict<std::string>& rulings);  // 5
    cases::Case build(const std::string& level);

    llm::Chat chat;
    std::string context;
    std::vector<evidence::Observation> evidence;
    Log log;
    int calls = 0;
    std::vector<std::string> warnings;  // things the user should check before the debate
};

}  // namespace intake
