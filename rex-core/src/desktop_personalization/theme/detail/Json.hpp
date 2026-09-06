#pragma once
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace rex::theme::detail {

// Minimal dependency-free JSON value + parser/serializer.
// Supports the subset needed for .rextheme inspection:
// objects, arrays, strings, numbers, booleans, null.
class Json {
public:
    using Object = std::map<std::string, Json>;
    using Array = std::vector<Json>;
    using Value = std::variant<std::nullptr_t, bool, double, std::string, Array, Object>;

    Json() : value_(nullptr) {}
    Json(std::nullptr_t) : value_(nullptr) {}
    Json(bool b) : value_(b) {}
    Json(int i) : value_(static_cast<double>(i)) {}
    Json(double d) : value_(d) {}
    Json(const char* s) : value_(std::string(s)) {}
    Json(std::string s) : value_(std::move(s)) {}
    Json(Array a) : value_(std::move(a)) {}
    Json(Object o) : value_(std::move(o)) {}

    // ── Type queries ────────────────────────────────────────────────────
    bool isNull()   const { return std::holds_alternative<std::nullptr_t>(value_); }
    bool isBool()   const { return std::holds_alternative<bool>(value_); }
    bool isNumber() const { return std::holds_alternative<double>(value_); }
    bool isString() const { return std::holds_alternative<std::string>(value_); }
    bool isArray()  const { return std::holds_alternative<Array>(value_); }
    bool isObject() const { return std::holds_alternative<Object>(value_); }

    // ── Accessors ───────────────────────────────────────────────────────
    bool asBool() const {
        if (!isBool()) throw JsonError("not a boolean");
        return std::get<bool>(value_);
    }
    double asDouble() const {
        if (!isNumber()) throw JsonError("not a number");
        return std::get<double>(value_);
    }
    int asInt() const {
        return static_cast<int>(asDouble());
    }
    const std::string& asString() const {
        if (!isString()) throw JsonError("not a string");
        return std::get<std::string>(value_);
    }
    const Array& asArray() const {
        if (!isArray()) throw JsonError("not an array");
        return std::get<Array>(value_);
    }
    const Object& asObject() const {
        if (!isObject()) throw JsonError("not an object");
        return std::get<Object>(value_);
    }

    bool contains(const std::string& key) const {
        if (!isObject()) return false;
        return std::get<Object>(value_).count(key) != 0;
    }

    // Returns Json() (null) when member is missing.
    const Json& at(const std::string& key) const {
        static const Json kNull;
        if (!isObject()) return kNull;
        auto& obj = std::get<Object>(value_);
        auto it = obj.find(key);
        return it == obj.end() ? kNull : it->second;
    }

    // ── Parsing ─────────────────────────────────────────────────────────
    struct JsonError : std::runtime_error {
        explicit JsonError(const std::string& msg) : std::runtime_error(msg) {}
    };

    // Throws JsonError on invalid input.
    static Json parse(const std::string& text) {
        Parser p(text);
        Json j = p.parseValue();
        p.skipWhitespace();
        if (!p.atEnd()) throw JsonError("trailing content after JSON value");
        return j;
    }

    // ── Serialization ───────────────────────────────────────────────────
    std::string dump(bool pretty = false) const {
        std::string out;
        if (pretty) {
            dumpPretty(out, 0);
        } else {
            dumpCompact(out);
        }
        return out;
    }

private:
    void dumpCompact(std::string& out) const {
        if (isNull())       { out += "null"; return; }
        if (isBool())       { out += asBool() ? "true" : "false"; return; }
        if (isNumber())     { out += formatNumber(asDouble()); return; }
        if (isString())     { out += quote(asString()); return; }
        if (isArray()) {
            out += '[';
            bool first = true;
            for (const auto& e : asArray()) {
                if (!first) out += ',';
                first = false;
                e.dumpCompact(out);
            }
            out += ']';
            return;
        }
        // object
        out += '{';
        bool first = true;
        for (const auto& [k, v] : asObject()) {
            if (!first) out += ',';
            first = false;
            out += quote(k);
            out += ':';
            v.dumpCompact(out);
        }
        out += '}';
    }

    void dumpPretty(std::string& out, int indent) const {
        const std::string pad(static_cast<size_t>(indent) * 2, ' ');
        const std::string pad2(static_cast<size_t>(indent + 1) * 2, ' ');
        if (isArray()) {
            if (asArray().empty()) { out += "[]"; return; }
            out += "[\n";
            bool first = true;
            for (const auto& e : asArray()) {
                if (!first) out += ",\n";
                first = false;
                out += pad2;
                e.dumpPretty(out, indent + 1);
            }
            out += '\n';
            out += pad;
            out += ']';
            return;
        }
        if (isObject()) {
            if (asObject().empty()) { out += "{}"; return; }
            out += "{\n";
            bool first = true;
            for (const auto& [k, v] : asObject()) {
                if (!first) out += ",\n";
                first = false;
                out += pad2;
                out += quote(k);
                out += ": ";
                v.dumpPretty(out, indent + 1);
            }
            out += '\n';
            out += pad;
            out += '}';
            return;
        }
        dumpCompact(out);
    }

    static std::string formatNumber(double d) {
        if (d == static_cast<long long>(d) &&
            d >= -9007199254740992.0 && d <= 9007199254740992.0) {
            return std::to_string(static_cast<long long>(d));
        }
        std::string s = std::to_string(d);
        // Trim trailing zeros for readability.
        while (s.size() > 1 && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
        return s;
    }

    static std::string quote(const std::string& s) {
        std::string out = "\"";
        for (char c : s) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        char buf[8];
                        snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                        out += buf;
                    } else {
                        out += c;
                    }
            }
        }
        out += "\"";
        return out;
    }

    class Parser {
    public:
        explicit Parser(const std::string& text) : text_(text), pos_(0) {}

        bool atEnd() const { return pos_ >= text_.size(); }

        void skipWhitespace() {
            while (pos_ < text_.size()) {
                char c = text_[pos_];
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                    ++pos_;
                } else {
                    break;
                }
            }
        }

        Json parseValue() {
            skipWhitespace();
            if (atEnd()) throw JsonError("unexpected end of input");
            char c = text_[pos_];
            switch (c) {
                case '{': return parseObject();
                case '[': return parseArray();
                case '"': return Json(parseString());
                case 't': expect("true"); return Json(true);
                case 'f': expect("false"); return Json(false);
                case 'n': expect("null"); return Json(nullptr);
                default:
                    if (c == '-' || (c >= '0' && c <= '9')) return Json(parseNumber());
                    throw JsonError(std::string("unexpected character '") + c + "'");
            }
        }

    private:
        void expect(const char* word) {
            for (const char* w = word; *w; ++w) {
                if (pos_ >= text_.size() || text_[pos_] != *w) {
                    throw JsonError("invalid literal");
                }
                ++pos_;
            }
        }

        Json parseObject() {
            ++pos_; // consume '{'
            Object obj;
            skipWhitespace();
            if (!atEnd() && text_[pos_] == '}') { ++pos_; return Json(obj); }
            for (;;) {
                skipWhitespace();
                if (atEnd() || text_[pos_] != '"') throw JsonError("expected string key");
                std::string key = parseString();
                skipWhitespace();
                if (atEnd() || text_[pos_] != ':') throw JsonError("expected ':'");
                ++pos_;
                Json value = parseValue();
                obj.emplace(std::move(key), std::move(value));
                skipWhitespace();
                if (atEnd()) throw JsonError("unterminated object");
                char c = text_[pos_];
                if (c == ',') { ++pos_; continue; }
                if (c == '}') { ++pos_; return Json(obj); }
                throw JsonError("expected ',' or '}'");
            }
        }

        Json parseArray() {
            ++pos_; // consume '['
            Array arr;
            skipWhitespace();
            if (!atEnd() && text_[pos_] == ']') { ++pos_; return Json(arr); }
            for (;;) {
                Json value = parseValue();
                arr.emplace_back(std::move(value));
                skipWhitespace();
                if (atEnd()) throw JsonError("unterminated array");
                char c = text_[pos_];
                if (c == ',') { ++pos_; continue; }
                if (c == ']') { ++pos_; return Json(arr); }
                throw JsonError("expected ',' or ']'");
            }
        }

        std::string parseString() {
            ++pos_; // consume opening quote
            std::string out;
            for (;;) {
                if (atEnd()) throw JsonError("unterminated string");
                char c = text_[pos_];
                if (c == '"') { ++pos_; return out; }
                if (c == '\\') {
                    ++pos_;  // move to the escaped character
                    if (atEnd()) throw JsonError("unterminated escape");
                    char e = text_[pos_];
                    switch (e) {
                        case '"':  out += '"';  ++pos_; break;
                        case '\\': out += '\\'; ++pos_; break;
                        case '/':  out += '/';  ++pos_; break;
                        case 'b':  out += '\b'; ++pos_; break;
                        case 'f':  out += '\f'; ++pos_; break;
                        case 'n':  out += '\n'; ++pos_; break;
                        case 'r':  out += '\r'; ++pos_; break;
                        case 't':  out += '\t'; ++pos_; break;
                        case 'u': {
                            ++pos_;  // consume 'u', now at first hex digit
                            if (pos_ + 4 > text_.size()) throw JsonError("bad unicode escape");
                            unsigned cp = 0;
                            for (int i = 0; i < 4; ++i) {
                                cp <<= 4;
                                char h = text_[pos_ + static_cast<size_t>(i)];
                                cp |= hexValue(h);
                            }
                            pos_ += 4;
                            // Handle UTF-16 surrogate pair.
                            if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 6 <= text_.size() &&
                                text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
                                unsigned low = 0;
                                for (int i = 0; i < 4; ++i) {
                                    low <<= 4;
                                    low |= hexValue(text_[pos_ + 2 + static_cast<size_t>(i)]);
                                }
                                if (low >= 0xDC00 && low <= 0xDFFF) {
                                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                                    pos_ += 6;
                                }
                            }
                            appendUtf8(out, cp);
                            break;  // pos_ already points past the escape sequence
                        }
                        default:
                            throw JsonError("invalid escape");
                    }
                } else {
                    out += c;
                    ++pos_;
                }
            }
        }

        double parseNumber() {
            size_t start = pos_;
            if (!atEnd() && text_[pos_] == '-') ++pos_;
            while (!atEnd() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
            if (!atEnd() && text_[pos_] == '.') {
                ++pos_;
                while (!atEnd() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
            }
            if (!atEnd() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
                ++pos_;
                if (!atEnd() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
                if (atEnd() || text_[pos_] < '0' || text_[pos_] > '9') {
                    throw JsonError("invalid exponent");
                }
                while (!atEnd() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
            }
            std::string num = text_.substr(start, pos_ - start);
            if (num.empty() || num == "-") throw JsonError("invalid number");
            return std::stod(num);
        }

        static unsigned hexValue(char c) {
            if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
            if (c >= 'a' && c <= 'f') return static_cast<unsigned>(c - 'a' + 10);
            if (c >= 'A' && c <= 'F') return static_cast<unsigned>(c - 'A' + 10);
            throw JsonError("invalid hex digit");
        }

        static void appendUtf8(std::string& out, unsigned cp) {
            if (cp < 0x80) {
                out += static_cast<char>(cp);
            } else if (cp < 0x800) {
                out += static_cast<char>(0xC0 | (cp >> 6));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                out += static_cast<char>(0xE0 | (cp >> 12));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            } else {
                out += static_cast<char>(0xF0 | (cp >> 18));
                out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            }
        }

        const std::string& text_;
        size_t pos_;
    };

    Value value_;
};

} // namespace rex::theme::detail