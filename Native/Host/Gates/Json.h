#pragma once
// Minimal strict JSON reader for the host gates' input files (I track): objects, arrays, numbers, strings, true/false/null.
// Any syntax error fails with the byte offset; no extensions (comments, trailing commas, NaN).
#include "unx/core/Log.h"

#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace unx::host::json
{
struct Value
{
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    bool boolean = false;
    double number = 0;
    std::string string;
    std::vector<Value> array;
    std::map<std::string, Value> object;

    bool has(const std::string& key) const { return type == Type::Object && object.count(key) != 0; }
    const Value& at(const std::string& key) const
    {
        if (type != Type::Object) fail("json: '%s' looked up in a non-object", key.c_str());
        const auto it = object.find(key);
        if (it == object.end()) fail("json: missing key '%s'", key.c_str());
        return it->second;
    }
    double num() const
    {
        if (type != Type::Number) fail("json: number expected");
        return number;
    }
    const std::string& str() const
    {
        if (type != Type::String) fail("json: string expected");
        return string;
    }
    const std::vector<Value>& arr() const
    {
        if (type != Type::Array) fail("json: array expected");
        return array;
    }
    // A fixed-length array of numbers.
    template <size_t N>
    void numbers(float (&out)[N]) const
    {
        const std::vector<Value>& a = arr();
        if (a.size() != N) fail("json: array of %zu numbers expected, got %zu", N, a.size());
        for (size_t i = 0; i < N; ++i) out[i] = (float)a[i].num();
    }
};

class Parser
{
public:
    explicit Parser(const std::string& text) : m_text(text) {}
    Value parse()
    {
        Value v = value();
        space();
        if (m_at != m_text.size()) error("trailing characters");
        return v;
    }

private:
    const std::string& m_text;
    size_t m_at = 0;

    [[noreturn]] void error(const char* what) const { fail("json: %s at byte %zu", what, m_at); }
    void space()
    {
        while (m_at < m_text.size() && (m_text[m_at] == ' ' || m_text[m_at] == '\t' || m_text[m_at] == '\n' || m_text[m_at] == '\r')) ++m_at;
    }
    char peek()
    {
        space();
        if (m_at >= m_text.size()) error("unexpected end");
        return m_text[m_at];
    }
    void expect(char c)
    {
        if (peek() != c) error("unexpected character");
        ++m_at;
    }
    void literal(const char* word)
    {
        for (const char* p = word; *p; ++p, ++m_at)
            if (m_at >= m_text.size() || m_text[m_at] != *p) error("bad literal");
    }
    std::string text()
    {
        expect('"');
        std::string s;
        for (;;)
        {
            if (m_at >= m_text.size()) error("unterminated string");
            const char c = m_text[m_at++];
            if (c == '"') return s;
            if ((unsigned char)c < 0x20) error("control character in string");
            if (c != '\\')
            {
                s += c;
                continue;
            }
            if (m_at >= m_text.size()) error("unterminated escape");
            const char e = m_text[m_at++];
            switch (e)
            {
            case '"': s += '"'; break;
            case '\\': s += '\\'; break;
            case '/': s += '/'; break;
            case 'b': s += '\b'; break;
            case 'f': s += '\f'; break;
            case 'n': s += '\n'; break;
            case 'r': s += '\r'; break;
            case 't': s += '\t'; break;
            case 'u':
            {
                if (m_at + 4 > m_text.size()) error("short \\u escape");
                const uint32_t code = (uint32_t)std::strtoul(m_text.substr(m_at, 4).c_str(), nullptr, 16);
                m_at += 4;
                if (code < 0x80) s += (char)code;  // file keys and values are ASCII; others are kept as UTF-8 below
                else if (code < 0x800) { s += (char)(0xC0 | (code >> 6)); s += (char)(0x80 | (code & 0x3F)); }
                else { s += (char)(0xE0 | (code >> 12)); s += (char)(0x80 | ((code >> 6) & 0x3F)); s += (char)(0x80 | (code & 0x3F)); }
                break;
            }
            default: error("bad escape");
            }
        }
    }
    Value value()
    {
        Value v;
        const char c = peek();
        if (c == '{')
        {
            v.type = Value::Type::Object;
            ++m_at;
            if (peek() == '}') { ++m_at; return v; }
            for (;;)
            {
                std::string key = text();
                expect(':');
                if (!v.object.emplace(key, value()).second) error("duplicate key");
                if (peek() == ',') { ++m_at; continue; }
                expect('}');
                return v;
            }
        }
        if (c == '[')
        {
            v.type = Value::Type::Array;
            ++m_at;
            if (peek() == ']') { ++m_at; return v; }
            for (;;)
            {
                v.array.push_back(value());
                if (peek() == ',') { ++m_at; continue; }
                expect(']');
                return v;
            }
        }
        if (c == '"') { v.type = Value::Type::String; v.string = text(); return v; }
        if (c == 't') { literal("true"); v.type = Value::Type::Bool; v.boolean = true; return v; }
        if (c == 'f') { literal("false"); v.type = Value::Type::Bool; return v; }
        if (c == 'n') { literal("null"); return v; }
        // Number (strict JSON grammar; strtod does the conversion).
        const size_t begin = m_at;
        if (m_text[m_at] == '-') ++m_at;
        auto digits = [&] {
            const size_t d = m_at;
            while (m_at < m_text.size() && m_text[m_at] >= '0' && m_text[m_at] <= '9') ++m_at;
            if (m_at == d) error("digit expected");
        };
        digits();
        if (m_at < m_text.size() && m_text[m_at] == '.') { ++m_at; digits(); }
        if (m_at < m_text.size() && (m_text[m_at] == 'e' || m_text[m_at] == 'E'))
        {
            ++m_at;
            if (m_at < m_text.size() && (m_text[m_at] == '+' || m_text[m_at] == '-')) ++m_at;
            digits();
        }
        v.type = Value::Type::Number;
        v.number = std::strtod(m_text.substr(begin, m_at - begin).c_str(), nullptr);
        return v;
    }
};

inline Value parse(const std::string& text) { return Parser(text).parse(); }
} // namespace unx::host::json
