// Fern-FT8, an FT8 decoder module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Just enough JSON for FernSDR's commands: one object a line, read into a
// tree, and strings written back escaped. Numbers are doubles; nesting is
// limited, so a hostile line cannot exhaust the stack.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace fern::ft8 {

class Json {
public:
    enum class Kind { Null, Bool, Number, String, Array, Object };

    // Parses one value filling the whole of `text` (whitespace aside);
    // false, leaving `out` null, for anything else.
    static bool parse(const std::string& text, Json& out);

    Kind kind() const { return kind_; }
    bool is_object() const { return kind_ == Kind::Object; }
    bool is_array() const { return kind_ == Kind::Array; }
    bool is_number() const { return kind_ == Kind::Number; }
    bool is_string() const { return kind_ == Kind::String; }

    double number(double fallback = 0.0) const { return kind_ == Kind::Number ? number_ : fallback; }
    const std::string& string() const;
    bool boolean(bool fallback = false) const { return kind_ == Kind::Bool ? bool_ : fallback; }
    // A member, or a null value when there is none.
    const Json& operator[](const std::string& key) const;
    bool has(const std::string& key) const;
    const std::vector<Json>& elements() const { return array_; }
    const std::map<std::string, Json>& members() const { return object_; }

private:
    friend class JsonParser;
    Kind kind_ = Kind::Null;
    double number_ = 0.0;
    bool bool_ = false;
    std::string string_;
    std::vector<Json> array_;
    std::map<std::string, Json> object_;
};

// `text` as a JSON string, quotes included.
std::string json_quote(const std::string& text);

}  // namespace fern::ft8
