#include "util.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

extern char** environ;

void print_line(const std::string& text) { std::cout << text << std::endl; }

namespace utf8 {

bool decode(std::string_view s, size_t& i, uint32_t& cp) {
    auto byte = [&](size_t k) { return static_cast<unsigned char>(s[k]); };
    unsigned char c = byte(i);
    size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
    if (n == 0 || i + n > s.size()) {
        ++i;
        return false;
    }
    cp = n == 1 ? c : n == 2 ? c & 0x1F : n == 3 ? c & 0x0F : c & 0x07;
    for (size_t k = 1; k < n; ++k) {
        if ((byte(i + k) & 0xC0) != 0x80) {
            ++i;
            return false;
        }
        cp = (cp << 6) | (byte(i + k) & 0x3F);
    }
    static const uint32_t smallest[] = {0, 0, 0x80, 0x800, 0x10000};
    if (cp < smallest[n] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        ++i;
        return false;
    }
    i += n;
    return true;
}

void append(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

}  // namespace utf8

namespace py {

namespace {
bool is_space(char c) { return WHITESPACE.find(c) != std::string_view::npos; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
}  // namespace

std::string strip(std::string_view s, std::string_view chars) { return lstrip(rstrip(s, chars), chars); }

std::string lstrip(std::string_view s, std::string_view chars) {
    size_t b = s.find_first_not_of(chars);
    return b == std::string_view::npos ? "" : std::string(s.substr(b));
}

std::string rstrip(std::string_view s, std::string_view chars) {
    size_t e = s.find_last_not_of(chars);
    return e == std::string_view::npos ? "" : std::string(s.substr(0, e + 1));
}

std::vector<std::string> split(std::string_view s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && is_space(s[i])) ++i;
        size_t start = i;
        while (i < s.size() && !is_space(s[i])) ++i;
        if (i > start) out.emplace_back(s.substr(start, i - start));
    }
    return out;
}

std::vector<std::string> split(std::string_view s, std::string_view sep) {
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t at; (at = s.find(sep, start)) != std::string_view::npos; start = at + sep.size())
        out.emplace_back(s.substr(start, at - start));
    out.emplace_back(s.substr(start));
    return out;
}

std::vector<std::string> splitlines(std::string_view s) {
    // The line breaks str.splitlines() knows: \n, \r\n, \r, \v, \f, \x1c-\x1e, and U+0085, U+2028, U+2029.
    std::vector<std::string> out;
    size_t start = 0, i = 0;
    while (i < s.size()) {
        size_t width = 0;
        unsigned char c = s[i];
        if (c == '\r') width = i + 1 < s.size() && s[i + 1] == '\n' ? 2 : 1;
        else if (c == '\n' || c == '\v' || c == '\f' || c == 0x1c || c == 0x1d || c == 0x1e) width = 1;
        else if (s.substr(i, 2) == "\xc2\x85") width = 2;
        else if (s.substr(i, 3) == "\xe2\x80\xa8" || s.substr(i, 3) == "\xe2\x80\xa9") width = 3;
        if (width) {
            out.emplace_back(s.substr(start, i - start));
            i += width;
            start = i;
        } else {
            ++i;
        }
    }
    if (start < s.size()) out.emplace_back(s.substr(start));
    return out;
}

std::string join(const std::vector<std::string>& parts, std::string_view sep) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}

std::string squash(std::string_view s) { return join(split(s), " "); }

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}

std::string upper(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return out;
}

std::string replace(std::string_view s, std::string_view from, std::string_view to) {
    if (from.empty()) return std::string(s);
    std::string out;
    size_t start = 0;
    for (size_t at; (at = s.find(from, start)) != std::string_view::npos; start = at + from.size()) {
        out += s.substr(start, at - start);
        out += to;
    }
    out += s.substr(start);
    return out;
}

std::string removeprefix(std::string_view s, std::string_view prefix) {
    return std::string(s.substr(0, prefix.size()) == prefix ? s.substr(prefix.size()) : s);
}

bool contains(std::string_view s, std::string_view part) { return s.find(part) != std::string_view::npos; }

std::string repr(std::string_view s) {
    char quote = contains(s, "'") && !contains(s, "\"") ? '"' : '\'';
    std::string out(1, quote);
    for (unsigned char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == quote) out += std::string("\\") + static_cast<char>(c);
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20 || c == 0x7f) out += format("\\x%02x", c);
        else out += static_cast<char>(c);
    }
    return out + quote;
}

size_t length(std::string_view s) {
    size_t n = 0;
    for (size_t i = 0; i < s.size();) {
        uint32_t cp;
        utf8::decode(s, i, cp);
        ++n;
    }
    return n;
}

