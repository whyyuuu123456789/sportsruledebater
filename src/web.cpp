#include "web.hpp"

#include <pthread.h>
#include <signal.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <thread>

#include "cli.hpp"
#include "council.hpp"
#include "evidence.hpp"
#include "intake.hpp"
#include "library.hpp"
#include "llm.hpp"
#include "review.hpp"

namespace web {

fs::path WEB = project_root() / "web";
fs::path UPLOADS = project_root() / "uploads";
std::function<std::string(const std::string&)> which = ::which;

namespace {

const std::vector<std::string> VIDEO_EXT = {".mp4", ".webm", ".mov", ".mkv", ".m4v", ".avi", ".ogv", ".ogg"};
constexpr size_t MAX_VIDEO = size_t{500} << 20;  // bytes, for downloads and uploads
constexpr size_t MAX_JSON = size_t{1} << 20;
const std::string AGENT = "sportsruledebater/0.1 (local review tool)";
const Dict<std::string> STATIC = {{"index.html", "text/html; charset=utf-8"},
                                  {"app.js", "text/javascript; charset=utf-8"},
                                  {"style.css", "text/css; charset=utf-8"}};

fs::path home() { return council::HERE; }  // read at call time, so tests can point it somewhere else

bool in(const std::vector<std::string>& list, const std::string& item) {
    return std::find(list.begin(), list.end(), item) != list.end();
}

class Event {
public:
    void set() {
        {
            std::lock_guard<std::mutex> g(m_);
            set_ = true;
        }
        cv_.notify_all();
    }
    void wait() {
        std::unique_lock<std::mutex> g(m_);
        cv_.wait(g, [&] { return set_; });
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    bool set_ = false;
};

// One review, from the link to the ruling. The page polls view() for progress.
struct Job {
    explicit Job(Params p) : params(std::move(p)) {
        id = random_hex(12);
        name = strftime_now("web_%Y%m%d_%H%M%S_") + id.substr(0, 4);
    }

    std::string id, name;
    const Params params;
    std::mutex lock;
    std::string status = "queued";  // queued, downloading, watching, building, checking, discussing, done, failed
    std::vector<std::string> lines;
    json::Value raw_case;
    json::Value case_view;  // null until the case is built
    std::vector<std::string> warnings;
    json::Value result;
    std::string error;
    std::optional<cases::Case> edited;
    Event go_on;  // set when the user has checked the case

