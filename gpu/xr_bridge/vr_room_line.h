// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// The co-op room code for the VR dashboard: a small read-only line "Room K7Q-M4X  2/3". Pure
// logic, no networking: the launcher passes BB_ONLINE_ROOM (the join code) and BB_ONLINE_ROOM_FILE
// (user/online_room.json, which its heartbeat keeps current with the members) to the game; the
// bridge only reads them. Without BB_ONLINE_ROOM there is no line (a solo game shows nothing).

#pragma once

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

namespace vrui::room {

struct Info {
    std::string code;
    int members = 0; // 0: not known (the heartbeat has not written the file yet)
    int max = 3;
    bool closed = false;
};

/// The value of a top-level-ish `"key": "..."` string in the room file ("" when missing).
inline std::string JsonString(const std::string& text, const char* key) {
    const std::string pat = std::string("\"") + key + "\"";
    size_t at = text.find(pat);
    if (at == std::string::npos) {
        return {};
    }
    at = text.find(':', at + pat.size());
    if (at == std::string::npos) {
        return {};
    }
    at = text.find_first_not_of(" \t\r\n", at + 1);
    if (at == std::string::npos || text[at] != '"') {
        return {};
    }
    const size_t end = text.find('"', at + 1);
    return end == std::string::npos ? std::string() : text.substr(at + 1, end - at - 1);
}

inline int JsonInt(const std::string& text, const char* key, int fallback) {
    const std::string pat = std::string("\"") + key + "\"";
    size_t at = text.find(pat);
    if (at == std::string::npos) {
        return fallback;
    }
    at = text.find(':', at + pat.size());
    if (at == std::string::npos) {
        return fallback;
    }
    return std::atoi(text.c_str() + at + 1);
}

/// online_room.json -> Info. The member count is the number of `"you":` flags (one per member).
inline Info Parse(const std::string& text, const std::string& env_code) {
    Info i;
    i.code = env_code;
    const std::string code = JsonString(text, "code");
    if (i.code.empty()) {
        i.code = code;
    }
    for (size_t at = 0; (at = text.find("\"you\"", at)) != std::string::npos; at += 5) {
        ++i.members;
    }
    i.max = std::max(1, JsonInt(text, "max", 3));
    i.closed = text.find("\"closed\": true") != std::string::npos;
    return i;
}

/// "Room K7Q-M4X  2/3" ("Room K7Q-M4X" while the members are unknown, "Room K7Q-M4X closed"), "" without a code.
inline std::string Line(const Info& i) {
    if (i.code.empty()) {
        return {};
    }
    std::string s = "Room " + i.code;
    if (i.closed) {
        return s + " closed";
    }
    if (i.members > 0) {
        s += "  " + std::to_string(i.members) + "/" + std::to_string(i.max);
    }
    return s;
}

/// Reads the environment once and the room file every few seconds.
class Reader {
public:
    /// The line to show now (`now` in seconds, any monotonic clock).
    const std::string& Poll(double now) {
        if (!init_) {
            init_ = true;
            const char* c = std::getenv("BB_ONLINE_ROOM");
            const char* f = std::getenv("BB_ONLINE_ROOM_FILE");
            code_ = c ? c : "";
            file_ = f ? f : "";
            if (code_.empty()) {
                return line_; // a solo game: no line, no file reads
            }
        }
        if (code_.empty() || (read_ && now - at_ < 3.0)) {
            return line_;
        }
        read_ = true;
        at_ = now;
        std::string text;
        if (!file_.empty()) {
            std::ifstream in(file_, std::ios::binary);
            text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        line_ = Line(Parse(text, code_));
        return line_;
    }

private:
    bool init_ = false, read_ = false;
    double at_ = 0.0;
    std::string code_, file_, line_;
};

} // namespace vrui::room
