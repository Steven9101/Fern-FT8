// Fern-FT8, an FT8 decoder module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "json.h"

#include <cstdio>
#include <cstdlib>

namespace fern::ft8 {

namespace {
const Json& null_json() {
    static const Json null;
    return null;
}
const std::string& empty_string() {
    static const std::string empty;
    return empty;
}
}  // namespace

class JsonParser {
public:
    explicit JsonParser(const std::string& text) : s_(text) {}

    bool parse(Json& out) {
        if (!value(out, 0)) return false;
        space();
        return i_ == s_.size();
    }

private:
    static constexpr int kMaxDepth = 32;

    void space() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) i_++;
    }

    bool literal(const char* word) {
        size_t n = 0;
        while (word[n]) n++;
        if (s_.compare(i_, n, word) != 0) return false;
        i_ += n;
        return true;
    }

    bool value(Json& out, int depth) {
        if (depth > kMaxDepth) return false;
        space();
        if (i_ >= s_.size()) return false;
        const char c = s_[i_];
        if (c == '{') return object(out, depth);
        if (c == '[') return array(out, depth);
        if (c == '"') {
            out.kind_ = Json::Kind::String;
            return string(out.string_);
        }
        if (c == 't' || c == 'f') {
            out.kind_ = Json::Kind::Bool;
            out.bool_ = c == 't';
            return literal(c == 't' ? "true" : "false");
        }
        if (c == 'n') return literal("null");
        return number(out);
    }

    bool number(Json& out) {
        const size_t start = i_;
        if (i_ < s_.size() && s_[i_] == '-') i_++;
        bool digits = false;
        while (i_ < s_.size() && ((s_[i_] >= '0' && s_[i_] <= '9') || s_[i_] == '.' || s_[i_] == 'e' ||
                                  s_[i_] == 'E' || s_[i_] == '+' || s_[i_] == '-')) {
            digits = digits || (s_[i_] >= '0' && s_[i_] <= '9');
            i_++;
        }
        if (!digits) return false;
        const std::string text = s_.substr(start, i_ - start);
        char* end = nullptr;
        const double v = std::strtod(text.c_str(), &end);
        if (!end || *end != '\0') return false;
        out.kind_ = Json::Kind::Number;
        out.number_ = v;
        return true;
    }

    static void append_utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xc0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        } else {
            out += static_cast<char>(0xe0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        }
    }

    bool string(std::string& out) {
        if (s_[i_] != '"') return false;
        i_++;
        while (i_ < s_.size()) {
            const char c = s_[i_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return false;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i_ >= s_.size()) return false;
            const char e = s_[i_++];
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
                    if (i_ + 4 > s_.size()) return false;
                    unsigned cp = 0;
                    for (int k = 0; k < 4; k++) {
                        const char h = s_[i_++];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
                        else return false;
                    }
                    append_utf8(out, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }

    bool array(Json& out, int depth) {
        out.kind_ = Json::Kind::Array;
        i_++;
        space();
        if (i_ < s_.size() && s_[i_] == ']') {
            i_++;
            return true;
        }
        while (true) {
            Json element;
            if (!value(element, depth + 1)) return false;
            out.array_.push_back(std::move(element));
            space();
            if (i_ >= s_.size()) return false;
            if (s_[i_] == ',') {
                i_++;
                continue;
            }
            if (s_[i_] == ']') {
                i_++;
                return true;
            }
            return false;
        }
    }

    bool object(Json& out, int depth) {
        out.kind_ = Json::Kind::Object;
        i_++;
        space();
        if (i_ < s_.size() && s_[i_] == '}') {
            i_++;
            return true;
        }
        while (true) {
            space();
            std::string key;
            if (i_ >= s_.size() || !string(key)) return false;
            space();
            if (i_ >= s_.size() || s_[i_] != ':') return false;
            i_++;
            Json member;
            if (!value(member, depth + 1)) return false;
            out.object_[key] = std::move(member);
            space();
            if (i_ >= s_.size()) return false;
            if (s_[i_] == ',') {
                i_++;
                continue;
            }
            if (s_[i_] == '}') {
                i_++;
                return true;
            }
            return false;
        }
    }

    const std::string& s_;
    size_t i_ = 0;
};

bool Json::parse(const std::string& text, Json& out) {
    out = Json();
    JsonParser parser(text);
    Json parsed;
    if (!parser.parse(parsed)) return false;
    out = std::move(parsed);
    return true;
}

const std::string& Json::string() const { return kind_ == Kind::String ? string_ : empty_string(); }

const Json& Json::operator[](const std::string& key) const {
    if (kind_ != Kind::Object) return null_json();
    const auto found = object_.find(key);
    return found == object_.end() ? null_json() : found->second;
}

bool Json::has(const std::string& key) const { return kind_ == Kind::Object && object_.count(key) > 0; }

std::string json_quote(const std::string& text) {
    std::string out = "\"";
    for (const char c : text) {
        const auto u = static_cast<unsigned char>(c);
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (u < 0x20 || u == 0x7f) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", u);
            out += buf;
        } else {
            out += c;
        }
    }
    return out + "\"";
}

}  // namespace fern::ft8
