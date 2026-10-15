#include "review.hpp"

#include <signal.h>

#include <algorithm>
#include <iostream>
#include <regex>

#include "cli.hpp"
#include "council.hpp"
#include "evidence.hpp"
#include "intake.hpp"
#include "library.hpp"

namespace review {

using council::HERE;

void show(const cases::Case& c) {
    Dict<library::Rule> rules;
    for (const auto& rulebook : c.rulebooks())
        for (const auto& [id, rule] : library::load_rulebook(rulebook)) rules[id] = rule;
    std::cout << "\n" << c.title << " (" << c.sport << ")\n" << c.incident << "\n";
    std::cout << "The call: " << c.officials << " ruled " << c.call << ": " << c.rulings.at(c.call)
              << (c.call_reason.empty() ? "" : " (" + c.call_reason + ")") << "\n";
    std::cout << "Possible rulings:\n";
    for (const auto& [rid, text] : c.rulings) std::cout << "  " << rid << ": " << text << "\n";
    std::cout << "Parties (their side's preferences, best first; each starts the discussion on its first choice):\n";
    for (const auto& a : c.parties) {
        auto order = a.likes.keys();
        std::stable_sort(order.begin(), order.end(), [&](const auto& x, const auto& y) { return a.likes.at(x) > a.likes.at(y); });
        std::cout << "  " << a.name << ": " << py::join(order, ", ") << "\n";
    }
    std::cout << "Established facts (every advocate treats these as true, so check them):\n";
    for (const auto& [fid, text] : c.facts) std::cout << "  " << fid << ": " << text << "\n";
    std::cout << "Disputed points:\n";
    for (const auto& [did, text] : c.disputed) std::cout << "  " << did << ": " << text << "\n";
    std::cout << "Rules (" << py::join(c.rulebooks(), ", ") << "):\n";
    for (const auto& rid : c.rules) std::cout << "  " << rid << ": " << rules.at(rid).title << "\n";
    if (!c.frames.empty())
        std::cout << "The neutral panel will look at the clip's frames (" << c.frames.size() << " in "
                  << fs::path(c.frames[0]).parent_path().string() << ").\n";
    std::cout << std::flush;
}

Prepared prepare(const std::optional<fs::path>& video, const std::string& context, const std::string& name,
                 const llm::Chat& chat, const llm::Chat& look, std::optional<double> at, double window, int count,
                 Log log, Log step) {
    if (!log) log = print_line;
    if (!step) step = [](const std::string&) {};
    fs::path folder = HERE / "cases" / name;
    std::vector<evidence::Observation> observed;
    if (video) {
        step("watching");
        log("Watching " + video->filename().string() + " with " + llm::VISION_MODEL);
        for (const auto& old : glob(folder / "frames", "frame_", ".jpg")) fs::remove(old);  // from an earlier run
        observed = evidence::watch(*video, context, look, folder / "frames", count, at, window, log);
    }
    step("building");
    log("Building the case with " + llm::MODEL);
    intake::Intake builder(chat, context, observed, log);
    cases::Case c = builder.build(video ? "from video" : "from description");
    if (video) {
        c.video = video->string();
        c.frames.clear();
        for (const auto& f : glob(folder / "frames", "frame_", ".jpg")) c.frames.push_back(f.lexically_relative(HERE).string());
    }
    fs::path path = folder / "case.json";
    std::vector<std::string> evidence_lines;
    for (const auto& o : observed) evidence_lines.push_back(o.str());
    cases::save_case(c, path, json::Object{{"context", context}, {"evidence", evidence_lines}, {"warnings", builder.warnings}});
    return {c, path, builder.warnings};
}

namespace {

const char* DESCRIPTION = R"(Review a contested call from a video. The agents watch it, work out the incident and who the parties are, find
the rules in rules/ and take their starting positions. Then the advocates discuss it, and a neutral panel, who watch
the clip themselves, decide it.

