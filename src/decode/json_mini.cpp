#include "decode/json_mini.h"
#include <cstdlib>
#include <cstring>

namespace json {
namespace {

const char* skipWS(const char* p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    return p;
}

const char* parseString(const char* p, std::string& out) {
    if (*p != '"') return nullptr;
    ++p;
    while (*p && *p != '"') {
        if (*p == '\\') { ++p; if (!*p) return nullptr; }
        out += *p++;
    }
    if (*p != '"') return nullptr;
    return p + 1;
}

const char* parseNum(const char* p, double& out) {
    char* end;
    out = std::strtod(p, &end);
    return (end > p) ? end : nullptr;
}

const char* parseValue(const char* p, Value& out);

const char* parseObj(const char* p, Object& out) {
    p = skipWS(p);
    if (*p != '{') return nullptr;
    p = skipWS(p + 1);
    if (*p == '}') return p + 1;
    for (;;) {
        p = skipWS(p);
        std::string key;
        p = parseString(p, key);
        if (!p) return nullptr;
        p = skipWS(p);
        if (*p != ':') return nullptr;
        p = skipWS(p + 1);
        Value v;
        p = parseValue(p, v);
        if (!p) return nullptr;
        out[key] = std::move(v);
        p = skipWS(p);
        if (*p == ',') { ++p; continue; }
        if (*p == '}') return p + 1;
        return nullptr;
    }
}

const char* parseArr(const char* p, Array& out) {
    p = skipWS(p);
    if (*p != '[') return nullptr;
    p = skipWS(p + 1);
    if (*p == ']') return p + 1;
    for (;;) {
        Value v;
        p = parseValue(skipWS(p), v);
        if (!p) return nullptr;
        out.push_back(std::move(v));
        p = skipWS(p);
        if (*p == ',') { ++p; continue; }
        if (*p == ']') return p + 1;
        return nullptr;
    }
}

const char* parseValue(const char* p, Value& out) {
    p = skipWS(p);
    if (*p == '"') { out.tag = Value::STR; return parseString(p, out.s); }
    if (*p == '{') { out.tag = Value::OBJ; return parseObj(p, out.o); }
    if (*p == '[') { out.tag = Value::ARR; return parseArr(p, out.a); }
    if ((*p >= '0' && *p <= '9') || *p == '-' || *p == '+') {
        out.tag = Value::NUM;
        return parseNum(p, out.d);
    }
    if (*p == 'n' && std::strncmp(p, "null", 4) == 0) { out.tag = Value::NIL; return p + 4; }
    if (*p == 't' && std::strncmp(p, "true", 4) == 0) { out.tag = Value::NIL; return p + 4; }
    if (*p == 'f' && std::strncmp(p, "false", 5) == 0) { out.tag = Value::NIL; return p + 5; }
    return nullptr;
}

} // namespace

Value parse(const char* s, int* errPos) {
    Value v;
    const char* end = parseValue(s, v);
    if (!end && errPos) *errPos = (int)(skipWS(s) - s);
    return v;
}

} // namespace json
