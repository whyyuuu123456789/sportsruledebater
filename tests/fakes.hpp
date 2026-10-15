// A stand-in model for the case builder (JSON), the council's advocates and the neutral panel, shared by the review
// and website tests.

#pragma once

#include <string>

#include "json.hpp"
#include "llm.hpp"

inline const json::Value& understand_answer() {
    static const json::Value answer = json::Value::parse(R"({
        "sport": "soccer", "rulebooks": ["soccer"], "title": "Late goal after a defender's touch",
        "incident": "Eastport's defender touched a long ball, which reached Northgate's striker, who scored.",
        "officials": "Referee & VAR", "call": "Goal allowed", "call_reason": "The defender played the ball.",
        "parties": [{"name": "Northgate United", "stake": "Leads 2-1 if the goal stands."},
                    {"name": "Eastport Athletic", "stake": "Stays 1-1 if it is disallowed."}]})");
    return answer;
}

inline llm::Chat model(json::Value understand = understand_answer(), json::Value rulings = nullptr) {
    return [understand, rulings](const std::string& system, const std::string& user, int, int, const llm::Images&,
                                 bool) -> std::string {
        auto has = [&](const char* words) { return user.find(words) != std::string::npos; };
        auto starts = [&](const char* words) { return system.rfind(words, 0) == 0; };
        if (has("RULEBOOKS AVAILABLE")) return understand.dump();
        if (has("CANDIDATE CLAUSES")) return R"({"rules": ["[LOTG-11.2]", "LOTG-11.DP", "LOTG-99.9", "LOTG-11.4"]})";
        if (has("List the rulings"))
            return !rulings.is_null() ? rulings.dump()
                                      : R"({"rulings": [{"id": "Goal Stands", "text": "The goal stands: 2-1"},
                                                        {"id": "no_goal", "text": "Offside: still 1-1"}],
                                            "call_id": "goal_stands"})";
        if (has("Split what is known"))
            return R"({"facts": [{"text": "The striker was in an offside position.", "source": "context"},
                                 {"text": "The defender stretched for the ball.", "source": "video 0:41.5"}],
                       "disputed": [{"text": "Whether the touch was a deliberate play."}]})";
        if (has("Rank every ruling"))
            return std::string(R"({"ranking": [")") + (py::contains(system, "Northgate") ? "goal_stands" : "no_goal") + "\"]}";
        if (starts("You are Panelist")) {  // the neutral panel
            if (has("Before hearing the advocates")) return "NOTES: A defender stretches for the ball.";
            std::string vote = starts("You are Panelist C") ? "no_goal" : "goal_stands";
            return "REASON: The touch looked deliberate to me.\nPOSITION: " + vote;
        }
        std::string mine = starts("You are the advocate for Northgate") || starts("You are the advocate for Referee")
                               ? "goal_stands"
                               : "no_goal";
        if (has("MESSAGE:")) return "MESSAGE: LOTG-11.DP and F2.\nPOSITION: " + mine;  // a turn in the discussion
        return "REASON: A judgment call.\nPOSITION: " + mine;                          // the closing vote
    };
}
