#include "json.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace json {

namespace {

const Value NONE;

[[noreturn]] void wrong_type(const char* wanted) { throw ValueError(std::string("expected a JSON ") + wanted); }

void hex4(std::string& out, unsigned v) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "\\u%04x", v);
    out += buf;
}

void dump_string(std::string& out, std::string_view s, bool ensure_ascii) {
    out += '"';
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = s[i];
        if (c < 0x80) {
            ++i;
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                default:
                    if (c < 0x20 || (ensure_ascii && c == 0x7f)) hex4(out, c);
                    else out += static_cast<char>(c);
            }
            continue;
        }
        size_t start = i;
        uint32_t cp = 0;
        bool ok = utf8::decode(s, i, cp);
        if (!ok) cp = 0xFFFD;
        if (ensure_ascii) {
            if (cp >= 0x10000) {
                cp -= 0x10000;
                hex4(out, 0xD800 + (cp >> 10));
                hex4(out, 0xDC00 + (cp & 0x3FF));
            } else {
                hex4(out, cp);
            }
        } else if (ok) {
            out.append(s.substr(start, i - start));
        } else {
            utf8::append(out, 0xFFFD);
        }
    }
    out += '"';
}

std::string float_text(double d) {
    if (std::isnan(d)) return "NaN";
    if (std::isinf(d)) return d < 0 ? "-Infinity" : "Infinity";
    return py::float_repr(d);
}

void dump_value(std::string& out, const Value& v, int indent, bool ensure_ascii, int level) {
    auto newline = [&](int depth) {
        if (indent >= 0) {
            out += '\n';
            out.append(static_cast<size_t>(depth * indent), ' ');
        }
    };
    switch (v.type()) {
        case Value::Type::Null: out += "null"; break;
        case Value::Type::Bool: out += v.as_bool() ? "true" : "false"; break;
        case Value::Type::Int: out += std::to_string(v.as_int()); break;
        case Value::Type::Float: out += float_text(v.as_number()); break;
        case Value::Type::String: dump_string(out, v.as_string(), ensure_ascii); break;
        case Value::Type::Array: {
            const auto& items = v.as_array();
            if (items.empty()) {
                out += "[]";
                break;
            }
            out += '[';
            for (size_t k = 0; k < items.size(); ++k) {
                if (k) out += indent < 0 ? ", " : ",";
                newline(level + 1);
                dump_value(out, items[k], indent, ensure_ascii, level + 1);
            }
            newline(level);
            out += ']';
            break;
        }
        case Value::Type::Object: {
            const auto& items = v.as_object();
            if (items.empty()) {
                out += "{}";
                break;
            }
            out += '{';
            bool first = true;
            for (const auto& [key, value] : items) {
                if (!first) out += indent < 0 ? ", " : ",";
                first = false;
                newline(level + 1);
                dump_string(out, key, ensure_ascii);
                out += ": ";
                dump_value(out, value, indent, ensure_ascii, level + 1);
            }
            newline(level);
            out += '}';
            break;
        }
    }
}

class Parser {
public:
    explicit Parser(std::string_view s) : s_(s) {}

    Value document() {
        skip();
        Value v = value(0);
        skip();
        if (i_ != s_.size()) fail("Extra data");
        return v;
    }

private:
    [[noreturn]] void fail(const char* what) const {
        size_t line = 1, col = 1;
        for (size_t k = 0; k < i_ && k < s_.size(); ++k) {
            if (s_[k] == '\n') {
                ++line;
                col = 1;
            } else {
                ++col;
            }
        }
        throw ParseError(py::format("%s: line %zu column %zu (char %zu)", what, line, col, i_));
    }

