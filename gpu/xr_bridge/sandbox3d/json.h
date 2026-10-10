// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// A small JSON reader for sandbox3d (model descriptions, the live config, hitboxes): parses a
// whole document into a tree; lookups on a missing key or a wrong type return a null value, so
// reads chain safely (doc["a"]["b"].Number(1.0)). No writer.

#pragma once

#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace json {

class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<Value> array;
    std::map<std::string, Value> object;

    bool IsNull() const { return type == Type::Null; }
    bool IsObject() const { return type == Type::Object; }
    bool IsArray() const { return type == Type::Array; }
    bool IsNumber() const { return type == Type::Number; }
    bool IsString() const { return type == Type::String; }
    bool Has(const std::string& key) const { return IsObject() && object.count(key); }

    const Value& operator[](const std::string& key) const {
        if (IsObject()) {
            const auto it = object.find(key);
            if (it != object.end()) {
                return it->second;
            }
        }
        return Null();
    }
    const Value& operator[](size_t index) const {
        return IsArray() && index < array.size() ? array[index] : Null();
    }
    size_t Size() const { return IsArray() ? array.size() : IsObject() ? object.size() : 0; }

    double Number(double fallback = 0.0) const {
        return IsNumber() ? number : type == Type::Bool ? (boolean ? 1.0 : 0.0) : fallback;
    }
    float Float(float fallback = 0.0f) const { return float(Number(fallback)); }
    int Int(int fallback = 0) const { return IsNumber() ? int(number) : fallback; }
    bool Bool(bool fallback = false) const {
        return type == Type::Bool ? boolean : IsNumber() ? number != 0.0 : fallback;
    }
    const std::string& Str(const std::string& fallback = EmptyString()) const {
        return IsString() ? string : fallback;
    }

    static const Value& Null() {
        static const Value null;
        return null;
    }
    static const std::string& EmptyString() {
        static const std::string empty;
        return empty;
    }
};

class Parser {
public:
    explicit Parser(const std::string& text) : s(text) {}

    /// The document, or a null value with `error` set.
    Value Parse() {
        Value v;
        Skip();
        if (!ParseValue(v, 0)) {
            return {};
        }
        Skip();
        if (i != s.size()) {
            Fail("trailing characters");
            return {};
        }
        return v;
    }

    std::string error;

private:
    const std::string& s;
    size_t i = 0;

    bool Fail(const char* what) {
        if (error.empty()) {
            error = std::string(what) + " at offset " + std::to_string(i);
        }
        return false;
    }

    void Skip() {
        while (i < s.size()) {
            const char c = s[i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++i;
            } else if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') {
                // Comments are not JSON, but a hand-edited config may have them.
                while (i < s.size() && s[i] != '\n') {
                    ++i;
                }
            } else {
                break;
            }
        }
    }

    bool ParseValue(Value& v, int depth) {
        if (depth > 64) {
            return Fail("nesting too deep");
        }
        if (i >= s.size()) {
            return Fail("unexpected end");
        }
        const char c = s[i];
        if (c == '{') {
            return ParseObject(v, depth);
        }
        if (c == '[') {
            return ParseArray(v, depth);
        }
        if (c == '"') {
            v.type = Value::Type::String;
            return ParseString(v.string);
        }
        if (s.compare(i, 4, "true") == 0) {
            i += 4;
            v.type = Value::Type::Bool;
            v.boolean = true;
            return true;
        }
        if (s.compare(i, 5, "false") == 0) {
            i += 5;
            v.type = Value::Type::Bool;
            return true;
        }
        if (s.compare(i, 4, "null") == 0) {
            i += 4;
            return true;
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            const char* begin = s.c_str() + i;
            char* end = nullptr;
            v.number = std::strtod(begin, &end);
            if (end == begin) {
                return Fail("bad number");
            }
            v.type = Value::Type::Number;
            i += size_t(end - begin);
            return true;
        }
        return Fail("unexpected character");
    }

    bool ParseObject(Value& v, int depth) {
        v.type = Value::Type::Object;
        ++i;
        Skip();
        if (i < s.size() && s[i] == '}') {
            ++i;
            return true;
        }
        for (;;) {
            Skip();
            std::string key;
            if (i >= s.size() || s[i] != '"' || !ParseString(key)) {
                return Fail("expected a key");
            }
            Skip();
            if (i >= s.size() || s[i] != ':') {
                return Fail("expected ':'");
            }
            ++i;
            Skip();
            Value& child = v.object[key];
            if (!ParseValue(child, depth + 1)) {
                return false;
            }
            Skip();
            if (i < s.size() && s[i] == ',') {
                ++i;
                continue;
            }
            if (i < s.size() && s[i] == '}') {
                ++i;
                return true;
            }
            return Fail("expected ',' or '}'");
        }
    }

    bool ParseArray(Value& v, int depth) {
        v.type = Value::Type::Array;
        ++i;
        Skip();
        if (i < s.size() && s[i] == ']') {
            ++i;
            return true;
        }
        for (;;) {
            Skip();
            v.array.emplace_back();
            if (!ParseValue(v.array.back(), depth + 1)) {
                return false;
            }
            Skip();
            if (i < s.size() && s[i] == ',') {
                ++i;
                continue;
            }
            if (i < s.size() && s[i] == ']') {
                ++i;
                return true;
            }
            return Fail("expected ',' or ']'");
        }
    }

    static void AppendUtf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out += char(cp);
        } else if (cp < 0x800) {
            out += char(0xC0 | (cp >> 6));
            out += char(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += char(0xE0 | (cp >> 12));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        } else {
            out += char(0xF0 | (cp >> 18));
            out += char(0x80 | ((cp >> 12) & 0x3F));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        }
    }

    bool ParseHex4(uint32_t& out) {
        if (i + 4 > s.size()) {
            return Fail("short \\u escape");
        }
        out = uint32_t(std::strtoul(s.substr(i, 4).c_str(), nullptr, 16));
        i += 4;
        return true;
    }

    bool ParseString(std::string& out) {
        ++i; // opening quote
        while (i < s.size()) {
            const char c = s[i++];
            if (c == '"') {
                return true;
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i >= s.size()) {
                break;
            }
            const char e = s[i++];
            switch (e) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                uint32_t cp = 0;
                if (!ParseHex4(cp)) {
                    return false;
                }
                if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= s.size() && s[i] == '\\' &&
                    s[i + 1] == 'u') {
                    i += 2;
                    uint32_t low = 0;
                    if (!ParseHex4(low)) {
                        return false;
                    }
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                }
                AppendUtf8(out, cp);
                break;
            }
            default: out += e; break; // \" \\ \/
            }
        }
        return Fail("unterminated string");
    }
};

inline Value Parse(const std::string& text, std::string* error = nullptr) {
    Parser parser(text);
    Value v = parser.Parse();
    if (error) {
        *error = parser.error;
    }
    return v;
}

} // namespace json
