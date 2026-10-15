#include <chrono>
#include <set>
#include <thread>

#include "council.hpp"
#include "fakes.hpp"
#include "http.hpp"
#include "test.hpp"
#include "web.hpp"

namespace {

struct Answer {
    int status = 0;
    json::Value json;  // when the server sent JSON
    std::string raw;
};

// (status, parsed JSON or raw bytes) for a request to the test server.
Answer call(const std::string& base, const std::string& path, const std::optional<std::string>& data = std::nullopt,
            const http::Headers& headers = {}) {
    auto resp = http::request(base + path, data ? "POST" : "GET", data.value_or(""), headers, 10);
    Answer a{resp.status, nullptr, resp.body};
    if (resp.content_type == "application/json") a.json = json::Value::parse(resp.body);
    return a;
}

const http::Headers SEND = {"X-Requested-With: council", "Content-Type: application/json"};

void print_nothing(const std::string&) {}

}  // namespace

TEST(FormTests, the_form_is_checked) {
    auto p = web::review_params(json::Value::parse(R"({"source": "https://example.com/clip.mp4", "context": "x",
        "start": "1:12", "end": "1:20", "rounds": "3", "panel": "9"})"));
    CHECK_EQ(p.start, std::optional<double>(72.0));
    CHECK_EQ(p.end, std::optional<double>(80.0));
    CHECK_EQ(p.rounds, 3);
    CHECK_EQ(p.panel, 3);
    CHECK(p.check);
    std::vector<std::pair<std::string, std::string>> bad = {
        {R"({})", "Paste a link"},
        {R"({"context": "x", "start": "1:20", "end": "1:12"})", "after the start"},
        {R"({"context": "x", "start": "soon"})", "like 1:12"},
        {R"({"context": "x", "end": "0:05"})", "starts, too"}};
    for (const auto& [body, message] : bad) CHECK_THROWS(ValueError, message, web::review_params(json::Value::parse(body)));
}

TEST(FormTests, page_links_need_yt_dlp) {
    {
        test::Patch<std::function<std::string(const std::string&)>> no_yt_dlp(web::which, [](const std::string&) { return std::string(); });
        CHECK_THROWS(ValueError, "brew install yt-dlp",
                     web::fetch_page("https://www.youtube.com/watch?v=abcdefghijk", ".", std::nullopt, std::nullopt, print_nothing));
    }
    CHECK_THROWS(ValueError, "http:// or https://", web::fetch("ftp://example.com/clip.mp4", ".", std::nullopt, std::nullopt, print_nothing));
}

TEST(SiteTests, a_review_from_upload_to_ruling) {
    SKIP_UNLESS(!which("ffmpeg").empty(), "needs ffmpeg");
    test::TempDir tmp;
    test::Patch<fs::path> here(council::HERE, tmp.path);
    test::Patch<fs::path> uploads(web::UPLOADS, tmp.path / "uploads");
    test::Patch<std::function<llm::Chat(const std::string&)>> connect(llm::connect, [](const std::string&) { return model(); });
    auto server = web::start_server(0);
    std::string base = "http://127.0.0.1:" + std::to_string(server->port());

    auto wait_for = [&](const std::string& id, const std::string& status) {
        for (int i = 0; i < 200; ++i) {
            auto view = call(base, "/api/reviews/" + id).json;
            std::string now = view.get("status").as_string();
            if (now == status || now == "failed") return view;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        test::fail(__FILE__, __LINE__, "the review never reached " + status);
    };

    auto page = call(base, "/");
    CHECK_EQ(page.status, 200);
    CHECK_IN("<title>Sports Rules Council</title>", page.raw);
    CHECK_EQ(call(base, "/api/config").json.get("panel").as_int(), int64_t{3});
    CHECK_EQ(call(base, "/api/reviews", R"({"context": "x"})", {"Content-Type: application/json"}).status, 403);  // no X-Requested-With

    fs::path clip = tmp.path / "clip.mp4";
    Completed made_clip = run({"ffmpeg", "-v", "error", "-f", "lavfi", "-i", "testsrc=duration=3:size=320x240:rate=10",
                               "-pix_fmt", "yuv420p", clip.string()});
    CHECK_EQ(made_clip.code, 0);
    auto up = call(base, "/api/upload?name=clip.mp4", read_file(clip), {"X-Requested-With: council"});
    CHECK_EQ(up.status, 200);
    json::Value request = json::Object{{"source", up.json.get("source")}, {"context", "Northgate v Eastport"},
                                       {"start", "0:01"}, {"end", "0:02"}, {"rounds", 2}};
    auto made = call(base, "/api/reviews", request.dump(), SEND);
    CHECK_EQ(made.status, 200);
    std::string id = made.json.get("id").as_string();

    auto view = wait_for(id, "checking");
    if (view.get("status").as_string() != "checking") test::fail(__FILE__, __LINE__, view.get("error").dump());
    json::Value c = view.get("case");
    CHECK(!c.get("frame_urls").as_array().empty());
    CHECK_EQ(c.get("rule_titles").get("LOTG-11.2").as_string(), "Offside offence");
    auto jpeg = call(base, c.get("frame_urls").as_array()[0].as_string());
    CHECK_EQ(jpeg.status, 200);
    CHECK(jpeg.raw.rfind("\xff\xd8", 0) == 0);

    auto answer = call(base, "/api/reviews/" + id + "/start", R"({"case": {"call": "nonsense"}})", SEND);
    CHECK_EQ(answer.status, 400);
    CHECK_IN("isn't one of the rulings", answer.json.get("error").as_string());
    json::Value facts = c.get("facts");
    facts["F1"] = "Edited: the striker was offside when the ball was kicked. [your context]";
    CHECK_EQ(call(base, "/api/reviews/" + id + "/start", json::Value(json::Object{{"case", json::Object{{"facts", facts}}}}).dump(), SEND).status, 200);

    view = wait_for(id, "done");
    if (view.get("status").as_string() != "done") test::fail(__FILE__, __LINE__, view.get("error").dump());
    json::Value result = view.get("result");
    CHECK_EQ(result.get("decided_by").as_string(), "panel");
    CHECK_EQ(result.get("choice").as_string(), "goal_stands");  // A and B vote for it
    CHECK_EQ(result.get("ruling").as_string(), "The goal stands: 2-1");
    std::set<std::string> urls, seen;
    for (const auto& u : c.get("frame_urls").as_array()) urls.insert(u.as_string());
    for (const auto& u : result.get("frames_seen").as_array()) seen.insert(u.as_string());
    CHECK(!seen.empty());  // the panel looked at the clip
    CHECK(std::includes(urls.begin(), urls.end(), seen.begin(), seen.end()));
    fs::path folder = tmp.path / "cases" / view.get("name").as_string();
    json::Value saved = json::Value::parse(read_file(folder / "case.json"));
    CHECK(saved.get("facts").get("F1").as_string().rfind("Edited:", 0) == 0);
    CHECK(fs::exists(folder / "transcript.md"));
}
