// Runs the tests: build/tests, or build/tests <part of a name> ... to run some of them.

#include <chrono>
#include <iostream>

#include "test.hpp"

int main(int argc, char** argv) {
    std::vector<std::string> wanted(argv + 1, argv + argc);
    int ran = 0, failed = 0, skipped = 0;
    auto started = std::chrono::steady_clock::now();
    for (const auto& t : test::registry()) {
        bool match = wanted.empty();
        for (const auto& w : wanted) match = match || t.name.find(w) != std::string::npos;
        if (!match) continue;
        ++ran;
        std::cerr << t.name << " ... " << std::flush;
        try {
            t.fn();
            std::cerr << "ok" << std::endl;
        } catch (const test::Skipped& s) {
            ++skipped;
            std::cerr << "skipped: " << s.reason << std::endl;
        } catch (const test::Failure& f) {
            ++failed;
            std::cerr << "FAIL\n  " << f.message << std::endl;
        } catch (const std::exception& e) {
            ++failed;
            std::cerr << "ERROR\n  " << e.what() << std::endl;
        }
    }
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::cerr << "\nRan " << ran << " test" << (ran == 1 ? "" : "s") << " in " << py::format("%.3fs", seconds) << "\n\n";
    if (failed) {
        std::cerr << "FAILED (failures=" << failed << ")" << std::endl;
        return 1;
    }
    std::cerr << "OK" << (skipped ? " (skipped=" + std::to_string(skipped) + ")" : "") << std::endl;
    return 0;
}
