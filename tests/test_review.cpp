#include <regex>
#include <set>

#include "cases.hpp"
#include "council.hpp"
#include "evidence.hpp"
#include "fakes.hpp"
#include "intake.hpp"
#include "library.hpp"
#include "review.hpp"
#include "test.hpp"

namespace {

// A one-page PDF with these lines of text (no parentheses).
std::string tiny_pdf(const std::vector<std::string>& lines) {
    std::string content = "BT /F1 10 Tf 40 760 Td 12 TL ";
    for (size_t i = 0; i < lines.size(); ++i) content += (i ? " (" : "(") + lines[i] + ") Tj T*";
    content += " ET";
    std::vector<std::string> objects = {
        "<< /Type /Catalog /Pages 2 0 R >>", "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >>",
        "<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content + "\nendstream",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"};
    std::string out = "%PDF-1.4\n";
    std::vector<size_t> offsets;
    for (size_t i = 0; i < objects.size(); ++i) {
        offsets.push_back(out.size());
        out += std::to_string(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }
    size_t xref = out.size();
    out += "xref\n0 " + std::to_string(objects.size() + 1) + "\n0000000000 65535 f \n";
    for (size_t o : offsets) out += py::format("%010zu 00000 n \n", o);
    return out + "trailer\n<< /Size " + std::to_string(objects.size() + 1) + " /Root 1 0 R >>\nstartxref\n" +
           std::to_string(xref) + "\n%%EOF\n";
}

const std::string FILLER = "the committee decides questions of fact on all the information reasonably available to it at the time";

void quiet(const std::string&) {}

std::set<std::string> ids_of(const std::vector<library::Rule>& rules) {
    std::set<std::string> ids;
    for (const auto& r : rules) ids.insert(r.id);
    return ids;
}

bool subset(const std::set<std::string>& small, const std::set<std::string>& big) {
    return std::includes(big.begin(), big.end(), small.begin(), small.end());
}

void make_clip(const fs::path& clip) {
    Completed done = run({"ffmpeg", "-v", "error", "-f", "lavfi", "-i", "testsrc=duration=3:size=320x240:rate=10",
                          "-pix_fmt", "yuv420p", clip.string()});
    if (done.code != 0) throw RuntimeError("ffmpeg couldn't make a test clip: " + done.err);
}

}  // namespace

TEST(LibraryTests, markdown_rulebooks_are_listed_with_titles) {
    auto books = library::rulebooks();
    auto names = books.keys();
    CHECK(subset({"soccer", "basketball", "golf"}, std::set<std::string>(names.begin(), names.end())));
    CHECK(library::title_of(books.at("soccer")).rfind("Soccer", 0) == 0);
}

TEST(LibraryTests, search_finds_the_clause_that_matches) {
    auto hits = library::search(library::load_rulebook("soccer"), "did the defender deliberately play the ball or was it a deflection", 3);
    CHECK(hits[0].id == "LOTG-11.DP" || hits[0].id == "LOTG-11.2");
    auto f1 = ids_of(library::search(library::load_rulebook("formula1"), "they collided at turn 1; who was to blame", 3));
    CHECK(subset({"FIA-L4.2d", "FIA-B1.9.3"}, f1));
    auto lapped = library::search(library::load_rulebook("formula1"), "lapped cars were allowed to overtake behind the safety car", 2);
    CHECK_EQ(ids_of(lapped), (std::set<std::string>{"FIA-B5.13.4", "FIA-B5.13.5"}));
}

TEST(LibraryTests, every_rulebook_id_works_as_a_citation) {
    static const std::regex FACT_LIKE(R"(^[FD]\d+$)"), CITABLE(R"(^[A-Za-z0-9.]*[A-Za-z0-9]$)");
    for (const auto& [name, _] : library::rulebooks()) {
        for (const auto& [rid, rule] : library::load_rulebook(name)) {
            size_t dash = rid.find('-');
            std::string prefix = rid.substr(0, dash), rest = dash == std::string::npos ? "" : rid.substr(dash + 1);
            CHECK(!std::regex_search(prefix, FACT_LIKE));  // would read as a fact ID like F1
            CHECK(std::regex_search(rest, CITABLE));       // what the council's citation pattern takes
        }
    }
}

TEST(LibraryTests, pdf_rulebook_is_split_into_page_clauses_and_markdown_wins) {
    SKIP_UNLESS(!which("pdftotext").empty(), "needs pdftotext (brew install poppler)");
    test::TempDir tmp;
    std::vector<std::string> lines = {"Rule 9.2b Deciding What Caused Ball to Move"};
    for (int i = 0; i < 6; ++i) lines.push_back(FILLER);
    lines.push_back("Rule 9.3 Ball Moved by Natural Forces");
    lines.push_back("if wind moves the ball there is no penalty");
    lines.push_back(FILLER);
    write_file(tmp.path / "randa-rules.pdf", tiny_pdf(lines));
    auto rules = library::load_rulebook("randa-rules", tmp.path);
    CHECK_EQ(rules.keys(), (std::vector<std::string>{"RANDA-p1.1", "RANDA-p1.2"}));
    CHECK(rules.at("RANDA-p1.2").title.rfind("Rule 9.3", 0) == 0);
    CHECK_EQ(rules.at("RANDA-p1.1").source, "randa-rules.pdf, page 1");
    CHECK(fs::exists(tmp.path / ".cache" / "randa-rules.json"));
    write_file(tmp.path / "randa-rules.md", "# Golf\n\n## GOLF-9.3 Ball moved by natural forces\nNo penalty.\n");
    CHECK_EQ(library::rulebooks(tmp.path).at("randa-rules").extension().string(), ".md");
}

TEST(EvidenceTests, frame_times) {
    using V = std::vector<double>;
    CHECK_EQ(evidence::frame_times(60, 5, 30, 2), (V{28.0, 29.0, 30.0, 31.0, 32.0}));
    CHECK_EQ(evidence::frame_times(10, 3), (V{0.0, 4.88, 9.75}));
    CHECK_EQ(evidence::frame_times(10, 3, 9.5, 2), (V{7.5, 8.62, 9.75}));
    CHECK_EQ(evidence::seconds("1:02.5"), 62.5);
    CHECK_EQ(evidence::seconds("42"), 42.0);
}

TEST(EvidenceTests, observations_are_parsed_per_frame) {
    auto obs = evidence::parse_observations("t=1.0s: red #9 shoots\nT = 2.5 s - ball in the net", {1.0, 2.5});
    CHECK_EQ(obs, (std::vector<evidence::Observation>{{1.0, "red #9 shoots"}, {2.5, "ball in the net"}}));
    CHECK_EQ(obs[1].str(), "[video 0:02.5] ball in the net");
    auto loose = evidence::parse_observations("Players in red and blue.", {4.0, 5.0});
    CHECK_EQ(loose, (std::vector<evidence::Observation>{{4.0, "Players in red and blue."}}));
}

TEST(EvidenceTests, watch_sends_frames_to_the_vision_model) {
    SKIP_UNLESS(!which("ffmpeg").empty(), "needs ffmpeg");
    test::TempDir tmp;
    fs::path clip = tmp.path / "clip.mp4";
    make_clip(clip);
    std::vector<std::string> seen;
    llm::Chat look = [&](const std::string&, const std::string& user, int, int, const llm::Images& images, bool) {
        seen.insert(seen.end(), images.begin(), images.end());
        static const std::regex TIME(R"(t=([\d.]+)s)");
        std::vector<std::string> lines;
        for (std::sregex_iterator it(user.begin(), user.end(), TIME), end; it != end; ++it)
            lines.push_back("t=" + (*it)[1].str() + "s: test pattern");
        return py::join(lines, "\n");
    };
    auto obs = evidence::watch(clip, "", look, tmp.path / "frames", 4, std::nullopt, 3.0, quiet);
    CHECK_EQ(glob(tmp.path / "frames", "", ".jpg").size(), size_t{4});
    CHECK_EQ(seen.size(), size_t{4});
    for (const auto& img : seen) CHECK(img.rfind("\xff\xd8", 0) == 0);  // JPEGs
    std::vector<double> times;
    for (const auto& o : obs) times.push_back(o.t);
    CHECK_EQ(times, (std::vector<double>{0.0, 0.92, 1.83, 2.75}));
}

TEST(IntakeTests, builds_a_case_the_council_can_debate) {
    auto c = intake::Intake(model(), "Northgate v Eastport, 88th minute.", {}, quiet).build("from description");
    CHECK_EQ(c.rulebooks(), std::vector<std::string>{"soccer"});
    CHECK_EQ(c.rules, (std::vector<std::string>{"LOTG-11.2", "LOTG-11.DP", "LOTG-11.4"}));  // unknown IDs dropped
    CHECK_EQ(c.call, "goal_stands");
    CHECK_EQ(c.rulings.keys(), (std::vector<std::string>{"goal_stands", "no_goal"}));
    CHECK_EQ(c.facts.at("F1"), "The striker was in an offside position. [your context]");
    CHECK_EQ(c.facts.at("F2"), "The defender stretched for the ball. [video 0:41.5]");
    CHECK_EQ(c.disputed, (Dict<std::string>{{"D1", "Whether the touch was a deliberate play."}}));
    const auto& northgate = c.parties.at(0);
    const auto& eastport = c.parties.at(1);
    CHECK_EQ(northgate.likes, (Dict<double>{{"goal_stands", 1.0}, {"no_goal", 0.0}}));
    CHECK_EQ(eastport.likes, (Dict<double>{{"no_goal", 1.0}, {"goal_stands", 0.0}}));

    cases::Case again;
    {
        test::TempDir tmp;
        fs::path path = tmp.path / "case.json";
        cases::save_case(c, path, json::Object{{"evidence", std::vector<std::string>{"[video 0:41.5] a defender stretches"}}});
        again = cases::load_case(path);
    }
    CHECK(again == c);
    auto result = council::Council("t", again, model(), 2, quiet).run();
    CHECK_EQ(result.status, "VOTE");
    CHECK_EQ(result.choice, "goal_stands");
    CHECK_EQ(result.tally, (Dict<int>{{"goal_stands", 2}, {"no_goal", 1}}));
}

TEST(IntakeTests, doubtful_answers_are_flagged_for_the_user) {
    json::Value rulings = json::Value::parse(R"({"rulings": [{"id": "no_goal", "text": "Offside"}, {"id": "retake", "text": "Retake"}]})");
    intake::Intake builder(model(understand_answer(), rulings), "...", {}, quiet);
    auto c = builder.build("x");
    CHECK_EQ(c.call, "no_goal");  // no call_id: the first ruling
    CHECK_EQ(c.rulings.keys(), (std::vector<std::string>{"no_goal", "retake"}));
    auto any = [&](const std::string& part) {
        for (const auto& w : builder.warnings)
            if (w.find(part) != std::string::npos) return true;
        return false;
    };
    CHECK(any("'no_goal' is the call"));
    CHECK(any("Every party's first choice is no_goal"));
}

TEST(IntakeTests, it_asks_for_help_instead_of_guessing) {
    json::Value one_party = understand_answer();
    one_party["parties"] = json::Array{json::Object{{"name", "Northgate United"}}};
    CHECK_THROWS(intake::IntakeError, "two parties", intake::Intake(model(one_party), "...", {}, quiet).build("x"));
    json::Value curling = understand_answer();
    curling["sport"] = "curling";
    curling["rulebooks"] = json::Array{};
    CHECK_THROWS(intake::IntakeError, "No rulebook in rules/ covers curling", intake::Intake(model(curling), "...", {}, quiet).build("x"));
}

TEST(ReviewTests, context_only_review_saves_the_case_and_the_transcript) {
    test::TempDir tmp;
    test::Patch<std::function<llm::Chat(const std::string&)>> connect(llm::connect, [](const std::string&) { return model(); });
    test::Patch<fs::path> here(council::HERE, tmp.path);
    {
        test::Quiet quiet_stdout;
        CHECK_EQ(review::main({"--context", "Northgate v Eastport", "--yes", "--name", "t", "--rounds", "2"}), 0);
    }
    fs::path folder = tmp.path / "cases" / "t";
    CHECK_EQ(cases::load_case(folder / "case.json").call, "goal_stands");
    std::string transcript = read_file(folder / "transcript.md");
    CHECK_IN("Closing statements to the panel", transcript);
    CHECK_IN("DECISION after 2 rounds, by the panel (2 of 3)", transcript);
}