std::string head(std::string_view s, size_t n) {
    size_t i = 0;
    for (size_t k = 0; k < n && i < s.size(); ++k) {
        uint32_t cp;
        utf8::decode(s, i, cp);
    }
    return std::string(s.substr(0, i));
}

std::string ljust(std::string_view s, size_t width) {
    size_t n = length(s);
    return std::string(s) + std::string(n < width ? width - n : 0, ' ');
}

std::string rjust(std::string_view s, size_t width) {
    size_t n = length(s);
    return std::string(n < width ? width - n : 0, ' ') + std::string(s);
}

std::string format(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    va_list again;
    va_copy(again, args);
    int n = std::vsnprintf(nullptr, 0, fmt, args);
    va_end(args);
    std::string out(n > 0 ? n : 0, '\0');
    if (n > 0) std::vsnprintf(out.data(), out.size() + 1, fmt, again);
    va_end(again);
    return out;
}

double round(double x, int digits) {
    // printf rounds the exact binary value correctly, with ties to even: the same as Python's round().
    if (!std::isfinite(x)) return x;
    return std::strtod(format("%.*f", digits, x).c_str(), nullptr);
}

namespace {
// Python allows single underscores between digits in numbers ("1_000").
bool drop_underscores(std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '_') {
            if (i == 0 || i + 1 == s.size() || !is_digit(s[i - 1]) || !is_digit(s[i + 1])) return false;
            continue;
        }
        out += s[i];
    }
    s = out;
    return true;
}
}  // namespace

double to_float(std::string_view text) {
    std::string s = strip(text);
    auto fail = [&]() -> double { throw ValueError("could not convert string to float: " + repr(text)); };
    if (s.empty() || !drop_underscores(s)) fail();
    std::string word = lower(s[0] == '+' || s[0] == '-' ? s.substr(1) : s);
    if (word == "inf" || word == "infinity" || word == "nan") {
        double value = word == "nan" ? NAN : INFINITY;
        return s[0] == '-' ? -value : value;
    }
    for (char c : s)
        if (!(is_digit(c) || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-')) fail();
    char* end = nullptr;
    double value = std::strtod(s.c_str(), &end);
    if (end != s.c_str() + s.size()) fail();
    return value;
}

int64_t to_int(std::string_view text) {
    std::string s = strip(text);
    auto fail = [&]() -> int64_t { throw ValueError("invalid literal for int() with base 10: " + repr(text)); };
    if (!drop_underscores(s)) fail();
    size_t start = !s.empty() && (s[0] == '+' || s[0] == '-') ? 1 : 0;
    if (start == s.size()) fail();
    for (size_t i = start; i < s.size(); ++i)
        if (!is_digit(s[i])) fail();
    int64_t value = 0;
    auto [ptr, ec] = std::from_chars(s.data() + start, s.data() + s.size(), value);
    if (ec != std::errc()) fail();
    return s[0] == '-' ? -value : value;
}

std::string float_repr(double x) {
    if (std::isnan(x)) return "nan";
    if (std::isinf(x)) return x < 0 ? "-inf" : "inf";
    if (x == 0) return std::signbit(x) ? "-0.0" : "0.0";
    // The shortest digits that read back as x, then laid out the way Python's repr does.
    char buf[64];
    auto res = std::to_chars(buf, buf + sizeof buf, x, std::chars_format::scientific);
    std::string sci(buf, res.ptr);
    std::string sign = sci[0] == '-' ? "-" : "";
    if (!sign.empty()) sci.erase(0, 1);
    size_t e = sci.find('e');
    std::string digits = sci.substr(0, e);
    digits.erase(std::remove(digits.begin(), digits.end(), '.'), digits.end());
    int exponent = std::atoi(sci.c_str() + e + 1);
    int decpt = exponent + 1;  // where the decimal point goes in digits
    int n = static_cast<int>(digits.size());
    if (decpt <= -4 || decpt > 16) {
        std::string mantissa = digits.substr(0, 1) + (n > 1 ? "." + digits.substr(1) : "");
        return sign + mantissa + format("e%c%02d", exponent < 0 ? '-' : '+', std::abs(exponent));
    }
    if (decpt <= 0) return sign + "0." + std::string(-decpt, '0') + digits;
    if (decpt >= n) return sign + digits + std::string(decpt - n, '0') + ".0";
    return sign + digits.substr(0, decpt) + "." + digits.substr(decpt);
}

}  // namespace py

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw OSError(py::format("[Errno %d] %s: %s", errno, std::strerror(errno), py::repr(path.string()).c_str()));
    std::ostringstream data;
    data << in.rdbuf();
    return data.str();
}

void write_file(const fs::path& path, std::string_view data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw OSError(py::format("[Errno %d] %s: %s", errno, std::strerror(errno), py::repr(path.string()).c_str()));
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!out) throw OSError("couldn't write " + path.string());
}

