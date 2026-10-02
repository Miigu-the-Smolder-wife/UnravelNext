#include "unx/core/Config.h"

#include "unx/core/File.h"
#include "unx/core/Log.h"
#include "unx/core/Sha256.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>

namespace unx
{
namespace
{
struct Cursor
{
    std::string_view s;
    size_t i = 0;
    int line = 1;
    const std::string* origin;
    [[noreturn]] void error(const char* what) const { fail("%s:%d: %s", origin->c_str(), line, what); }
    void skipSpace() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i; }
    bool atLineEnd()
    {
        skipSpace();
        return i >= s.size() || s[i] == '\n' || s[i] == '\r' || s[i] == '#';
    }
};

QualityConfig::Value parseValue(Cursor& c)
{
    using V = QualityConfig::Value;
    c.skipSpace();
    if (c.i >= c.s.size()) c.error("missing value");
    V v;
    char ch = c.s[c.i];
    if (ch == '"')
    {
        v.kind = V::Kind::String;
        ++c.i;
        while (c.i < c.s.size() && c.s[c.i] != '"')
        {
            if (c.s[c.i] == '\n') c.error("unterminated string");
            if (c.s[c.i] == '\\')
            {
                if (++c.i >= c.s.size()) c.error("bad escape");
                char e = c.s[c.i];
                v.text += e == 'n' ? '\n' : e == 't' ? '\t' : e;
            }
            else v.text += c.s[c.i];
            ++c.i;
        }
        if (c.i >= c.s.size()) c.error("unterminated string");
        ++c.i;
        return v;
    }
    if (ch == '[')
    {
        v.kind = V::Kind::Array;
        ++c.i;
        for (;;)
        {
            c.skipSpace();
            if (c.i < c.s.size() && c.s[c.i] == ']') { ++c.i; break; }
            v.items.push_back(parseValue(c));
            c.skipSpace();
            if (c.i < c.s.size() && c.s[c.i] == ',') { ++c.i; continue; }
            if (c.i < c.s.size() && c.s[c.i] == ']') { ++c.i; break; }
            c.error("expected ',' or ']' in array");
        }
        return v;
    }
    size_t start = c.i;
    while (c.i < c.s.size() && c.s[c.i] != ',' && c.s[c.i] != ']' && c.s[c.i] != '#' && c.s[c.i] != '\n' && c.s[c.i] != '\r' && c.s[c.i] != ' ' && c.s[c.i] != '\t') ++c.i;
    std::string token(c.s.substr(start, c.i - start));
    if (token == "true" || token == "false")
    {
        v.kind = V::Kind::Bool;
        v.boolean = token == "true";
        return v;
    }
    std::string digits;
    for (char d : token) if (d != '_') digits += d;
    bool isFloat = digits.find_first_of(".eE") != std::string::npos;
    if (!isFloat)
    {
        int64_t x = 0;
        auto r = std::from_chars(digits.data(), digits.data() + digits.size(), x);
        if (r.ec != std::errc() || r.ptr != digits.data() + digits.size()) c.error("bad value (expected number, bool, string or array)");
        v.kind = V::Kind::Integer;
        v.integer = x;
        v.number = (double)x;
        return v;
    }
    double x = 0;
    auto r = std::from_chars(digits.data(), digits.data() + digits.size(), x);
    if (r.ec != std::errc() || r.ptr != digits.data() + digits.size() || !std::isfinite(x)) c.error("bad float");
    v.kind = V::Kind::Float;
    v.number = x;
    return v;
}

std::string readKey(Cursor& c)
{
    c.skipSpace();
    size_t start = c.i;
    while (c.i < c.s.size() && (std::isalnum((unsigned char)c.s[c.i]) || c.s[c.i] == '_' || c.s[c.i] == '-' || c.s[c.i] == '.')) ++c.i;
    if (c.i == start) c.error("expected key");
    return std::string(c.s.substr(start, c.i - start));
}

const char* kindName(QualityConfig::Value::Kind k)
{
    switch (k)
    {
    case QualityConfig::Value::Kind::Integer: return "integer";
    case QualityConfig::Value::Kind::Float: return "float";
    case QualityConfig::Value::Kind::Bool: return "bool";
    case QualityConfig::Value::Kind::String: return "string";
    case QualityConfig::Value::Kind::Array: return "array";
    }
    return "?";
}
} // namespace

