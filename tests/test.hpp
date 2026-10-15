// A small test harness: TEST registers a test, CHECK_* fail it with the values involved, SKIP_UNLESS skips it.

#pragma once

#include <stdlib.h>
#include <unistd.h>

#include <functional>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "util.hpp"

namespace test {

struct Failure {
    std::string message;
};
struct Skipped {
    std::string reason;
};

struct Test {
    std::string name;
    void (*fn)();
};

inline std::vector<Test>& registry() {
    static std::vector<Test> tests;
    return tests;
}

struct Register {
    Register(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

[[noreturn]] inline void fail(const char* file, int line, const std::string& message) {
    throw Failure{std::string(file) + ":" + std::to_string(line) + ": " + message};
}

template <class T> struct is_vector : std::false_type {};
template <class T> struct is_vector<std::vector<T>> : std::true_type {};
template <class T> struct is_dict : std::false_type {};
template <class T> struct is_dict<Dict<T>> : std::true_type {};
template <class T> struct is_pair : std::false_type {};
template <class A, class B> struct is_pair<std::pair<A, B>> : std::true_type {};
template <class T> struct is_optional : std::false_type {};
template <class T> struct is_optional<std::optional<T>> : std::true_type {};

template <class T>
std::string show(const T& v) {
    if constexpr (std::is_same_v<T, bool>) {
        return v ? "true" : "false";
    } else if constexpr (std::is_same_v<T, double>) {
        return py::float_repr(v);
    } else if constexpr (std::is_arithmetic_v<T>) {
        return std::to_string(v);
    } else if constexpr (std::is_same_v<T, std::string>) {
        return py::repr(v);
    } else if constexpr (std::is_array_v<T> || std::is_convertible_v<T, std::string_view>) {
        return py::repr(std::string_view(v));
    } else if constexpr (is_vector<T>::value) {
        std::vector<std::string> parts;
        for (const auto& item : v) parts.push_back(show(item));
        return "[" + py::join(parts, ", ") + "]";
    } else if constexpr (is_dict<T>::value) {
        std::vector<std::string> parts;
        for (const auto& [key, value] : v) parts.push_back(py::repr(key) + ": " + show(value));
        return "{" + py::join(parts, ", ") + "}";
    } else if constexpr (is_pair<T>::value) {
        return "(" + show(v.first) + ", " + show(v.second) + ")";
    } else if constexpr (is_optional<T>::value) {
        return v ? show(*v) : "None";
    } else {
        return "<value>";
    }
}

// A temporary folder, removed with everything in it at the end of the test.
struct TempDir {
    fs::path path;
    TempDir() {
        std::string pattern = (fs::temp_directory_path() / "srd-test-XXXXXX").string();
        if (!mkdtemp(pattern.data())) throw OSError("couldn't make a temporary folder");
        path = pattern;
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

// Sets a global for the rest of the test, like mock.patch.object.
template <class T>
struct Patch {
    T& target;
    T saved;
    Patch(T& target_, T value) : target(target_), saved(target_) { target = std::move(value); }
    ~Patch() { target = std::move(saved); }
    Patch(const Patch&) = delete;
    Patch& operator=(const Patch&) = delete;
};

// Swallows everything printed to stdout while it's alive.
struct Quiet {
    std::ostringstream sink;
    std::streambuf* saved;
    Quiet() : saved(std::cout.rdbuf(sink.rdbuf())) {}
    ~Quiet() { std::cout.rdbuf(saved); }
};

}  // namespace test

#define TEST(suite, name)                                                                   \
    static void suite##_##name();                                                           \
    static const test::Register suite##_##name##_registered(#suite "." #name, suite##_##name); \
    static void suite##_##name()

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) test::fail(__FILE__, __LINE__, "CHECK(" #cond ")"); \
    } while (0)

#define CHECK_EQ(a, b)                                                                                     \
    do {                                                                                                   \
        const auto& a_ = (a);                                                                              \
        const auto& b_ = (b);                                                                              \
        if (!(a_ == b_)) test::fail(__FILE__, __LINE__, test::show(a_) + " != " + test::show(b_));         \
    } while (0)

#define CHECK_IN(part, whole)                                                                                   \
    do {                                                                                                        \
        const std::string p_ = (part), w_ = (whole);                                                            \
        if (w_.find(p_) == std::string::npos) test::fail(__FILE__, __LINE__, py::repr(p_) + " not found in " + py::repr(w_)); \
    } while (0)

#define CHECK_NOT_IN(part, whole)                                                                         \
    do {                                                                                                  \
        const std::string p_ = (part), w_ = (whole);                                                      \
        if (w_.find(p_) != std::string::npos) test::fail(__FILE__, __LINE__, py::repr(p_) + " unexpectedly found"); \
    } while (0)

// CHECK_THROWS(Type, "part of the message", statement...)
#define CHECK_THROWS(type, text, ...)                                                                      \
    do {                                                                                                   \
        try {                                                                                              \
            __VA_ARGS__;                                                                                   \
        } catch (const type& e_) {                                                                         \
            if (std::string(e_.what()).find(text) == std::string::npos)                                    \
                test::fail(__FILE__, __LINE__, "wrong message: " + py::repr(e_.what()));                   \
            break;                                                                                         \
        }                                                                                                  \
        test::fail(__FILE__, __LINE__, "expected " #type " from " #__VA_ARGS__);                           \
    } while (0)

#define SKIP_UNLESS(cond, reason) \
    do {                          \
        if (!(cond)) throw test::Skipped{reason}; \
    } while (0)
