// SPDX-License-Identifier: Apache-2.0
// PresetRelay reference header (spec v0.2 draft). Plain C++17, no dependencies.
//
// A plugin implements ONE function: take a JSON request, return a JSON reply. The same function serves every doorway
// (AU property, AUv3 channel, VST3 interface, CLAP extension). The helpers below are optional: the access levels,
// a throttle that enforces the spec's load limit, and small reply builders.
#pragma once
#include <chrono>
#include <cstdio>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>

namespace presetrelay {

// AU v2 custom property id (spec 4.3).
constexpr unsigned int kAUPropertyID = 1346457454u;

// Limits a plugin applies to requests it receives and replies it sends (spec 8).
constexpr std::size_t kMaxRequestBytes = 64 * 1024;
constexpr std::size_t kMaxReplyBytes = 16 * 1024 * 1024;

// The one function a plugin implements.
struct Handler {
    virtual ~Handler() = default;
    // request: {"op":"hello"|"list"|"get"|"load"|"current"|..., "v":1, ...}
    // return:  {"ok":true,...} or {"ok":false,"error":{"code":"unsupported","message":"..."}}
    // Called on a non-audio thread. Keep it quick; `load` should hop to the plugin's UI/message thread.
    virtual std::string handleRequest(const std::string& requestJson) = 0;
};

// ---- Access levels (spec 3.1) ----

enum class Access { catalog = 1, audition = 2, insight = 3, open = 4 };

inline const char* accessName(Access a) {
    switch (a) {
        case Access::catalog: return "catalog";
        case Access::audition: return "audition";
        case Access::insight: return "insight";
        case Access::open: return "open";
    }
    return "catalog";
}

// "catalog" / "audition" / "insight" / "open" -> level; anything else -> `fallback`.
inline Access accessFromName(const std::string& s, Access fallback = Access::open) {
    if (s == "catalog") return Access::catalog;
    if (s == "audition") return Access::audition;
    if (s == "insight") return Access::insight;
    if (s == "open") return Access::open;
    return fallback;
}

inline bool atLeast(Access have, Access need) { return static_cast<int>(have) >= static_cast<int>(need); }

// ---- Throttle (spec 3.2) ----

// Allows at most `perMinute` uses in any sliding 60 s window. perMinute <= 0 means no limit. Thread-safe.
class Throttle {
public:
    explicit Throttle(int perMinute = 0) : perMinute_(perMinute) {}
    void setLimit(int perMinute) {
        std::lock_guard<std::mutex> g(m_);
        perMinute_ = perMinute;
    }
    int limit() const {
        std::lock_guard<std::mutex> g(m_);
        return perMinute_;
    }

    // Records a use and returns 0 if it is allowed; otherwise returns the milliseconds the caller must wait (and records nothing).
    int tryUse(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) {
        std::lock_guard<std::mutex> g(m_);
        if (perMinute_ <= 0) return 0;
        const auto window = std::chrono::seconds(60);
        while (!uses_.empty() && now - uses_.front() >= window) uses_.pop_front();
        if (static_cast<int>(uses_.size()) < perMinute_) {
            uses_.push_back(now);
            return 0;
        }
        const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(uses_.front() + window - now).count();
        return static_cast<int>(wait < 1 ? 1 : wait);
    }

private:
    mutable std::mutex m_;
    std::deque<std::chrono::steady_clock::time_point> uses_;
    int perMinute_;
};

// ---- Reply builders (for plugins without a JSON library) ----

inline std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof b, "\\u%04x", c);
                    out += b;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

// {"ok":false,"error":{"code":...,"message":...,"retryAfterMs":N}}. retryAfterMs is written only when > 0, `reason` only for
// needs_authorization. Codes: spec appendix B.
inline std::string errorReply(const std::string& code, const std::string& message, int retryAfterMs = 0,
                              const std::string& reason = std::string()) {
    std::string r = "{\"ok\":false,\"error\":{\"code\":\"" + jsonEscape(code) + "\",\"message\":\"" + jsonEscape(message) + "\"";
    if (!reason.empty()) r += ",\"reason\":\"" + jsonEscape(reason) + "\"";
    if (retryAfterMs > 0) r += ",\"retryAfterMs\":" + std::to_string(retryAfterMs);
    return r + "}}";
}

}  // namespace presetrelay
