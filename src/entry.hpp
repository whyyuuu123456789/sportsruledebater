// What the three programs share: turning argv into arguments, and reporting errors the way Python's sys.exit did.

#pragma once

#include <iostream>
#include <string>
#include <vector>

#include "cli.hpp"
#include "llm.hpp"

inline int run_main(int (*main_fn)(const std::vector<std::string>&), int argc, char** argv) {
    try {
        return main_fn(std::vector<std::string>(argv + 1, argv + argc));
    } catch (const cli::Exit& e) {
        return e.code;
    } catch (const llm::Unavailable& e) {
        std::cerr << e.what() << std::endl;
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << std::endl;
        return 1;
    }
}
