// Small helpers the rest of the council leans on: an insertion-ordered dict, Python-style string functions, files,
// subprocesses, and where the project lives on disk.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

// The kinds of error the council distinguishes, named after the Python exceptions they stand for.
struct ValueError : std::runtime_error { using std::runtime_error::runtime_error; };
struct RuntimeError : std::runtime_error { using std::runtime_error::runtime_error; };
struct OSError : std::runtime_error { using std::runtime_error::runtime_error; };
struct SubprocessError : std::runtime_error { using std::runtime_error::runtime_error; };

using Log = std::function<void(const std::string&)>;

// Prints a line to stdout, like Python's print().
void print_line(const std::string& text);

// A string-keyed map that remembers insertion order, like a Python dict: assigning to an existing key keeps its place.
template <class V>
class Dict {
public:
    using Item = std::pair<std::string, V>;
    using iterator = typename std::vector<Item>::iterator;
    using const_iterator = typename std::vector<Item>::const_iterator;

    Dict() = default;
    Dict(std::initializer_list<Item> items) {
        for (const auto& item : items) (*this)[item.first] = item.second;
    }

    bool contains(const std::string& key) const { return index_.find(key) != index_.end(); }
    V& operator[](const std::string& key) {
        auto it = index_.find(key);
        if (it != index_.end()) return items_[it->second].second;
        index_.emplace(key, items_.size());
        items_.emplace_back(key, V{});
        return items_.back().second;
    }
    const V& at(const std::string& key) const {
        auto it = index_.find(key);
        if (it == index_.end()) throw std::out_of_range("no key '" + key + "'");
        return items_[it->second].second;
    }
    V& at(const std::string& key) { return const_cast<V&>(std::as_const(*this).at(key)); }
    const V* find(const std::string& key) const {
        auto it = index_.find(key);
        return it == index_.end() ? nullptr : &items_[it->second].second;
    }
    // dict.setdefault: adds the key only if it's missing. Returns whether it was added.
    bool setdefault(const std::string& key, V value) {
        if (contains(key)) return false;
        (*this)[key] = std::move(value);
        return true;
    }

    size_t size() const { return items_.size(); }
    bool empty() const { return items_.empty(); }
    iterator begin() { return items_.begin(); }
    iterator end() { return items_.end(); }
    const_iterator begin() const { return items_.begin(); }
    const_iterator end() const { return items_.end(); }
    const Item& front() const { return items_.front(); }

    std::vector<std::string> keys() const {
        std::vector<std::string> out;
        for (const auto& item : items_) out.push_back(item.first);
        return out;
    }
    std::vector<V> values() const {
        std::vector<V> out;
        for (const auto& item : items_) out.push_back(item.second);
        return out;
    }

    // Like Python, two dicts are equal when they hold the same pairs, in any order.
    friend bool operator==(const Dict& a, const Dict& b) {
        if (a.size() != b.size()) return false;
        for (const auto& [key, value] : a.items_) {
            const V* other = b.find(key);
            if (!other || !(*other == value)) return false;
        }
        return true;
    }

private:
    std::vector<Item> items_;
    std::unordered_map<std::string, size_t> index_;
};

// UTF-8, for the few places that count characters rather than bytes.
namespace utf8 {
// Reads the code point at s[i] and moves i past it. On a malformed byte it moves one byte and returns false.
bool decode(std::string_view s, size_t& i, uint32_t& cp);
void append(std::string& out, uint32_t cp);
}  // namespace utf8

// Python's string functions, with the same behaviour (ASCII case and whitespace).
namespace py {
inline constexpr std::string_view WHITESPACE = " \t\n\r\v\f\x1c\x1d\x1e\x1f";

std::string strip(std::string_view s, std::string_view chars = WHITESPACE);
std::string lstrip(std::string_view s, std::string_view chars = WHITESPACE);
std::string rstrip(std::string_view s, std::string_view chars = WHITESPACE);
std::vector<std::string> split(std::string_view s);                        // s.split()
std::vector<std::string> split(std::string_view s, std::string_view sep);  // s.split(sep)
std::vector<std::string> splitlines(std::string_view s);
std::string join(const std::vector<std::string>& parts, std::string_view sep);
std::string squash(std::string_view s);  // " ".join(s.split())
std::string lower(std::string_view s);
std::string upper(std::string_view s);
std::string replace(std::string_view s, std::string_view from, std::string_view to);
std::string removeprefix(std::string_view s, std::string_view prefix);
bool contains(std::string_view s, std::string_view part);
std::string repr(std::string_view s);   // repr(s): 'quoted', with escapes
size_t length(std::string_view s);      // len(s), in characters
std::string head(std::string_view s, size_t n);  // s[:n], in characters
std::string ljust(std::string_view s, size_t width);
std::string rjust(std::string_view s, size_t width);
std::string format(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
double round(double x, int digits);     // round(x, digits): correctly rounded, ties to even
double to_float(std::string_view s);    // float(s); raises ValueError
int64_t to_int(std::string_view s);     // int(s); raises ValueError
std::string float_repr(double x);       // repr(x): the shortest form that reads back the same
}  // namespace py

// Files and processes.
std::string read_file(const fs::path& path);
void write_file(const fs::path& path, std::string_view data);
std::string which(const std::string& name);  // shutil.which: the program's path, or "" if it isn't installed

struct Completed {
    int code = 0;  // the exit status, or minus the signal that killed it
    std::string out, err;
};
// Runs a program and captures its output. Raises OSError if it can't start, SubprocessError if it outlives timeout
// seconds (0: no limit).
Completed run(const std::vector<std::string>& argv, double timeout = 0);

// Entries in dir whose names start with prefix and end with suffix, sorted: glob(prefix + "*" + suffix).
std::vector<fs::path> glob(const fs::path& dir, std::string_view prefix, std::string_view suffix);

std::string strftime_now(const char* fmt);
double now();
std::string random_hex(size_t n);

// The project folder (the one holding rules/ and web/), found from where the program is.
const fs::path& project_root();
