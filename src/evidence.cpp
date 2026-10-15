#include "evidence.hpp"

#include <algorithm>
#include <cmath>
#include <regex>

namespace evidence {

const std::string SYSTEM =
    "You review video frames of a sports incident for a rules review. You describe only what is visible.";

namespace {
const std::regex LINE(R"(t\s*=\s*(\d+(?:\.\d+)?)\s*s?\s*(?::|-|–)\s*(.+))", std::regex::ECMAScript | std::regex::icase);
}

std::string Observation::str() const { return "[video " + stamp(t) + "] " + text; }

std::string stamp(double t) {
    double rest = std::fmod(t, 60.0);
    if (rest < 0) rest += 60.0;
    return py::format("%d:%04.1f", static_cast<int>(std::floor(t / 60.0)), rest);
}

double seconds(const std::string& text) {
    double total = 0;
    for (const auto& part : py::split(text, ":")) total = total * 60 + py::to_float(part);
    return total;
}

double duration(const fs::path& video) {
    std::vector<std::string> argv{"ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0",
                                  video.string()};
    Completed done = run(argv);
    if (done.code != 0)
        throw SubprocessError(py::format("Command 'ffprobe %s' returned non-zero exit status %d.", video.string().c_str(), done.code));
    return py::to_float(done.out);
}

std::vector<double> frame_times(double length, int count, std::optional<double> at, double window) {
    double start = at ? std::max(0.0, *at - window) : 0.0;
    double end = at ? *at + window : length;
    end = std::max(start, std::min(end, length - END_MARGIN));
    if (count <= 1) return {py::round((start + end) / 2, 2)};
    std::vector<double> times;
    for (int i = 0; i < count; ++i) times.push_back(py::round(start + i * (end - start) / (count - 1), 2));
    return times;
}

std::vector<std::pair<double, fs::path>> extract_frames(const fs::path& video, const std::vector<double>& times,
                                                        const fs::path& out_dir, int width) {
    fs::create_directories(out_dir);
    std::vector<std::pair<double, fs::path>> frames;
    for (double t : times) {
        fs::path path = out_dir / py::format("frame_%07.2f.jpg", t);
        Completed done = run({"ffmpeg", "-v", "error", "-y", "-ss", py::format("%.2f", t), "-i", video.string(),
                              "-frames:v", "1", "-vf", py::format("scale='min(%d,iw)':-2", width), "-q:v", "3",
                              path.string()});
        std::error_code ec;
        if (done.code == 0 && fs::exists(path, ec) && fs::file_size(path, ec) > 0) frames.emplace_back(t, path);
    }
    if (frames.empty()) throw RuntimeError("ffmpeg couldn't read any frames from " + video.filename().string());
    return frames;
}

std::vector<Observation> parse_observations(const std::string& reply, const std::vector<double>& times) {
    std::vector<std::pair<double, std::string>> seen;  // in the order the times first come up
    for (const auto& line : py::splitlines(reply)) {
        std::smatch m;
        if (!std::regex_search(line, m, LINE)) continue;
        double said = std::stod(m[1].str());
        double t = *std::min_element(times.begin(), times.end(),
                                     [&](double a, double b) { return std::abs(a - said) < std::abs(b - said); });
        if (std::abs(t - said) > 0.1) continue;
        std::string text = py::strip(m[2].str());
        auto it = std::find_if(seen.begin(), seen.end(), [&](const auto& s) { return s.first == t; });
        if (it == seen.end()) seen.emplace_back(t, text);
        else it->second += " " + text;
    }
    std::vector<Observation> found;
    for (const auto& [t, text] : seen) found.push_back({t, text});
    if (found.empty() && !py::strip(reply).empty()) found.push_back({times[0], py::squash(reply)});
    return found;
}

std::vector<Observation> watch(const fs::path& video, const std::string& context, const llm::Chat& look,
                               const fs::path& out_dir, int count, std::optional<double> at, double window, Log log) {
    if (!log) log = print_line;
    auto frames = extract_frames(video, frame_times(duration(video), count, at, window), out_dir);
    std::vector<Observation> observations;
    for (size_t i = 0; i < frames.size(); i += BATCH) {
        std::vector<double> times;
        llm::Images images;
        std::vector<std::string> marks;
        for (size_t j = i; j < std::min(frames.size(), i + BATCH); ++j) {
            times.push_back(frames[j].first);
            images.push_back(read_file(frames[j].second));
            marks.push_back(py::format("t=%gs", frames[j].first));
        }
        std::string user =
            "What the person who sent the video says: " + (context.empty() ? std::string("nothing") : context) + "\n" +
            "These " + std::to_string(times.size()) + " frames are from the video at " + py::join(marks, ", ") +
            ", in that order.\n"
            "For each frame, write one line that starts with its time, like 't=<time>s: <what you see>'. Describe what "
            "matters for an officiating decision: who is involved (team colours, shirt numbers), where the ball is, any "
            "official and their signal, and any on-screen text (score, clock, names, captions). Describe only what you "
            "can see; don't guess names, scores or outcomes.";
        std::string reply = look(SYSTEM, user, 120 * static_cast<int>(times.size()), 7 + static_cast<int>(i), images, false);
        auto seen = parse_observations(reply, times);
        observations.insert(observations.end(), seen.begin(), seen.end());
        log("  watched " + stamp(times.front()) + " to " + stamp(times.back()));
    }
    return observations;
}

}  // namespace evidence
