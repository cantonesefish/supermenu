#include "json.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <windows.h>

namespace sm::json {
namespace {

class Parser {
public:
    explicit Parser(std::string_view text) : s_(text) {}

    bool ParseTop(Object& out) {
        SkipWs();
        if (!Take('{')) return false;
        SkipWs();
        if (Take('}')) return true;
        for (;;) {
            std::string key;
            if (!ParseString(key)) return false;
            SkipWs();
            if (!Take(':')) return false;
            SkipWs();
            Value v;
            if (!ParseValue(v)) return false;
            out.emplace(std::move(key), std::move(v));
            SkipWs();
            if (Take(',')) {
                SkipWs();
                continue;
            }
            if (Take('}')) return true;
            return false;
        }
    }

private:
    void SkipWs() {
        while (pos_ < s_.size()) {
            char c = s_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++pos_;
            } else {
                break;
            }
        }
    }
    bool Take(char c) {
        if (pos_ < s_.size() && s_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }
    bool ParseValue(Value& out) {
        if (pos_ >= s_.size()) return false;
        char c = s_[pos_];
        if (c == '"') {
            out.type = Value::Type::String;
            return ParseString(out.string);
        }
        if (c == '{' || c == '[') return SkipNested();
        if (c == 't') {
            if (!Literal("true")) return false;
            out.type = Value::Type::Bool;
            out.boolean = true;
            return true;
        }
        if (c == 'f') {
            if (!Literal("false")) return false;
            out.type = Value::Type::Bool;
            out.boolean = false;
            return true;
        }
        if (c == 'n') {
            if (!Literal("null")) return false;
            out.type = Value::Type::Null;
            return true;
        }
        return ParseNumber(out);
    }
    bool Literal(std::string_view t) {
        if (s_.substr(pos_, t.size()) != t) return false;
        pos_ += t.size();
        return true;
    }
    bool ParseNumber(Value& out) {
        const char* begin = s_.data() + pos_;
        char* end = nullptr;
        double v = std::strtod(begin, &end);
        if (end == begin) return false;
        pos_ = static_cast<size_t>(end - s_.data());
        out.type = Value::Type::Number;
        out.number = v;
        return true;
    }
    static void AppendUtf8(std::string& out, unsigned cp) {
        if (cp <= 0x7F) {
            out.push_back(static_cast<char>(cp));
        } else if (cp <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp <= 0xFFFF) {
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
    bool ParseString(std::string& out) {
        if (!Take('"')) return false;
        out.clear();
        while (pos_ < s_.size()) {
            char c = s_[pos_++];
            if (c == '"') return true;
            if (c == '\\') {
                if (pos_ >= s_.size()) return false;
                char e = s_[pos_++];
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
                        if (pos_ + 4 > s_.size()) return false;
                        unsigned cp = 0;
                        for (int i = 0; i < 4; ++i) {
                            char h = s_[pos_++];
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= unsigned(h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= unsigned(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= unsigned(h - 'A' + 10);
                            else return false;
                        }
                        // surrogate pair
                        if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 6 <= s_.size() && s_[pos_] == '\\' && s_[pos_ + 1] == 'u') {
                            pos_ += 2;
                            unsigned lo = 0;
                            for (int i = 0; i < 4; ++i) {
                                char h = s_[pos_++];
                                lo <<= 4;
                                if (h >= '0' && h <= '9') lo |= unsigned(h - '0');
                                else if (h >= 'a' && h <= 'f') lo |= unsigned(h - 'a' + 10);
                                else if (h >= 'A' && h <= 'F') lo |= unsigned(h - 'A' + 10);
                                else return false;
                            }
                            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            } else {
                                AppendUtf8(out, cp);
                                cp = lo;
                            }
                        }
                        AppendUtf8(out, cp);
                        break;
                    }
                    default: return false;
                }
            } else if (static_cast<unsigned char>(c) < 0x20) {
                return false;
            } else {
                out.push_back(c);
            }
        }
        return false;
    }
    bool SkipNested() {
        // We are at '{' or '['; validate it with a simple recursive scan.
        return SkipValue();
    }
    bool SkipValue() {
        SkipWs();
        if (pos_ >= s_.size()) return false;
        char c = s_[pos_];
        if (c == '"') {
            std::string tmp;
            return ParseString(tmp);
        }
        if (c == '{') {
            ++pos_;
            SkipWs();
            if (Take('}')) return true;
            for (;;) {
                std::string key;
                if (!ParseString(key)) return false;
                SkipWs();
                if (!Take(':')) return false;
                if (!SkipValue()) return false;
                SkipWs();
                if (Take(',')) continue;
                return Take('}');
            }
        }
        if (c == '[') {
            ++pos_;
            SkipWs();
            if (Take(']')) return true;
            for (;;) {
                if (!SkipValue()) return false;
                SkipWs();
                if (Take(',')) continue;
                return Take(']');
            }
        }
        if (c == 't') return Literal("true");
        if (c == 'f') return Literal("false");
        if (c == 'n') return Literal("null");
        Value dummy;
        return ParseNumber(dummy);
    }

    std::string_view s_;
    size_t pos_ = 0;
};

}  // namespace

bool ParseObject(std::string_view text, Object& out) {
    out.clear();
    Parser p(text);
    return p.ParseTop(out);
}

std::string Quote(std::string_view utf8) {
    std::string out;
    out.reserve(utf8.size() + 2);
    out.push_back('"');
    for (unsigned char c : utf8) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04X", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
    return out;
}

}  // namespace sm::json
