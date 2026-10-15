// Review a contested call from a video. The agents watch it, work out the incident and who the parties are, find
// the rules in rules/ and take their starting positions. Then the advocates discuss it, and a neutral panel, who watch
// the clip themselves, decide it.
//
// Before the debate it stops so you can check the case it built: cases/<name>/case.json, with the frames the vision
// model looked at in cases/<name>/frames/. Fix anything that's wrong in case.json, then press Enter. --yes skips
// the pause.

#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "cases.hpp"
#include "llm.hpp"
#include "util.hpp"

namespace review {

void show(const cases::Case& c);

struct Prepared {
    cases::Case c;
    fs::path path;
    std::vector<std::string> warnings;
};

// Watches the clip (if there is one), builds the case and saves it to cases/<name>/case.json. step(stage) is told
// when a stage starts: "watching", then "building". Raises IntakeError when the case can't be built without more
// from the user.
Prepared prepare(const std::optional<fs::path>& video, const std::string& context, const std::string& name,
                 const llm::Chat& chat, const llm::Chat& look, std::optional<double> at = std::nullopt,
                 double window = 3.0, int count = 12, Log log = nullptr, Log step = nullptr);

int main(const std::vector<std::string>& args);

}  // namespace review
