// JSON values that keep their keys in order, read and written the way Python's json module does it, so case files
// look the same as before and the model's answers are read with the same leniency.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "util.hpp"

namespace json {

struct ParseError : ValueError { using ValueError::ValueError; };

class Value;
using Array = std::vector<Value>;
using Object = Dict<Value>;

class Value {
public:
    enum class Type { Null, Bool, Int, Float, String, Array, Object };

    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool b) : type_(Type::Bool), bool_(b) {}
    template <class T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>, int> = 0>
    Value(T i) : type_(Type::Int), int_(static_cast<int64_t>(i)) {}
    Value(double d) : type_(Type::Float), float_(d) {}
    Value(const char* s) : type_(Type::String), string_(s) {}
    Value(std::string s) : type_(Type::String), string_(std::move(s)) {}
    Value(std::string_view s) : type_(Type::String), string_(s) {}
    Value(Array a) : type_(Type::Array), array_(std::move(a)) {}
    Value(Object o) : type_(Type::Object), object_(std::move(o)) {}
    template <class T>
    Value(const std::vector<T>& items) : type_(Type::Array) {
        for (const auto& item : items) array_.emplace_back(item);
    }
    template <class T>
    Value(const Dict<T>& items) : type_(Type::Object) {
        for (const auto& [key, value] : items) object_[key] = Value(value);
    }
    template <class A, class B>
    Value(const std::pair<A, B>& pair) : type_(Type::Array) {
        array_.emplace_back(pair.first);
        array_.emplace_back(pair.second);
    }

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_bool() const { return type_ == Type::Bool; }
    bool is_int() const { return type_ == Type::Int; }
    bool is_number() const { return type_ == Type::Int || type_ == Type::Float; }
    bool is_string() const { return type_ == Type::String; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_object() const { return type_ == Type::Object; }

    bool as_bool() const;
    int64_t as_int() const;
    double as_number() const;
    const std::string& as_string() const;
    const Array& as_array() const;
    Array& as_array();
    const Object& as_object() const;
    Object& as_object();

    // dict.get(key): the value, or null when it's missing (or this isn't an object).
    const Value& get(const std::string& key) const;
    bool contains(const std::string& key) const { return is_object() && object_.contains(key); }
    // For building: a null value becomes an object (or array) on first use.
    Value& operator[](const std::string& key);
    void push_back(Value v);

    // json.dumps: indent < 0 writes one line with ", " and ": " between items, like Python's default.
    std::string dump(int indent = -1, bool ensure_ascii = true) const;
    static Value parse(std::string_view text);  // json.loads; raises ParseError

    friend bool operator==(const Value& a, const Value& b);

private:
    Type type_ = Type::Null;
    bool bool_ = false;
    int64_t int_ = 0;
    double float_ = 0;
    std::string string_;
    Array array_;
    Object object_;
};

// What Python's built-ins would make of a value that came out of json.loads.
std::string str(const Value& v);     // str(v)
std::string repr(const Value& v);    // repr(v)
bool truthy(const Value& v);         // bool(v)
std::vector<Value> iterate(const Value& v);  // for x in (v or []): a list's items, a dict's keys, a string's characters

}  // namespace json