std::string QualityConfig::Value::canonical() const
{
    switch (kind)
    {
    case Kind::Integer: return std::to_string(integer);
    case Kind::Float:
    {
        char buf[64];
        auto r = std::to_chars(buf, buf + sizeof buf, number);  // shortest round-trip form
        std::string s(buf, r.ptr);
        if (s.find_first_of(".eE") == std::string::npos) s += ".0";
        return s;
    }
    case Kind::Bool: return boolean ? "true" : "false";
    case Kind::String:
    {
        std::string s = "\"";
        for (char ch : text) { if (ch == '"' || ch == '\\') s += '\\'; s += ch; }
        return s + "\"";
    }
    case Kind::Array:
    {
        std::string s = "[";
        for (size_t i = 0; i < items.size(); ++i) s += (i ? ", " : "") + items[i].canonical();
        return s + "]";
    }
    }
    return {};
}

QualityConfig QualityConfig::load(const std::filesystem::path& path)
{
    return parse(readTextFile(path), path.string());
}

QualityConfig QualityConfig::loadDirectory(const std::filesystem::path& directory)
{
    if (!std::filesystem::is_directory(directory)) fail("quality directory %s does not exist", directory.string().c_str());
    std::vector<std::filesystem::path> files;
    for (const auto& e : std::filesystem::directory_iterator(directory))
        if (e.is_regular_file() && e.path().extension() == ".toml") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    QualityConfig merged;
    merged.m_origin = directory.string();
    for (const auto& f : files)
    {
        QualityConfig part = load(f);
        const std::string prefix = f.stem().string() + ".";
        for (auto& [key, value] : part.m_values)
        {
            if (key.rfind(prefix, 0) != 0) fail("%s: key '%s' is outside the file's namespace '%s*'", f.string().c_str(), key.c_str(), prefix.c_str());
            merged.m_values[key] = std::move(value);
        }
    }
    merged.applyTier();
    return merged;
}

void QualityConfig::applyTier()
{
    const auto it = m_values.find("output.tier");
    if (it == m_values.end()) return;
    if (it->second.kind != Value::Kind::String) fail("output.tier must be a string (a file name under %s/tiers)", m_origin.c_str());
    const std::string tier = it->second.text;
    {
        std::lock_guard lock(m_read->mutex);
        m_read->keys.insert(it->first);  // (read here, not through get: not an unread key)
    }
    const std::filesystem::path file = std::filesystem::path(m_origin) / "tiers" / (tier + ".toml");
    if (!std::filesystem::is_regular_file(file))
    {
        if (tier == "epic") return;  // (the files as they are)
        fail("output.tier = \"%s\": %s does not exist", tier.c_str(), file.string().c_str());
    }
    QualityConfig part = load(file);
    for (auto& [key, value] : part.m_values)
    {
        if (key == "output.tier") fail("%s: a tier file does not name a tier", file.string().c_str());
        if (!m_values.count(key)) fail("%s: key '%s' is not a quality key of %s", file.string().c_str(), key.c_str(), m_origin.c_str());
        m_values[key] = std::move(value);
    }
}

QualityConfig QualityConfig::parse(std::string_view text, const std::string& origin)
{
    QualityConfig cfg;
    cfg.m_origin = origin;
    Cursor c{ text, 0, 1, &cfg.m_origin };
    std::string section;
    while (c.i < c.s.size())
    {
        c.skipSpace();
        if (c.i >= c.s.size()) break;
        char ch = c.s[c.i];
        if (ch == '\n') { ++c.i; ++c.line; continue; }
        if (ch == '\r' || ch == '#')
        {
            while (c.i < c.s.size() && c.s[c.i] != '\n') ++c.i;
            continue;
        }
        if (ch == '[')
        {
            ++c.i;
            section = readKey(c);
            c.skipSpace();
            if (c.i >= c.s.size() || c.s[c.i] != ']') c.error("expected ']'");
            ++c.i;
            if (!c.atLineEnd()) c.error("trailing text after section header");
            continue;
        }
        std::string key = readKey(c);
        c.skipSpace();
        if (c.i >= c.s.size() || c.s[c.i] != '=') c.error("expected '='");
        ++c.i;
        Value v = parseValue(c);
        if (!c.atLineEnd()) c.error("trailing text after value");
        std::string full = section.empty() ? key : section + "." + key;
        if (cfg.m_values.count(full)) c.error(("duplicate key " + full).c_str());
        cfg.m_values[full] = std::move(v);
    }
    return cfg;
}

