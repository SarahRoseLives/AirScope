// Minimal JSON parser for band plan files.  Only handles the subset of JSON
// that our .json band plan schema uses — no unicode escapes, no nested arrays,
// no null/true/false/bool.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <map>

namespace json {

struct Value;
using Object = std::map<std::string, Value>;
using Array  = std::vector<Value>;

struct Value {
    enum { NIL, STR, NUM, OBJ, ARR } tag = NIL;
    std::string s;
    double d = 0.0;
    Object o;
    Array  a;
    Value() = default;
    Value(const std::string& v)   : tag(STR), s(v) {}
    Value(double v)               : tag(NUM), d(v) {}
    Value(const Object& v)        : tag(OBJ), o(v) {}
    Value(const Array& v)         : tag(ARR), a(v) {}
    const Value& operator[](const char* key) const {
        static Value nil;
        auto it = o.find(key);
        return it != o.end() ? it->second : nil;
    }
};

// Parse a single JSON value from the string.  Returns NIL on error and sets
// *errPos to the approximate error byte offset (if non-null).
Value parse(const char* s, int* errPos = nullptr);

} // namespace json
