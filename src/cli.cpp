#include "cli.hpp"

#include <algorithm>
#include <iostream>

namespace cli {

Parser& Parser::positional(std::string name, std::string help, std::string fallback, std::vector<std::string> choices) {
    Arg a;
    a.name = std::move(name);
    a.help = std::move(help);
    a.fallback = std::move(fallback);
    a.choices = std::move(choices);
    a.positional = true;
    args_.push_back(a);
    return *this;
}

Parser& Parser::option(std::string flag, std::string help, std::string fallback, std::string type,
                       std::vector<std::string> choices) {
    Arg a;
    a.name = std::move(flag);
    a.help = std::move(help);
    a.fallback = std::move(fallback);
    a.type = std::move(type);
    a.choices = std::move(choices);
    args_.push_back(a);
    return *this;
}

Parser& Parser::flag(std::string flag, std::string help) {
    Arg a;
    a.name = std::move(flag);
    a.help = std::move(help);
    a.is_flag = true;
    args_.push_back(a);
    return *this;
}

std::string Parser::metavar(const Arg& a) {
    if (!a.choices.empty()) return "{" + py::join(a.choices, ",") + "}";
    if (a.positional) return a.name;
    std::string m = py::upper(py::removeprefix(a.name, "--"));
    std::replace(m.begin(), m.end(), '-', '_');
    return m;
}

std::string Parser::usage() const {
    std::string out = "usage: " + prog_ + " [-h]";
    for (const auto& a : args_)
        if (!a.positional) out += a.is_flag ? " [" + a.name + "]" : " [" + a.name + " " + metavar(a) + "]";
    for (const auto& a : args_)
        if (a.positional) out += " [" + metavar(a) + "]";
    return out;
}

std::string Parser::help() const {
    std::string out = usage() + "\n\n" + description_ + "\n";
    auto row = [](const std::string& left, const std::string& help) {
        std::string line = "  " + left;
        if (help.empty()) return line + "\n";
        return (line.size() < 24 ? py::ljust(line, 24) : line + "\n" + std::string(24, ' ')) + help + "\n";
    };
    bool any = false;
    for (const auto& a : args_)
        if (a.positional) {
            if (!any) out += "\npositional arguments:\n";
            any = true;
            out += row(metavar(a), a.help);
        }
    out += "\noptions:\n" + row("-h, --help", "show this help message and exit");
    for (const auto& a : args_)
        if (!a.positional) out += row(a.is_flag ? a.name : a.name + " " + metavar(a), a.help);
    return out;
}

void Parser::error(const std::string& message) const {
    std::cerr << usage() << "\n" << prog_ << ": error: " << message << std::endl;
    throw Exit{2};
}

Parser::Arg& Parser::set(const std::string& name, const std::string& value) {
    for (auto& a : args_) {
        if (a.name != name) continue;
        std::string label = "argument " + a.name + ": ";
        try {
            if (a.type == "int") py::to_int(value);
            if (a.type == "float") py::to_float(value);
        } catch (const ValueError&) {
            error(label + "invalid " + a.type + " value: " + py::repr(value));
        }
        if (!a.choices.empty() && std::find(a.choices.begin(), a.choices.end(), value) == a.choices.end()) {
            std::string shown = a.type == "str" ? py::repr(value) : value;
            error(label + "invalid choice: " + shown + " (choose from " + py::join(a.choices, ", ") + ")");
        }
        a.value = value;
        a.given = true;
        return a;
    }
    error("unrecognized arguments: " + name);
}

const Parser::Arg& Parser::find(const std::string& name) const {
    for (const auto& a : args_)
        if (a.name == name) return a;
    throw std::logic_error("no argument called " + name);
}

void Parser::parse(const std::vector<std::string>& args) {
    std::vector<std::string> extra;
    size_t next_positional = 0;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "-h" || arg == "--help") {
            std::cout << help();
            throw Exit{0};
        }
        if (arg.size() > 1 && arg[0] == '-' && arg != "--") {
            size_t eq = arg.find('=');
            std::string flag_name = arg.substr(0, eq);
            auto it = std::find_if(args_.begin(), args_.end(), [&](const Arg& a) { return !a.positional && a.name == flag_name; });
            if (it == args_.end()) {
                extra.push_back(arg);
                continue;
            }
            if (it->is_flag) {
                if (eq != std::string::npos) error("argument " + flag_name + ": ignored explicit argument " + py::repr(arg.substr(eq + 1)));
                it->given = true;
            } else if (eq != std::string::npos) {
                set(flag_name, arg.substr(eq + 1));
            } else if (i + 1 < args.size()) {
                set(flag_name, args[++i]);
            } else {
                error("argument " + flag_name + ": expected one argument");
            }
            continue;
        }
        size_t seen = 0;
        bool placed = false;
        for (auto& a : args_) {
            if (!a.positional) continue;
            if (seen++ == next_positional) {
                set(a.name, arg);
                ++next_positional;
                placed = true;
                break;
            }
        }
        if (!placed) extra.push_back(arg);
    }
    if (!extra.empty()) error("unrecognized arguments: " + py::join(extra, " "));
}

std::string Parser::str(const std::string& name) const {
    const Arg& a = find(name);
    return a.given ? a.value : a.fallback;
}

int Parser::integer(const std::string& name) const { return static_cast<int>(py::to_int(str(name))); }

double Parser::number(const std::string& name) const { return py::to_float(str(name)); }

bool Parser::given(const std::string& name) const { return find(name).given; }

}  // namespace cli