    void say(const std::string& text) {
        std::lock_guard<std::mutex> g(lock);
        lines.push_back(text);
    }
    void set_status(const std::string& stage) {
        std::lock_guard<std::mutex> g(lock);
        status = stage;
    }
    void fail(const std::string& message) {
        std::lock_guard<std::mutex> g(lock);
        error = message;
        status = "failed";
    }
    json::Value view(size_t since) {
        std::lock_guard<std::mutex> g(lock);
        std::vector<std::string> recent(lines.begin() + static_cast<long>(std::min(since, lines.size())), lines.end());
        return json::Object{{"id", id},
                            {"name", name},
                            {"status", status},
                            {"error", error},
                            {"lines", recent},
                            {"next", lines.size()},
                            {"case", case_view},
                            {"warnings", warnings},
                            {"result", result},
                            {"check", params.check},
                            {"has_clip", !params.source.empty()}};
    }
};

// One review runs at a time: the models share this machine. Kept for the life of the program, so the worker
// thread never outlives them.
class Queue {
public:
    void push(std::shared_ptr<Job> job) {
        {
            std::lock_guard<std::mutex> g(m_);
            q_.push_back(std::move(job));
        }
        cv_.notify_one();
    }
    std::shared_ptr<Job> pop() {
        std::unique_lock<std::mutex> g(m_);
        cv_.wait(g, [&] { return !q_.empty(); });
        auto job = q_.front();
        q_.pop_front();
        return job;
    }
    size_t size() {
        std::lock_guard<std::mutex> g(m_);
        return q_.size();
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::shared_ptr<Job>> q_;
};

Queue& queue() {
    static auto* q = new Queue;
    return *q;
}

std::mutex& jobs_lock() {
    static auto* m = new std::mutex;
    return *m;
}

std::map<std::string, std::shared_ptr<Job>>& jobs() {
    static auto* j = new std::map<std::string, std::shared_ptr<Job>>;
    return *j;
}

std::shared_ptr<Job> find_job(const std::string& id) {
    std::lock_guard<std::mutex> g(jobs_lock());
    auto it = jobs().find(id);
    return it == jobs().end() ? nullptr : it->second;
}

// int(value), as Python would take it from the form's JSON.
int64_t to_int(const json::Value& v) {
    if (v.is_bool() || v.is_int()) return v.as_int();
    if (v.is_number() && std::isfinite(v.as_number())) return static_cast<int64_t>(v.as_number());
    if (v.is_string()) return py::to_int(v.as_string());
    throw ValueError("not a number");
}

// (scheme://netloc, path) pieces of a URL, as urllib.parse.urlparse splits them.
std::string url_netloc(const std::string& url) {
    size_t start = url.find("://");
    if (start == std::string::npos) return "";
    start += 3;
    return url.substr(start, url.find_first_of("/?#", start) - start);
}

std::string url_path(const std::string& url) {
    size_t start = url.find("://");
    if (start == std::string::npos) return "";
    size_t slash = url.find_first_of("/?#", start + 3);
    if (slash == std::string::npos || url[slash] != '/') return "";
    return url.substr(slash, url.find_first_of("?#", slash) - slash);
}

std::pair<std::string, std::string> split_target(const std::string& target) {
    size_t q = target.find('?');
    std::string path = target.substr(0, q), query = q == std::string::npos ? "" : target.substr(q + 1);
    if (size_t hash = query.find('#'); hash != std::string::npos) query.resize(hash);
    return {path, query};
}

void run_job(const std::shared_ptr<Job>& job) {
    const Params& p = job->params;
    Log say = [job](const std::string& text) { job->say(text); };
    try {
        llm::Chat chat = llm::connect(llm::MODEL), look = llm::connect(llm::VISION_MODEL);
        fs::path folder = home() / "cases" / job->name;
        std::optional<fs::path> video;
        double offset = 0;
        if (!p.source.empty()) {
            job->set_status("downloading");
            auto [path, starts_at] = fetch(p.source, folder, p.start, p.end, say);
            video = path;
            offset = starts_at;
        }
        std::optional<double> at;
        double window = 3.0;
        if (p.start) {
            double end = p.end ? *p.end : *p.start + 3;
            at = (*p.start + end) / 2 - offset;
            window = std::max(0.5, (end - *p.start) / 2);
        }
        auto prepared = review::prepare(video, p.context, job->name, chat, look, at, window, p.frames, say,
                                        [job](const std::string& stage) { job->set_status(stage); });
        cases::Case c = prepared.c;
        json::Value view = describe(c, job->name);
        {
            std::lock_guard<std::mutex> g(job->lock);
            job->warnings = prepared.warnings;
            job->raw_case = cases::to_json(c);
            job->case_view = view;
        }
        if (p.check) {
            job->set_status("checking");
            job->say("The case is ready for you to check.");
            job->go_on.wait();
            std::optional<cases::Case> edited;
            {
                std::lock_guard<std::mutex> g(job->lock);
                edited = job->edited;
            }
            if (edited) {
                c = *edited;
                cases::save_case(c, prepared.path, json::Object{{"context", p.context}, {"warnings", prepared.warnings}});
                view = describe(c, job->name);
                std::lock_guard<std::mutex> g(job->lock);
                job->case_view = view;
            }
        }
        job->set_status("discussing");
        council::Result result = council::debate(job->name, c, chat, p.rounds, prepared.path.parent_path() / "transcript.md",
                                                 council::SAVED_HEADER, p.panel ? look : nullptr, p.panel, say);
        std::set<std::string> seen;
        for (double t : result.frames_seen) seen.insert(py::format("frame_%07.2f.jpg", t));
        json::Value out = result.to_json();
        out["ruling"] = c.rulings.at(result.choice);
        out["call"] = c.call;
        out["rulings"] = c.rulings;
        json::Array frames_seen;
        for (const auto& url : json::iterate(view.get("frame_urls")))
            if (seen.count(fs::path(url.as_string()).filename().string())) frames_seen.push_back(url);
        out["frames_seen"] = frames_seen;
        std::lock_guard<std::mutex> g(job->lock);
        job->result = out;
        job->status = "done";
    } catch (const intake::IntakeError& e) {
        job->fail(std::string("Couldn't build the case: ") + e.what());
    } catch (const ValueError& e) {
        job->fail(e.what());
    } catch (const RuntimeError& e) {  // also: Ollama isn't running, or a model hasn't been downloaded
        job->fail(e.what());
    } catch (const OSError& e) {
        job->fail(e.what());
    } catch (const SubprocessError& e) {
        job->fail(e.what());
    } catch (const fs::filesystem_error& e) {
        job->fail(e.what());
    } catch (const std::exception& e) {  // keep the worker alive, and show what happened
        job->say(std::string("Unexpected error: ") + e.what());
        job->fail(std::string("Something went wrong: ") + e.what());
    }
}

void worker() {
    while (true) run_job(queue().pop());
}

void send(http::Connection& conn, int code, std::string_view body, std::string_view kind) {
    conn.send(code, body, kind, {{"Cache-Control", "no-store"}});
}

void send_json(http::Connection& conn, int code, const json::Value& data) {
    send(conn, code, data.dump(), "application/json");
}

json::Value error_json(const std::string& message) { return json::Object{{"error", message}}; }

void do_get(http::Connection& conn) {
    static const std::regex STATIC_FILE(R"(/static/([a-z]+\.(?:html|js|css)))");
    static const std::regex REVIEW(R"(/api/reviews/([0-9a-f]{12}))");
    static const std::regex FRAME(R"(/frames/([A-Za-z0-9_]+)/(frame_[0-9.]+\.jpg))");
    auto [path, query] = split_target(conn.target);
    if (path == "/") path = "/static/index.html";
    std::smatch m;
    std::error_code ec;
    if (std::regex_match(path, m, STATIC_FILE)) {
        fs::path f = WEB / m[1].str();
        if (STATIC.contains(m[1].str()) && fs::exists(f, ec)) return send(conn, 200, read_file(f), STATIC.at(m[1].str()));
    } else if (path == "/api/config") {
        return send_json(conn, 200,
                         json::Object{{"model", llm::MODEL},
                                      {"vision_model", llm::VISION_MODEL},
                                      {"panel", council::PANEL.size()},
                                      {"rounds", council::MAX_ROUNDS},
                                      {"yt_dlp", !which("yt-dlp").empty()}});
    } else if (std::regex_match(path, m, REVIEW)) {
        if (auto job = find_job(m[1].str())) {
            auto qs = http::parse_qs(query);
            std::string since = qs.contains("since") ? qs.at("since")[0] : "0";
            bool digits = !since.empty() && std::all_of(since.begin(), since.end(), [](char c) { return c >= '0' && c <= '9'; });
            size_t n = digits ? (since.size() > 18 ? SIZE_MAX : static_cast<size_t>(std::stoull(since))) : 0;
            return send_json(conn, 200, job->view(n));
        }
    } else if (std::regex_match(path, m, FRAME)) {
        fs::path f = home() / "cases" / m[1].str() / "frames" / m[2].str();
        if (fs::exists(f, ec)) return send(conn, 200, read_file(f), "image/jpeg");
    }
    send_json(conn, 404, error_json("Not found."));
}

void upload(http::Connection& conn, const std::string& query, size_t length) {
    auto qs = http::parse_qs(query);
    std::string name = qs.contains("name") ? qs.at("name")[0] : "";
    std::string ext = py::lower(fs::path(name).extension().string());
    if (!in(VIDEO_EXT, ext)) return send_json(conn, 400, error_json("Upload a video file (" + py::join(VIDEO_EXT, ", ") + ")."));
    if (!(0 < length && length <= MAX_VIDEO)) return send_json(conn, 413, error_json("The file is empty or bigger than 500 MB."));
    fs::create_directory(UPLOADS);
    fs::path dest = UPLOADS / (random_hex(12) + ext);
    std::ofstream f(dest, std::ios::binary | std::ios::trunc);
    std::vector<char> chunk(size_t{1} << 20);
    size_t remaining = length;
    while (remaining) {
        size_t got = conn.read(chunk.data(), std::min(chunk.size(), remaining));
        if (!got) break;
        f.write(chunk.data(), static_cast<std::streamsize>(got));
        remaining -= got;
    }
    f.close();
    send_json(conn, 200, json::Object{{"source", "upload:" + dest.filename().string()}});
}

// The user has checked the case (and maybe edited it): carry on with the discussion.
void start(http::Connection& conn, const std::string& job_id, const json::Value& body) {
    auto job = find_job(job_id);
    if (!job) return send_json(conn, 404, error_json("No such review."));
    json::Value raw;
    {
        std::lock_guard<std::mutex> g(job->lock);
        if (job->status != "checking") raw = nullptr;
        else raw = job->raw_case;
    }
    if (raw.is_null()) return send_json(conn, 409, error_json("This review isn't waiting for a check."));
    if (body.get("case").is_object()) {
        for (const auto& [key, value] : body.get("case").as_object()) raw[key] = value;
        try {
            cases::Case edited = cases::case_from_dict(raw);
            council::Council check(job->name, edited, nullptr);  // raises if a rule ID isn't in the rulebook
            std::lock_guard<std::mutex> g(job->lock);
            job->edited = edited;
        } catch (const ValueError& e) {
            return send_json(conn, 400, error_json(std::string("Can't use that case: ") + e.what() + "."));
        } catch (const OSError& e) {
            return send_json(conn, 400, error_json(std::string("Can't use that case: ") + e.what() + "."));
        }
    }
    job->go_on.set();
    send_json(conn, 200, json::Object{{"ok", true}});
}

void do_post(http::Connection& conn) {
    static const std::regex START(R"(/api/reviews/([0-9a-f]{12})/start)");
    if (conn.header("X-Requested-With") != "council")  // another site's page can't send this header
        return send_json(conn, 403, error_json("Requests have to come from the council's own page."));
    auto [path, query] = split_target(conn.target);
    size_t length = 0;
    try {
        length = static_cast<size_t>(std::max<int64_t>(0, py::to_int(conn.header("Content-Length").empty() ? "0" : conn.header("Content-Length"))));
    } catch (const ValueError&) {
        length = 0;
    }
    if (path == "/api/upload") return upload(conn, query, length);
    if (length > MAX_JSON) return send_json(conn, 413, error_json("That request is too big."));
    std::string raw = conn.read_body(length);
    json::Value body;
    try {
        body = json::Value::parse(raw.empty() ? "{}" : raw);
    } catch (const json::ParseError&) {
        return send_json(conn, 400, error_json("That wasn't valid JSON."));
    }
    if (!body.is_object()) return send_json(conn, 400, error_json("That wasn't valid JSON."));
    std::smatch m;
    if (path == "/api/reviews") {
        Params params;
        try {
            params = review_params(body);
        } catch (const ValueError& e) {
            return send_json(conn, 400, error_json(e.what()));
        }
        auto job = std::make_shared<Job>(params);
        {
            std::lock_guard<std::mutex> g(jobs_lock());
            jobs()[job->id] = job;
        }
        queue().push(job);
        return send_json(conn, 200, json::Object{{"id", job->id}, {"ahead", static_cast<int64_t>(queue().size()) - 1}});
    }
    if (std::regex_match(path, m, START)) return start(conn, m[1].str(), body);
    send_json(conn, 404, error_json("Not found."));
}

void handle(http::Connection& conn) {
    try {
        if (conn.method == "GET") do_get(conn);
        else if (conn.method == "POST") do_post(conn);
        else send_json(conn, 501, error_json("Unsupported method (" + py::repr(conn.method) + ")"));
    } catch (const std::exception& e) {
        send_json(conn, 500, error_json(std::string("Something went wrong: ") + e.what()));
    }
}

const char* DESCRIPTION = R"(A local website for the sports rules council. Paste a link to a clip (or upload one), mark when the incident
happens, say what you know, and get a ruling: the advocates discuss the call, and the neutral panel, who watch the
clip themselves, decide it.

