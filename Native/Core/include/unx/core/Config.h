#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace unx
{
// Quality parameters (ARCHITECTURE_KO.md 6, 7.0): sample counts, rates, update periods and resolutions live in one
// file, Config/quality.toml. Code never supplies a default for a quality parameter: a missing key is an error, so a
// value cannot be lowered silently. Every performance report carries hash(), the SHA-256 of the canonical effective
// values (file plus command-line overrides).
//
// Accepted syntax is a TOML subset: [section] / [a.b] headers, key = value, # comments, values that are integers,
// floats, booleans, "strings", or single-line arrays of those.
class QualityConfig
{
public:
    struct Value
    {
        enum class Kind { Integer, Float, Bool, String, Array } kind = Kind::Integer;
        int64_t integer = 0;
        double number = 0;
        bool boolean = false;
        std::string text;
        std::vector<Value> items;
        std::string canonical() const;
    };

    static QualityConfig load(const std::filesystem::path& path);
    // Every <name>.toml of a directory (Config/quality): a file may define only keys under "<name>." so each track
    // owns its own file (INTERFACES_KO.md 9). The hash covers the merged set.
    static QualityConfig loadDirectory(const std::filesystem::path& directory);
    static QualityConfig parse(std::string_view text, const std::string& origin);

    // "section.key=value" with the value in TOML syntax; recorded in the hash like any file value.
    // An override of output.tier applies that tier's file (applyTier): give it before the keys it should not undo.
    void applyOverride(std::string_view assignment);
    // Quality tiers (the reference's scalability groups): <directory>/tiers/<output.tier>.toml is a set of values laid
    // over the directory's files - a tier is the files' values with these differences. loadDirectory applies the tier
    // the files name (output.tier; "epic" needs no file: it is the files as they are). Every key of a tier file must
    // exist in the directory's files.
    void applyTier();

    int64_t integer(std::string_view key) const;
    double number(std::string_view key) const;  // accepts integers too
    bool boolean(std::string_view key) const;
    std::string string(std::string_view key) const;
    std::vector<std::string> strings(std::string_view key) const;
    std::vector<double> numbers(std::string_view key) const;
    bool has(std::string_view key) const { return m_values.count(std::string(key)) != 0; }

    std::string canonical() const;  // sorted "key = value" lines
    std::string hash() const;       // SHA-256 hex of canonical()
    std::string shortHash() const { return hash().substr(0, 16); }
    // Keys present in the file that the program never read (typos or dead parameters).
    std::vector<std::string> unreadKeys() const;
    const std::string& origin() const { return m_origin; }

private:
    const Value& get(std::string_view key, Value::Kind kind) const;
    struct ReadLog
    {
        std::mutex mutex;
        std::set<std::string> keys;
    };
    std::map<std::string, Value> m_values;
    std::string m_origin;
    std::unique_ptr<ReadLog> m_read = std::make_unique<ReadLog>();
};
} // namespace unx
