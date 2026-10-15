// Watching the video: ffmpeg pulls frames, and a vision model says what it sees in them.
//
// Frames are spread evenly over the clip, or packed around the moment named with --at. For each frame the vision
// model describes who is involved (team colours, shirt numbers), the ball, any official and their signal, and any
// on-screen text such as the score, the clock or a VAR caption. It's told to describe only what it can see.

#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "llm.hpp"
#include "util.hpp"

namespace evidence {

constexpr int BATCH = 3;              // frames per vision call
constexpr double END_MARGIN = 0.25;   // seconds: the last frame of a clip comes a little before its stated end
extern const std::string SYSTEM;

struct Observation {
    double t = 0;
    std::string text;
    std::string str() const;  // "[video 0:41.5] ..."
    bool operator==(const Observation&) const = default;
};

std::string stamp(double t);                 // 62.5 -> "1:02.5"
double seconds(const std::string& text);     // "1:02.5" or "62.5" -> 62.5; raises ValueError
double duration(const fs::path& video);

// count times spread over the clip, or over [at - window, at + window].
std::vector<double> frame_times(double length, int count, std::optional<double> at = std::nullopt, double window = 3.0);
// One JPEG per time. A time ffmpeg can't get a frame for is skipped.
std::vector<std::pair<double, fs::path>> extract_frames(const fs::path& video, const std::vector<double>& times,
                                                        const fs::path& out_dir, int width = 768);
// One 't=12.5s: ...' line per frame. A line whose time isn't one of these frames' (an echoed example, or a made-up
// moment) is dropped. A reply in any other shape is kept whole, at the first frame's time.
std::vector<Observation> parse_observations(const std::string& reply, const std::vector<double>& times);
std::vector<Observation> watch(const fs::path& video, const std::string& context, const llm::Chat& look,
                               const fs::path& out_dir, int count = 12, std::optional<double> at = std::nullopt,
                               double window = 3.0, Log log = nullptr);

}  // namespace evidence