std::string which(const std::string& name) {
    auto runnable = [](const fs::path& p) {
        std::error_code ec;
        return access(p.c_str(), X_OK) == 0 && !fs::is_directory(p, ec);
    };
    if (name.find('/') != std::string::npos) return runnable(name) ? name : "";
    const char* path = std::getenv("PATH");
    for (const auto& dir : py::split(path ? path : "", ":")) {
        fs::path candidate = fs::path(dir.empty() ? "." : dir) / name;
        if (runnable(candidate)) return candidate.string();
    }
    return "";
}

Completed run(const std::vector<std::string>& argv, double timeout) {
    int out_pipe[2], err_pipe[2];
    if (pipe(out_pipe) != 0) throw OSError(std::strerror(errno));
    if (pipe(err_pipe) != 0) {
        close(out_pipe[0]);
        close(out_pipe[1]);
        throw OSError(std::strerror(errno));
    }
    for (int fd : {out_pipe[0], out_pipe[1], err_pipe[0], err_pipe[1]}) fcntl(fd, F_SETFD, FD_CLOEXEC);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, out_pipe[1], 1);
    posix_spawn_file_actions_adddup2(&actions, err_pipe[1], 2);
    std::vector<char*> args;
    for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);  // the child gets only stdin, stdout and stderr
#endif
    pid_t pid = 0;
    int spawned = posix_spawnp(&pid, args[0], &actions, &attr, args.data(), environ);
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
    close(out_pipe[1]);
    close(err_pipe[1]);
    if (spawned != 0) {
        close(out_pipe[0]);
        close(err_pipe[0]);
        throw OSError(py::format("[Errno %d] %s: %s", spawned, std::strerror(spawned), py::repr(argv[0]).c_str()));
    }

    Completed done;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeout);
    pollfd fds[2] = {{out_pipe[0], POLLIN, 0}, {err_pipe[0], POLLIN, 0}};
    std::string* sinks[2] = {&done.out, &done.err};
    int open_pipes = 2;
    bool timed_out = false;
    while (open_pipes > 0) {
        int wait_ms = -1;
        if (timeout > 0) {
            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (left.count() <= 0) {
                timed_out = true;
                break;
            }
            wait_ms = static_cast<int>(std::min<int64_t>(left.count(), 1000));
        }
        int ready = poll(fds, 2, wait_ms);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) break;
        for (int k = 0; k < 2; ++k) {
            if (fds[k].fd < 0 || !(fds[k].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            char buf[65536];
            ssize_t n = read(fds[k].fd, buf, sizeof buf);
            if (n > 0) {
                sinks[k]->append(buf, static_cast<size_t>(n));
            } else if (n == 0 || errno != EINTR) {
                close(fds[k].fd);
                fds[k].fd = -1;
                --open_pipes;
            }
        }
    }
    for (auto& f : fds)
        if (f.fd >= 0) close(f.fd);
    if (timed_out) kill(pid, SIGKILL);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (timed_out)
        throw SubprocessError(py::format("Command '%s' timed out after %g seconds", py::join(argv, " ").c_str(), timeout));
    done.code = WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? -WTERMSIG(status) : -1;
    return done;
}

std::vector<fs::path> glob(const fs::path& dir, std::string_view prefix, std::string_view suffix) {
    std::vector<fs::path> found;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::string name = it->path().filename().string();
        if (name.size() >= prefix.size() + suffix.size() && name.compare(0, prefix.size(), prefix) == 0 &&
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
            found.push_back(it->path());
    }
    std::sort(found.begin(), found.end());
    return found;
}

std::string strftime_now(const char* fmt) {
    std::time_t t = std::time(nullptr);
    std::tm local{};
    localtime_r(&t, &local);
    char buf[128];
    size_t n = std::strftime(buf, sizeof buf, fmt, &local);
    return std::string(buf, n);
}

double now() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string random_hex(size_t n) {
    thread_local std::mt19937_64 gen{std::random_device{}()};
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < n; ++i) out += digits[gen() % 16];
    return out;
}

const fs::path& project_root() {
    static const fs::path root = [] {
        fs::path exe;
        std::error_code ec;
#ifdef __APPLE__
        uint32_t size = 0;
        _NSGetExecutablePath(nullptr, &size);
        std::string buf(size, '\0');
        if (_NSGetExecutablePath(buf.data(), &size) == 0) exe = buf.c_str();
#elif defined(__linux__)
        exe = fs::read_symlink("/proc/self/exe", ec);
#endif
        exe = fs::weakly_canonical(exe, ec);
        for (fs::path dir = exe.parent_path(); !dir.empty(); dir = dir.parent_path()) {
            if (fs::is_directory(dir / "rules", ec) && fs::is_directory(dir / "web", ec)) return dir;
            if (dir == dir.parent_path()) break;
        }
        return fs::current_path(ec);
    }();
    return root;
}