    build/web                 # then open http://localhost:8000
    build/web --port 8080

It runs on this machine (Ollama for the models, ffmpeg for the frames) and only listens on localhost. A direct link
to a video file (.mp4, .webm, .mov and so on) is downloaded as it is. A link to a page, such as YouTube, needs yt-dlp
(brew install yt-dlp): check that the site's terms and the footage's copyright allow downloading it first.)";

}  // namespace

Params review_params(const json::Value& body) {
    auto field = [&](const char* key) {
        const json::Value& v = body.get(key);
        return json::truthy(v) ? py::strip(json::str(v)) : std::string();
    };
    Params p;
    p.source = field("source");
    p.context = field("context");
    if (p.source.empty() && p.context.empty()) throw ValueError("Paste a link or upload a clip, and describe what happened.");
    if (py::length(p.context) > 4000) throw ValueError("Keep the description under 4,000 characters.");
    auto when = [&](const char* key) -> std::optional<double> {
        std::string text = field(key);
        if (text.empty()) return std::nullopt;
        try {
            return evidence::seconds(text);
        } catch (const ValueError&) {
            throw ValueError(std::string("Write the ") + key + " time like 1:12 or 72.5.");
        }
    };
    p.start = when("start");
    p.end = when("end");
    if (p.end && !p.start) throw ValueError("Set when the incident starts, too.");
    if (p.start && p.end && *p.end <= *p.start) throw ValueError("The end has to come after the start.");
    int64_t rounds, panel, frames;
    try {
        rounds = json::truthy(body.get("rounds")) ? to_int(body.get("rounds")) : council::MAX_ROUNDS;
        panel = body.contains("panel") ? to_int(body.get("panel")) : static_cast<int64_t>(council::PANEL.size());
        frames = json::truthy(body.get("frames")) ? to_int(body.get("frames")) : 12;
    } catch (const ValueError&) {
        throw ValueError("Rounds, panel members and frames have to be numbers.");
    }
    p.rounds = static_cast<int>(std::clamp<int64_t>(rounds, 1, 8));
    p.panel = static_cast<int>(std::clamp<int64_t>(panel, 0, static_cast<int64_t>(council::PANEL.size())));
    p.frames = static_cast<int>(std::clamp<int64_t>(frames, 4, 20));
    p.check = body.contains("check") ? json::truthy(body.get("check")) : true;
    return p;
}

std::pair<fs::path, double> fetch(const std::string& source, const fs::path& folder, std::optional<double> start,
                                  std::optional<double> end, const Log& say) {
    if (source.rfind("upload:", 0) == 0) {
        fs::path path = UPLOADS / fs::path(source.substr(7)).filename();
        std::error_code ec;
        if (!fs::exists(path, ec)) throw ValueError("That upload is gone. Upload the clip again.");
        return {path, 0.0};
    }
    if (!std::regex_search(source, std::regex("^https?://", std::regex::icase)))
        throw ValueError("Paste a link that starts with http:// or https://, or upload a file.");
    fs::create_directories(folder);
    fs::path dest;
    std::ofstream file;
    size_t size = 0, reported = 0;
    bool page = false;
    try {
        http::stream(
            source, {"User-Agent: " + AGENT}, 30,
            [&](const http::Response& head) {
                const std::string& kind = head.content_type;
                std::string ext = py::lower(fs::path(url_path(head.url.empty() ? source : head.url)).extension().string());
                bool video_ext = in(VIDEO_EXT, ext);
                if (kind.rfind("video/", 0) == 0 || kind == "application/ogg" || (video_ext && !py::contains(kind, "html"))) {
                    dest = folder / ("clip" + (video_ext ? ext : std::string(".mp4")));
                    file.open(dest, std::ios::binary | std::ios::trunc);
                    if (!file) throw RuntimeError("Couldn't write " + dest.string());
                    say("Downloading the clip from " + url_netloc(source));
                    return true;
                }
                page = true;
                return false;
            },
            [&](std::string_view chunk) {
                size += chunk.size();
                if (size > MAX_VIDEO) throw ValueError("That video is bigger than 500 MB. Trim it, or link to a shorter clip.");
                file.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
                if (size - reported >= (size_t{25} << 20)) {
                    say("  " + std::to_string(size >> 20) + " MB so far");
                    reported = size;
                }
            });
    } catch (const OSError& e) {  // couldn't connect, or an error status
        throw ValueError(std::string("Couldn't open that link (") + e.what() + ").");
    }
    if (page) return fetch_page(source, folder, start, end, say);
    file.close();
    say(py::format("  Downloaded %.1f MB", static_cast<double>(size) / (1 << 20)));
    return {dest, 0.0};
}

std::pair<fs::path, double> fetch_page(const std::string& source, const fs::path& folder, std::optional<double> start,
                                       std::optional<double> end, const Log& say) {
    if (which("yt-dlp").empty())
        throw ValueError("That link is a web page, not a video file. Links to sites like YouTube need yt-dlp "
                         "(brew install yt-dlp); check that the site's terms and the footage's copyright allow "
                         "downloading it first. Or upload the clip, or paste a direct link to a video file.");
    double offset = 0;
    std::vector<std::string> section;
    if (start) {
        offset = std::max(0.0, *start - 5);
        section = {"--download-sections", py::format("*%.2f-%.2f", offset, (end ? *end : *start + 3) + 5),
                   "--force-keyframes-at-cuts"};
    }
    for (const auto& old : glob(folder, "clip.", "")) fs::remove(old);
    say(std::string("Downloading the clip with yt-dlp") + (section.empty() ? "" : ", just the part around the incident"));
    std::vector<std::string> argv{"yt-dlp", "--no-playlist", "--quiet", "--no-warnings", "-f",
                                  "b[height<=720]/bv*[height<=720]+ba/b", "--merge-output-format", "mp4", "-o",
                                  (folder / "clip.%(ext)s").string()};
    argv.insert(argv.end(), section.begin(), section.end());
    argv.insert(argv.end(), {"--", source});
    Completed done = run(argv, 900);
    auto clips = glob(folder, "clip.", "");
    if (done.code || clips.empty()) {
        auto lines = py::splitlines(py::strip(done.err));
        throw ValueError("yt-dlp couldn't download that link: " + (lines.empty() ? std::string("no file came back") : lines.back()));
    }
    return {clips[0], offset};
}

json::Value describe(const cases::Case& c, const std::string& name) {
    Dict<library::Rule> rules;
    for (const auto& rulebook : c.rulebooks())
        for (const auto& [id, rule] : library::load_rulebook(rulebook)) rules[id] = rule;
    json::Value view = cases::to_json(c);
    json::Object titles;
    for (const auto& r : c.rules)
        if (rules.contains(r)) titles[r] = rules.at(r).title;
    std::vector<std::string> urls;
    for (const auto& f : c.frames) urls.push_back("/frames/" + name + "/" + fs::path(f).filename().string());
    view["rule_titles"] = titles;
    view["frame_urls"] = urls;
    return view;
}

std::unique_ptr<http::Server> start_server(int port) {
    static std::once_flag started;
    std::call_once(started, [] {
        signal(SIGPIPE, SIG_IGN);  // a browser that leaves mid-answer
        std::thread(worker).detach();
    });
    auto server = std::make_unique<http::Server>("127.0.0.1", port, handle);
    server->start();
    return server;
}

int main(const std::vector<std::string>& args) {
    cli::Parser ap("web", DESCRIPTION);
    ap.option("--port", "", "8000", "int");
    ap.parse(args);
    // Ctrl-C is handled here, not in whichever thread it happens to land on.
    sigset_t stop;
    sigemptyset(&stop);
    sigaddset(&stop, SIGINT);
    sigaddset(&stop, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &stop, nullptr);
    auto server = start_server(ap.integer("--port"));
    std::cout << "Sports Rules Council is running: open http://localhost:" << server->port() << "  (Ctrl-C to stop)"
              << std::endl;
    int signal_number = 0;
    sigwait(&stop, &signal_number);
    std::cout << std::endl;
    std::_Exit(0);  // a review may still be running in the background: don't wait for it
}

}  // namespace web
