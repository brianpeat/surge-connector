// SPDX-License-Identifier: Apache-2.0
// Preset Bridge reference header (spec v0.2 draft). Apache License 2.0. Plain C++17, no dependencies.
//
// To take part, a plugin implements ONE function: take a JSON request, return a JSON reply. The same function serves every
// doorway (AU property, AUv3 channel, VST3 interface, CLAP extension, manifest generation, tests).
// The helpers below are optional: a throttle that enforces the spec's load limit, small reply builders, and the access levels.
#pragma once
#include <chrono>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>

namespace presetbridge {

// AU v2 custom property id (it spells 'PCon', from the working name; an arbitrary number kept for compatibility). Spec 4.3.
constexpr unsigned int kAUPropertyID = 1346457454u;

// Hard limits a plugin should apply to whatever a host sends and to what it sends back (spec 8).
constexpr std::size_t kMaxRequestBytes = 64 * 1024;
constexpr std::size_t kMaxReplyBytes = 16 * 1024 * 1024;

struct Handler {
    virtual ~Handler() = default;
    // request: {"op":"hello"|"list"|"get"|"load"|"exportState"|"current"|..., "v":1, ...}
    // return:  {"ok":true,...} or {"ok":false,"error":{"code":"unsupported","message":"..."}}
    // Called on a non-audio thread. Must be quick; `load` should hop to the plugin's own UI/message thread.
    virtual std::string handleRequest(const std::string& requestJson) = 0;
};

// ---- Access levels (spec 3.1) --------------------------------------------------------------------------------------
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

// ---- Throttle (spec 3.2) ---------------------------------------------------------------------------------------------
// Allows at most `perMinute` uses in any sliding 60 s window. perMinute <= 0 means no limit. Thread-safe.
class Throttle {
public:
    explicit Throttle(int perMinute = 0) : perMinute_(perMinute) {}
    void setLimit(int perMinute) { std::lock_guard<std::mutex> g(m_); perMinute_ = perMinute; }
    int limit() const { std::lock_guard<std::mutex> g(m_); return perMinute_; }

    // Records a use and returns 0 if it is allowed; otherwise returns how many milliseconds the caller must wait (and records nothing).
    int tryUse(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) {
        std::lock_guard<std::mutex> g(m_);
        if (perMinute_ <= 0) return 0;
        const auto window = std::chrono::seconds(60);
        while (!uses_.empty() && now - uses_.front() >= window) uses_.pop_front();
        if (static_cast<int>(uses_.size()) < perMinute_) { uses_.push_back(now); return 0; }
        const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(uses_.front() + window - now).count();
        return static_cast<int>(wait < 1 ? 1 : wait);
    }

private:
    mutable std::mutex m_;
    std::deque<std::chrono::steady_clock::time_point> uses_;
    int perMinute_;
};

// ---- Small reply builders (for plugins that do not already have a JSON library) -------------------------------------
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
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); out += b; } else out += static_cast<char>(c);
        }
    }
    return out;
}

// {"ok":false,"error":{"code":...,"message":...,"retryAfterMs":N}}   (retryAfterMs only when > 0). Codes: spec appendix B.
inline std::string errorReply(const std::string& code, const std::string& message, int retryAfterMs = 0) {
    std::string r = "{\"ok\":false,\"error\":{\"code\":\"" + jsonEscape(code) + "\",\"message\":\"" + jsonEscape(message) + "\"";
    if (retryAfterMs > 0) r += ",\"retryAfterMs\":" + std::to_string(retryAfterMs);
    return r + "}}";
}

}  // namespace presetbridge