    void skip() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) ++i_;
    }

    bool literal(std::string_view word) {
        if (s_.substr(i_, word.size()) != word) return false;
        i_ += word.size();
        return true;
    }

    Value value(int depth) {
        if (depth > 500) fail("Too deeply nested");
        if (i_ >= s_.size()) fail("Expecting value");
        char c = s_[i_];
        if (c == '{') return object(depth);
        if (c == '[') return array(depth);
        if (c == '"') return Value(string());
        if (literal("null")) return Value();
        if (literal("true")) return Value(true);
        if (literal("false")) return Value(false);
        if (literal("NaN")) return Value(std::numeric_limits<double>::quiet_NaN());
        if (literal("Infinity")) return Value(std::numeric_limits<double>::infinity());
        if (literal("-Infinity")) return Value(-std::numeric_limits<double>::infinity());
        if (c == '-' || (c >= '0' && c <= '9')) return number();
        fail("Expecting value");
    }

    bool digit_at(size_t k) const { return k < s_.size() && s_[k] >= '0' && s_[k] <= '9'; }

    Value number() {
        // -?(0|[1-9]\d*)(\.\d+)?([eE][-+]?\d+)?, as Python's scanner reads it
        size_t start = i_, k = i_;
        if (s_[k] == '-') ++k;
        if (!digit_at(k)) fail("Expecting value");
        if (s_[k] == '0') ++k;
        else
            while (digit_at(k)) ++k;
        bool is_float = false;
        if (k < s_.size() && s_[k] == '.' && digit_at(k + 1)) {
            is_float = true;
            k += 1;
            while (digit_at(k)) ++k;
        }
        if (k < s_.size() && (s_[k] == 'e' || s_[k] == 'E')) {
            size_t e = k + 1;
            if (e < s_.size() && (s_[e] == '+' || s_[e] == '-')) ++e;
            if (digit_at(e)) {
                is_float = true;
                k = e;
                while (digit_at(k)) ++k;
            }
        }
        i_ = k;
        std::string text(s_.substr(start, k - start));
        if (!is_float) {
            int64_t n = 0;
            auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), n);
            if (ec == std::errc() && ptr == text.data() + text.size()) return Value(n);
        }
        return Value(std::strtod(text.c_str(), nullptr));
    }

    unsigned hex(size_t at) {
        if (at + 4 > s_.size()) fail("Invalid \\uXXXX escape");
        unsigned v = 0;
        for (size_t k = at; k < at + 4; ++k) {
            char c = s_[k];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else fail("Invalid \\uXXXX escape");
        }
        return v;
    }

    std::string string() {
        size_t start = i_++;
        std::string out;
        while (true) {
            if (i_ >= s_.size()) {
                i_ = start;
                fail("Unterminated string starting at");
            }
            char c = s_[i_++];
            if (c == '"') return out;
            if (static_cast<unsigned char>(c) < 0x20) {
                --i_;
                fail("Invalid control character at");
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i_ >= s_.size()) fail("Unterminated string starting at");
            char e = s_[i_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    uint32_t cp = hex(i_);
                    i_ += 4;
                    if (cp >= 0xD800 && cp <= 0xDBFF && s_.substr(i_, 2) == "\\u") {
                        unsigned low = hex(i_ + 2);
                        if (low >= 0xDC00 && low <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                            i_ += 6;
                        }
                    }
                    if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;  // a lone surrogate has no UTF-8 form
                    utf8::append(out, cp);
                    break;
                }
                default: --i_; fail("Invalid \\escape");
            }
        }
    }

    Value object(int depth) {
        ++i_;
        Object obj;
        skip();
        if (i_ < s_.size() && s_[i_] == '}') {
            ++i_;
            return Value(std::move(obj));
        }
        while (true) {
            skip();
            if (i_ >= s_.size() || s_[i_] != '"') fail("Expecting property name enclosed in double quotes");
            std::string key = string();
            skip();
            if (i_ >= s_.size() || s_[i_] != ':') fail("Expecting ':' delimiter");
            ++i_;
            skip();
            obj[key] = value(depth + 1);
            skip();
            if (i_ < s_.size() && s_[i_] == ',') {
                ++i_;
                continue;
            }
            if (i_ < s_.size() && s_[i_] == '}') {
                ++i_;
                return Value(std::move(obj));
            }
            fail("Expecting ',' delimiter");
        }
    }

    Value array(int depth) {
        ++i_;
        Array items;
        skip();
        if (i_ < s_.size() && s_[i_] == ']') {
            ++i_;
            return Value(std::move(items));
        }
        while (true) {
            skip();
            items.push_back(value(depth + 1));
            skip();
            if (i_ < s_.size() && s_[i_] == ',') {
                ++i_;
                continue;
            }
            if (i_ < s_.size() && s_[i_] == ']') {
                ++i_;
                return Value(std::move(items));
            }
            fail("Expecting ',' delimiter");
        }
    }

    std::string_view s_;
    size_t i_ = 0;
};

}  // namespace

