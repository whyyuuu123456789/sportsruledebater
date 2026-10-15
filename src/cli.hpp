// A small argparse: optional positionals, --options with values, --flags, choices, int and float checks, and -h.

#pragma once

#include <string>
#include <vector>

#include "util.hpp"

namespace cli {

// Thrown to end the program with this exit status: after --help (0) or a usage error (2).
struct Exit {
    int code;
};

class Parser {
public:
    Parser(std::string prog, std::string description) : prog_(std::move(prog)), description_(std::move(description)) {}

    // An optional positional argument (nargs="?").
    Parser& positional(std::string name, std::string help, std::string fallback = "", std::vector<std::string> choices = {});
    // An option that takes a value. type is "str", "int" or "float".
    Parser& option(std::string flag, std::string help, std::string fallback = "", std::string type = "str",
                   std::vector<std::string> choices = {});
    Parser& flag(std::string flag, std::string help);

    void parse(const std::vector<std::string>& args);

    std::string str(const std::string& name) const;
    int integer(const std::string& name) const;
    double number(const std::string& name) const;
    bool given(const std::string& name) const;  // set on the command line
    bool on(const std::string& flag) const { return given(flag); }

    [[noreturn]] void error(const std::string& message) const;
    std::string usage() const;
    std::string help() const;

private:
    struct Arg {
        std::string name, help, fallback, type = "str";
        std::vector<std::string> choices;
        bool positional = false, is_flag = false, given = false;
        std::string value;
    };
    Arg& set(const std::string& name, const std::string& value);
    const Arg& find(const std::string& name) const;
    static std::string metavar(const Arg& a);

    std::string prog_, description_;
    std::vector<Arg> args_;
};

}  // namespace cli