void QualityConfig::applyOverride(std::string_view assignment)
{
    size_t eq = assignment.find('=');
    if (eq == std::string_view::npos) fail("override '%.*s' is not key=value", (int)assignment.size(), assignment.data());
    std::string key(assignment.substr(0, eq));
    while (!key.empty() && key.back() == ' ') key.pop_back();
    if (!m_values.count(key)) fail("override of unknown quality key '%s' (add it to %s first)", key.c_str(), m_origin.c_str());
    std::string origin = "override " + key;
    // (a string key takes a bare word: quotes do not survive every shell)
    std::string text(assignment.substr(eq + 1));
    while (!text.empty() && text.front() == ' ') text.erase(text.begin());
    if (m_values[key].kind == Value::Kind::String && !text.empty() && text.front() != '"') text = "\"" + text + "\"";
    Cursor c{ text, 0, 1, &origin };
    Value v = parseValue(c);
    if (!c.atLineEnd()) c.error("trailing text");
    m_values[key] = std::move(v);
    if (key == "output.tier") applyTier();
}

const QualityConfig::Value& QualityConfig::get(std::string_view key, Value::Kind kind) const
{
    auto it = m_values.find(std::string(key));
    if (it == m_values.end()) fail("quality parameter '%.*s' is missing from %s (no code default exists)", (int)key.size(), key.data(), m_origin.c_str());
    const Value& v = it->second;
    bool ok = v.kind == kind || (kind == Value::Kind::Float && v.kind == Value::Kind::Integer);
    if (!ok) fail("quality parameter '%.*s' is %s, expected %s", (int)key.size(), key.data(), kindName(v.kind), kindName(kind));
    {
        std::lock_guard lock(m_read->mutex);
        m_read->keys.insert(it->first);
    }
    return v;
}

int64_t QualityConfig::integer(std::string_view key) const { return get(key, Value::Kind::Integer).integer; }
double QualityConfig::number(std::string_view key) const { return get(key, Value::Kind::Float).number; }
bool QualityConfig::boolean(std::string_view key) const { return get(key, Value::Kind::Bool).boolean; }
std::string QualityConfig::string(std::string_view key) const { return get(key, Value::Kind::String).text; }

std::vector<std::string> QualityConfig::strings(std::string_view key) const
{
    std::vector<std::string> out;
    for (const Value& v : get(key, Value::Kind::Array).items)
    {
        if (v.kind != Value::Kind::String) fail("quality parameter '%.*s' must be an array of strings", (int)key.size(), key.data());
        out.push_back(v.text);
    }
    return out;
}

std::vector<double> QualityConfig::numbers(std::string_view key) const
{
    std::vector<double> out;
    for (const Value& v : get(key, Value::Kind::Array).items)
    {
        if (v.kind != Value::Kind::Integer && v.kind != Value::Kind::Float) fail("quality parameter '%.*s' must be an array of numbers", (int)key.size(), key.data());
        out.push_back(v.number);
    }
    return out;
}

std::string QualityConfig::canonical() const
{
    std::string s;
    for (const auto& [k, v] : m_values) s += k + " = " + v.canonical() + "\n";
    return s;
}

std::string QualityConfig::hash() const { return Sha256::hex(canonical()); }

std::vector<std::string> QualityConfig::unreadKeys() const
{
    std::lock_guard lock(m_read->mutex);
    std::vector<std::string> out;
    for (const auto& [k, v] : m_values) if (!m_read->keys.count(k)) out.push_back(k);
    return out;
}
} // namespace unx