    build/review clip.mp4 --context "Northgate v Eastport, 88th minute. The referee allowed the goal ..."
    build/review clip.mp4 --at 0:42 --context "..."   # look closely around 0:42
    build/review --context "..."                      # no video: build the case from your description
    build/council --case-file cases/<name>/case.json  # debate a saved case again

Before the debate it stops so you can check the case it built: cases/<name>/case.json, with the frames the vision
model looked at in cases/<name>/frames/. Fix anything that's wrong in case.json, then press Enter. --yes skips
the pause.)";

void on_interrupt(int) {}

// input(): false on Ctrl-C or end of input, like Python's KeyboardInterrupt and EOFError.
bool wait_for_enter(const std::string& prompt) {
    std::cout << prompt << std::flush;
    struct sigaction quiet {}, before {};
    quiet.sa_handler = on_interrupt;  // no SA_RESTART, so Ctrl-C interrupts the read instead of killing the program
    sigemptyset(&quiet.sa_mask);
    sigaction(SIGINT, &quiet, &before);
    std::string line;
    bool ok = static_cast<bool>(std::getline(std::cin, line));
    sigaction(SIGINT, &before, nullptr);
    std::cin.clear();
    return ok;
}

}  // namespace

int main(const std::vector<std::string>& args) {
    std::vector<std::string> panel_choices;
    for (size_t i = 0; i <= council::PANEL.size(); ++i) panel_choices.push_back(std::to_string(i));
    cli::Parser ap("review", DESCRIPTION);
    ap.positional("video", "the clip to review")
        .option("--context", "what you know: competition, teams, players, score, and the call")
        .option("--at", "when the incident happens in the clip, like 0:42")
        .option("--window", "seconds either side of --at to look at (default 3)", "3.0", "float")
        .option("--frames", "how many frames the vision model looks at (default 12)", "12", "int")
        .option("--name", "folder for the case under cases/ (default: the clip's name)")
        .option("--rounds", py::format("most rounds of discussion (default %d)", council::MAX_ROUNDS),
                std::to_string(council::MAX_ROUNDS), "int")
        .option("--panel", py::format("neutral panel members who decide (default %zu; 0: the advocates decide)", council::PANEL.size()),
                std::to_string(council::PANEL.size()), "int", panel_choices)
        .flag("--yes", "start the discussion without pausing to check the case");
    ap.parse(args);
    std::optional<fs::path> video;
    if (!ap.str("video").empty()) video = fs::path(ap.str("video"));
    std::string context = ap.str("--context");
    if (!video && context.empty()) ap.error("give a video, --context, or both");
    std::error_code ec;
    if (video && !fs::exists(*video, ec)) ap.error(video->string() + " doesn't exist");
    std::optional<double> at;
    if (!ap.str("--at").empty()) {
        try {
            at = evidence::seconds(ap.str("--at"));
        } catch (const ValueError&) {
            ap.error("argument --at: write the time like 0:42 or 42.5, not " + py::repr(ap.str("--at")));
        }
    }
    int panel = ap.integer("--panel");

    std::string raw_name = !ap.str("--name").empty() ? ap.str("--name")
                           : video                   ? video->stem().string()
                                                     : strftime_now("case_%Y%m%d_%H%M%S");
    std::string name = py::strip(std::regex_replace(raw_name, std::regex("[^A-Za-z0-9_-]+"), "_"), "_");
    fs::path folder = HERE / "cases" / name;
    llm::Chat chat = llm::connect(llm::MODEL);
    llm::Chat look = video || panel ? llm::connect(llm::VISION_MODEL) : nullptr;
    Prepared prepared;
    try {
        prepared = prepare(video, context, name, chat, look, at, ap.number("--window"), ap.integer("--frames"));
    } catch (const intake::IntakeError& e) {
        std::cerr << "Couldn't build the case: " << e.what() << std::endl;
        return 1;
    }
    cases::Case c = prepared.c;
    show(c);
    for (const auto& warning : prepared.warnings) std::cout << "CHECK: " << warning << "\n";
    std::string saved = prepared.path.lexically_relative(HERE).string();
    std::cout << "\nSaved to " << saved << std::endl;
    if (!ap.on("--yes")) {
        if (!wait_for_enter("Check it, edit case.json if anything is wrong, then press Enter to start the discussion "
                            "(Ctrl-C to stop): ")) {
            std::cout << "\nStopped. Run the discussion later with: build/council --case-file " << saved << std::endl;
            return 0;
        }
        c = cases::load_case(prepared.path);
    }
    std::cout << std::endl;
    council::debate(name, c, chat, ap.integer("--rounds"), folder / "transcript.md", council::SAVED_HEADER, look, panel);
    return 0;
}

}  // namespace review
