// A local website for the sports rules council. Paste a link to a clip (or upload one), mark when the incident
// happens, say what you know, and get a ruling: the advocates discuss the call, and the neutral panel, who watch the
// clip themselves, decide it.
//
// It runs on this machine (Ollama for the models, ffmpeg for the frames) and only listens on localhost. A direct link
// to a video file (.mp4, .webm, .mov and so on) is downloaded as it is. A link to a page, such as YouTube, needs yt-dlp
// (brew install yt-dlp): check that the site's terms and the footage's copyright allow downloading it first.

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cases.hpp"
#include "http.hpp"
#include "json.hpp"
#include "util.hpp"

namespace web {

extern fs::path WEB;      // the page's files
extern fs::path UPLOADS;  // clips uploaded through the page
// shutil.which, so tests can pretend yt-dlp isn't installed.
extern std::function<std::string(const std::string&)> which;

// The form's fields, checked.
struct Params {
    std::string source, context;
    std::optional<double> start, end;
    int rounds = 4, panel = 3, frames = 12;
    bool check = true;
};
// Raises ValueError with a message for the page.
Params review_params(const json::Value& body);

// Gets the clip. Returns (path, offset): offset is where the file starts in the original video, when only the part
// around the incident was downloaded. Raises ValueError with a message for the page.
std::pair<fs::path, double> fetch(const std::string& source, const fs::path& folder, std::optional<double> start,
                                  std::optional<double> end, const Log& say);
// A link to a page rather than a video file: download it with yt-dlp, if it's installed.
std::pair<fs::path, double> fetch_page(const std::string& source, const fs::path& folder, std::optional<double> start,
                                       std::optional<double> end, const Log& say);

// The case as the page shows it: the saved fields plus rule titles and frame URLs.
json::Value describe(const cases::Case& c, const std::string& name);

// Starts the review worker and the web server in background threads, and returns the server.
std::unique_ptr<http::Server> start_server(int port = 8000);

int main(const std::vector<std::string>& args);

}  // namespace web