bool Value::as_bool() const {
    if (type_ != Type::Bool) wrong_type("boolean");
    return bool_;
}

int64_t Value::as_int() const {
    if (type_ == Type::Int) return int_;
    if (type_ == Type::Bool) return bool_;
    wrong_type("integer");
}

double Value::as_number() const {
    if (type_ == Type::Float) return float_;
    if (type_ == Type::Int) return static_cast<double>(int_);
    if (type_ == Type::Bool) return bool_;
    wrong_type("number");
}

const std::string& Value::as_string() const {
    if (type_ != Type::String) wrong_type("string");
    return string_;
}

const Array& Value::as_array() const {
    if (type_ != Type::Array) wrong_type("list");
    return array_;
}

Array& Value::as_array() {
    if (type_ != Type::Array) wrong_type("list");
    return array_;
}

const Object& Value::as_object() const {
    if (type_ != Type::Object) wrong_type("object");
    return object_;
}

Object& Value::as_object() {
    if (type_ != Type::Object) wrong_type("object");
    return object_;
}

const Value& Value::get(const std::string& key) const {
    if (type_ != Type::Object) return NONE;
    const Value* found = object_.find(key);
    return found ? *found : NONE;
}

Value& Value::operator[](const std::string& key) {
    if (type_ == Type::Null) type_ = Type::Object;
    return as_object()[key];
}

void Value::push_back(Value v) {
    if (type_ == Type::Null) type_ = Type::Array;
    as_array().push_back(std::move(v));
}

std::string Value::dump(int indent, bool ensure_ascii) const {
    std::string out;
    dump_value(out, *this, indent, ensure_ascii, 0);
    return out;
}

Value Value::parse(std::string_view text) { return Parser(text).document(); }

bool operator==(const Value& a, const Value& b) {
    auto numeric = [](const Value& v) { return v.is_bool() || v.is_number(); };
    if (numeric(a) && numeric(b)) {  // as in Python, True == 1 == 1.0
        if (a.type_ == Value::Type::Float || b.type_ == Value::Type::Float) return a.as_number() == b.as_number();
        return a.as_int() == b.as_int();
    }
    if (a.type_ != b.type_) return false;
    switch (a.type_) {
        case Value::Type::Null: return true;
        case Value::Type::String: return a.string_ == b.string_;
        case Value::Type::Array: return a.array_ == b.array_;
        case Value::Type::Object: return a.object_ == b.object_;
        default: return false;
    }
}

std::string str(const Value& v) {
    switch (v.type()) {
        case Value::Type::Null: return "None";
        case Value::Type::Bool: return v.as_bool() ? "True" : "False";
        case Value::Type::Int: return std::to_string(v.as_int());
        case Value::Type::Float: return py::float_repr(v.as_number());
        case Value::Type::String: return v.as_string();
        case Value::Type::Array: {
            std::vector<std::string> parts;
            for (const auto& item : v.as_array()) parts.push_back(repr(item));
            return "[" + py::join(parts, ", ") + "]";
        }
        case Value::Type::Object: {
            std::vector<std::string> parts;
            for (const auto& [key, value] : v.as_object()) parts.push_back(py::repr(key) + ": " + repr(value));
            return "{" + py::join(parts, ", ") + "}";
        }
    }
    return "";
}

std::string repr(const Value& v) { return v.is_string() ? py::repr(v.as_string()) : str(v); }

bool truthy(const Value& v) {
    switch (v.type()) {
        case Value::Type::Null: return false;
        case Value::Type::Bool: return v.as_bool();
        case Value::Type::Int: return v.as_int() != 0;
        case Value::Type::Float: return v.as_number() != 0;
        case Value::Type::String: return !v.as_string().empty();
        case Value::Type::Array: return !v.as_array().empty();
        case Value::Type::Object: return !v.as_object().empty();
    }
    return false;
}

std::vector<Value> iterate(const Value& v) {
    std::vector<Value> out;
    if (v.is_array()) return v.as_array();
    if (v.is_object()) {
        for (const auto& key : v.as_object().keys()) out.emplace_back(key);
    } else if (v.is_string()) {
        const std::string& s = v.as_string();
        for (size_t i = 0; i < s.size();) {
            size_t start = i;
            uint32_t cp;
            utf8::decode(s, i, cp);
            out.emplace_back(s.substr(start, i - start));
        }
    }
    return out;
}

}  // namespace json
