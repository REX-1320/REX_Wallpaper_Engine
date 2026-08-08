#pragma once

// ─── Json ──────────────────────────────────────────────────────────────────
// Minimal dependency-free JSON value, parser, and serializer used by the
// wallpaper runtime command protocol. Not a general-purpose JSON library:
// it supports standard JSON objects, arrays, strings, numbers, booleans,
// and null, with a nesting depth limit for safety.

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace rex::runtime {

class Json {
public:
    using Object = std::map<std::string, Json, std::less<>>;
    using Array  = std::vector<Json>;
    using Value  = std::variant<std::nullptr_t, bool, double, std::string, Array, Object>;

    Json() : m_value(nullptr) {}
    Json(std::nullptr_t) : m_value(nullptr) {}
    Json(bool b) : m_value(b) {}
    Json(int v) : m_value(static_cast<double>(v)) {}
    Json(unsigned v) : m_value(static_cast<double>(v)) {}
    Json(long long v) : m_value(static_cast<double>(v)) {}
    Json(double v) : m_value(v) {}
    Json(float v) : m_value(static_cast<double>(v)) {}
    Json(const char* s) : m_value(std::string(s)) {}
    Json(std::string s) : m_value(std::move(s)) {}
    Json(Array a) : m_value(std::move(a)) {}
    Json(Object o) : m_value(std::move(o)) {}

    // ─── Type queries ────────────────────────────────────────────────────
    bool IsNull()   const { return std::holds_alternative<std::nullptr_t>(m_value); }
    bool IsBool()   const { return std::holds_alternative<bool>(m_value); }
    bool IsNumber() const { return std::holds_alternative<double>(m_value); }
    bool IsString() const { return std::holds_alternative<std::string>(m_value); }
    bool IsArray()  const { return std::holds_alternative<Array>(m_value); }
    bool IsObject() const { return std::holds_alternative<Object>(m_value); }

    // ─── Accessors ───────────────────────────────────────────────────────
    bool AsBool(bool def = false) const {
        if (auto* b = std::get_if<bool>(&m_value)) return *b;
        if (auto* n = std::get_if<double>(&m_value)) return *n != 0.0;
        if (auto* s = std::get_if<std::string>(&m_value)) {
            return *s == "true" || *s == "1";
        }
        return def;
    }

    double AsNumber(double def = 0.0) const {
        if (auto* n = std::get_if<double>(&m_value)) return *n;
        if (auto* b = std::get_if<bool>(&m_value)) return *b ? 1.0 : 0.0;
        if (auto* s = std::get_if<std::string>(&m_value)) return std::atof(s->c_str());
        return def;
    }

    int AsInt(int def = 0) const { return static_cast<int>(AsNumber(static_cast<double>(def))); }
    float AsFloat(float def = 0.0f) const { return static_cast<float>(AsNumber(static_cast<double>(def))); }

    const std::string& AsString() const {
        static const std::string kEmpty;
        if (auto* s = std::get_if<std::string>(&m_value)) return *s;
        return kEmpty;
    }

    std::string AsString(const std::string& def) const {
        if (auto* s = std::get_if<std::string>(&m_value)) return *s;
        return def;
    }

    const Array& AsArray() const {
        static const Array kEmpty;
        if (auto* a = std::get_if<Array>(&m_value)) return *a;
        return kEmpty;
    }

    const Object& AsObject() const {
        static const Object kEmpty;
        if (auto* o = std::get_if<Object>(&m_value)) return *o;
        return kEmpty;
    }

    // ─── Object access ───────────────────────────────────────────────────
    bool Has(std::string_view key) const {
        const auto* o = std::get_if<Object>(&m_value);
        return o && o->find(key) != o->end();
    }

    const Json& operator[](std::string_view key) const {
        static const Json kNull;
        const auto* o = std::get_if<Object>(&m_value);
        if (!o) return kNull;
        auto it = o->find(key);
        return it == o->end() ? kNull : it->second;
    }

    Json& operator[](std::string key) {
        auto& o = std::get<Object>(m_value);
        return o[key];
    }

    // ─── Serialization ───────────────────────────────────────────────────
    std::string Dump() const {
        std::string out;
        out.reserve(256);
        DumpInto(out);
        return out;
    }

    // ─── Parsing ─────────────────────────────────────────────────────────
    static std::optional<Json> Parse(std::string_view text) {
        Parser parser{text};
        auto value = parser.ParseValue();
        if (!value) return std::nullopt;
        parser.SkipWs(); // allow EOF (returns false at end — that's fine)
        if (parser.pos != text.size()) return std::nullopt;
        return value;
    }

private:
    static void AppendUtf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    static void WriteString(std::string& out, std::string_view s) {
        out.push_back('"');
        for (char c : s) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b";  break;
                case '\f': out += "\\f";  break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out += buf;
                    } else {
                        out.push_back(c);
                    }
            }
        }
        out.push_back('"');
    }

    void DumpInto(std::string& out) const {
        if (auto* n = std::get_if<std::nullptr_t>(&m_value)) {
            (void)n;
            out += "null";
        } else if (auto* b = std::get_if<bool>(&m_value)) {
            out += *b ? "true" : "false";
        } else if (auto* d = std::get_if<double>(&m_value)) {
            if (std::isfinite(*d)) {
                if (*d == std::floor(*d) && std::fabs(*d) < 1e15) {
                    out += std::to_string(static_cast<long long>(*d));
                } else {
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "%.10g", *d);
                    out += buf;
                }
            } else {
                out += "null";
            }
        } else if (auto* s = std::get_if<std::string>(&m_value)) {
            WriteString(out, *s);
        } else if (auto* a = std::get_if<Array>(&m_value)) {
            out.push_back('[');
            for (size_t i = 0; i < a->size(); ++i) {
                if (i) out.push_back(',');
                (*a)[i].DumpInto(out);
            }
            out.push_back(']');
        } else if (auto* o = std::get_if<Object>(&m_value)) {
            out.push_back('{');
            bool first = true;
            for (const auto& [k, v] : *o) {
                if (!first) out.push_back(',');
                first = false;
                WriteString(out, k);
                out.push_back(':');
                v.DumpInto(out);
            }
            out.push_back('}');
        }
    }

    struct Parser {
        std::string_view text;
        size_t pos = 0;
        int depth = 0;
        static constexpr int kMaxDepth = 64;

        bool SkipWs() {
            while (pos < text.size() &&
                   (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n' || text[pos] == '\r')) {
                ++pos;
            }
            return pos < text.size();
        }

        bool Consume(char c) {
            if (pos < text.size() && text[pos] == c) {
                ++pos;
                return true;
            }
            return false;
        }

        std::optional<Json> ParseValue() {
            if (!SkipWs()) return std::nullopt;
            switch (text[pos]) {
                case '{': return ParseObject();
                case '[': return ParseArray();
                case '"': return ParseString();
                case 't': case 'f': return ParseBool();
                case 'n': return ParseNull();
                default:  return ParseNumber();
            }
        }

        std::optional<Json> ParseObject() {
            if (++depth > kMaxDepth) return std::nullopt;
            if (!Consume('{')) return std::nullopt;
            Object obj;
            if (Consume('}')) {
                --depth;
                return Json(std::move(obj));
            }
            for (;;) {
                if (!SkipWs() || text[pos] != '"') return std::nullopt;
                auto key = ParseStringRaw();
                if (!key) return std::nullopt;
                if (!SkipWs() || !Consume(':')) return std::nullopt;
                auto value = ParseValue();
                if (!value) return std::nullopt;
                obj.emplace(*key, std::move(*value));
                if (!SkipWs()) return std::nullopt;
                if (Consume(',')) continue;
                if (Consume('}')) break;
                return std::nullopt;
            }
            --depth;
            return Json(std::move(obj));
        }

        std::optional<Json> ParseArray() {
            if (++depth > kMaxDepth) return std::nullopt;
            if (!Consume('[')) return std::nullopt;
            Array arr;
            if (Consume(']')) {
                --depth;
                return Json(std::move(arr));
            }
            for (;;) {
                auto value = ParseValue();
                if (!value) return std::nullopt;
                arr.emplace_back(std::move(*value));
                if (!SkipWs()) return std::nullopt;
                if (Consume(',')) continue;
                if (Consume(']')) break;
                return std::nullopt;
            }
            --depth;
            return Json(std::move(arr));
        }

        std::optional<Json> ParseString() {
            auto raw = ParseStringRaw();
            if (!raw) return std::nullopt;
            return Json(std::move(*raw));
        }

        std::optional<std::string> ParseStringRaw() {
            if (!Consume('"')) return std::nullopt;
            std::string out;
            while (pos < text.size()) {
                char c = text[pos++];
                if (c == '"') return out;
                if (c != '\\') {
                    out.push_back(c);
                    continue;
                }
                if (pos >= text.size()) return std::nullopt;
                char e = text[pos++];
                switch (e) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        uint32_t cp = 0;
                        if (!ParseHex4(cp)) return std::nullopt;
                        // Surrogate pair
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                            if (pos + 1 < text.size() && text[pos] == '\\' && text[pos + 1] == 'u') {
                                pos += 2;
                                uint32_t low = 0;
                                if (!ParseHex4(low)) return std::nullopt;
                                if (low >= 0xDC00 && low <= 0xDFFF) {
                                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                                }
                            }
                        }
                        AppendUtf8(out, cp);
                        break;
                    }
                    default:
                        return std::nullopt;
                }
            }
            return std::nullopt;
        }

        bool ParseHex4(uint32_t& out) {
            if (pos + 4 > text.size()) return false;
            uint32_t v = 0;
            for (int i = 0; i < 4; ++i) {
                char c = text[pos++];
                v <<= 4;
                if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
                else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
                else return false;
            }
            out = v;
            return true;
        }

        std::optional<Json> ParseBool() {
            if (text.substr(pos, 4) == "true") { pos += 4; return Json(true); }
            if (text.substr(pos, 5) == "false") { pos += 5; return Json(false); }
            return std::nullopt;
        }

        std::optional<Json> ParseNull() {
            if (text.substr(pos, 4) == "null") { pos += 4; return Json(nullptr); }
            return std::nullopt;
        }

        std::optional<Json> ParseNumber() {
            size_t start = pos;
            if (pos < text.size() && text[pos] == '-') ++pos;
            size_t digits = pos;
            while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') ++pos;
            if (pos == digits) return std::nullopt;
            if (pos < text.size() && text[pos] == '.') {
                ++pos;
                size_t frac = pos;
                while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') ++pos;
                if (pos == frac) return std::nullopt;
            }
            if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E')) {
                ++pos;
                if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) ++pos;
                size_t exp = pos;
                while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') ++pos;
                if (pos == exp) return std::nullopt;
            }
            std::string number(text.substr(start, pos - start));
            return Json(std::strtod(number.c_str(), nullptr));
        }
    };

    Value m_value;
};

} // namespace rex::runtime
