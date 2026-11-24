// SPDX-License-Identifier: Apache-2.0
// PresetRelay catalog helper for C++ (spec v0.2 draft). Header only, C++17, depends only on PresetRelay.h.
//
// The one-file way in: give a Catalog your sounds and a few callbacks and it answers hello, list, get, collections, collection, load, current,
// entitled, referenceState and events, including paging, limits, the load throttle, unsaved-edit protection, async loads and event bookkeeping.
// You never parse or write the protocol. It needs no JSON library: you hand it each sound as one JSON object string (build it with whatever you
// already use, or by hand with presetrelay::jsonEscape). Records that are not valid UTF-8 JSON objects with a string "id" (1 to 512 bytes, no NUL)
// and a string "name" are dropped, and so are duplicate ids and records over 1 MiB.
//
//     presetrelay::Catalog catalog("My Synth", "com.me.mysynth", presetrelay::Access::audition);   // ONE per process
//     catalog.records  = [] { return cachedRecordJson; };                    // std::vector<std::string>, each {"id":"...","name":"...",...}
//     catalog.revision = [] { return presetrelay::revisionOf({"v1", "<file stamps>"}); };   // optional, see below
//     catalog.isReady  = [] { return catalogBuilt.load(); };                // false while building: every op but hello and events answers busy
//     catalog.load     = [](const std::string& id) { return mySynth.select(id) ? presetrelay::LoadOutcome::ok() : presetrelay::LoadOutcome::notFound(); };
//     catalog.current  = [] { return presetrelay::CurrentSound{mySynth.currentId(), mySynth.hasUnsavedEdits()}; };
//     // then hand `&catalog` to your doorway (AU property hook, VST3 doorway, CLAP extension, C API) as the presetrelay::Handler.
//
// Threading contract. handleRequest may be called from any thread except the audio thread, several at a time, and never throws. The callbacks
// and the plain data members (kind, version, idStability, policyJson, packsJson, timeoutMs, supportsAsyncLoad, pageMax, and every std::function)
// are read as a copy taken when each request starts: assign them before you hand the catalog to a doorway, or later through reconfigure(), which
// is safe while requests run. setAccess, setEnabled and `events` are always safe. Loads run one at a time; `current` and `referenceState` go
// through the same hop as `load`, so a callback never runs concurrently with another one of those three. If sound changes must happen on your
// UI/message thread, set `runLoad` to a function that runs the given job there and waits for it, returning LoadOutcome::busy() when the thread
// does not answer within timeoutMs (hopWithTimeout below does this). Call shutdown() in your plugin's destructor, before the state your callbacks
// use is destroyed: it rejects new requests, drops queued async loads and waits a bounded time for one that is running.
//
// Revision. `revision` names the catalog's content and MUST change whenever any record, collection or the packs change. Left unset the catalog
// hashes the content of every record on each request, which is fine up to a few thousand records; larger catalogs should set `revision` (the
// validated records are then cached and reused until it changes).
#pragma once
#include <atomic>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "PresetRelay.h"

namespace presetrelay {

// Flat scanning of top-level keys and a strict validator; not a general parser.
namespace mini {

constexpr std::size_t npos = std::string::npos;

inline bool isSpace(char c) { return c == ' ' || c == '\n' || c == '\t' || c == '\r'; }

// Position just past whitespace.
inline std::size_t skipWs(const std::string& s, std::size_t i) {
    while (i < s.size() && isSpace(s[i])) ++i;
    return i;
}

// End (exclusive) of the string literal starting at s[i] == '"', or npos if unterminated.
inline std::size_t stringEnd(const std::string& s, std::size_t i) {
    for (++i; i < s.size(); ++i) {
        if (s[i] == '\\') ++i;
        else if (s[i] == '"') return i + 1;
    }
    return npos;
}

// End (exclusive) of the JSON value starting at i, or npos if there is no value there or it is unterminated. A result is always past i.
inline std::size_t valueEnd(const std::string& s, std::size_t i) {
    i = skipWs(s, i);
    if (i >= s.size()) return npos;
    const std::size_t start = i;
    if (s[i] == '"') return stringEnd(s, i);
    if (s[i] == '{' || s[i] == '[') {
        int depth = 0;
        for (; i < s.size(); ++i) {
            if (s[i] == '"') {
                const auto e = stringEnd(s, i);
                if (e == npos) return npos;
                i = e - 1;
            } else if (s[i] == '{' || s[i] == '[') {
                ++depth;
            } else if (s[i] == '}' || s[i] == ']') {
                if (--depth <= 0) return i + 1;
            }
        }
        return npos;
    }
    if (s[i] == ',' || s[i] == '}' || s[i] == ']' || s[i] == ':') return npos;
    while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' && !isSpace(s[i])) ++i;
    return i > start ? i : npos;
}

// One top-level member of an object string: its key (as written, without quotes) and where `"key":value` lies.
struct Member {
    std::string key;
    std::size_t begin = 0, end = 0;
};

// Splits a JSON object string into its members. False if it is malformed.
inline bool members(const std::string& obj, std::vector<Member>& out) {
    out.clear();
    std::size_t i = skipWs(obj, 0);
    if (i >= obj.size() || obj[i] != '{') return false;
    ++i;
    while (true) {
        i = skipWs(obj, i);
        if (i >= obj.size()) return false;
        if (obj[i] == '}') return true;
        if (obj[i] == ',') { ++i; continue; }
        if (obj[i] != '"') return false;
        const auto keyEnd = stringEnd(obj, i);
        if (keyEnd == npos) return false;
        Member m;
        m.begin = i;
        m.key = obj.substr(i + 1, keyEnd - i - 2);
        i = skipWs(obj, keyEnd);
        if (i >= obj.size() || obj[i] != ':') return false;
        i = skipWs(obj, i + 1);
        const auto end = valueEnd(obj, i);
        if (end == npos || end <= i) return false;
        m.end = end;
        out.push_back(std::move(m));
        i = end;
    }
}

// Raw text of the value of a top-level key in a JSON object string. False if absent or malformed.
inline bool find(const std::string& obj, const std::string& key, std::string& raw) {
    std::size_t i = skipWs(obj, 0);
    if (i >= obj.size() || obj[i] != '{') return false;
    ++i;
    while (true) {
        i = skipWs(obj, i);
        if (i >= obj.size() || obj[i] == '}') return false;
        if (obj[i] == ',') { ++i; continue; }
        if (obj[i] != '"') return false;
        const auto keyEnd = stringEnd(obj, i);
        if (keyEnd == npos) return false;
        const std::string k = obj.substr(i + 1, keyEnd - i - 2);
        i = skipWs(obj, keyEnd);
        if (i >= obj.size() || obj[i] != ':') return false;
        i = skipWs(obj, i + 1);
        const auto end = valueEnd(obj, i);
        if (end == npos || end <= i) return false;
        if (k == key) { raw = obj.substr(i, end - i); return true; }
        i = end;
    }
}

inline bool hex4(const std::string& s, std::size_t at, unsigned long& cp) {
    if (at + 4 > s.size()) return false;
    cp = 0;
    for (std::size_t k = at; k < at + 4; ++k) {
        const char c = s[k];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        cp = cp * 16 + static_cast<unsigned long>(d);
    }
    return true;
}

inline void appendUtf8(std::string& out, unsigned long cp) {
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

// Decodes a quoted JSON string (escapes and \u surrogate pairs) into UTF-8.
inline bool asString(const std::string& raw, std::string& out) {
    if (raw.size() < 2 || raw.front() != '"') return false;
    out.clear();
    for (std::size_t i = 1; i + 1 < raw.size(); ++i) {
        if (raw[i] != '\\') { out += raw[i]; continue; }
        if (++i + 1 > raw.size()) break;
        switch (raw[i]) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                unsigned long cp = 0;
                if (!hex4(raw, i + 1, cp)) { out += '?'; break; }
                i += 4;
                if (cp >= 0xD800 && cp < 0xDC00) {
                    // A high surrogate joins the \uDC00..DFFF that follows into one code point.
                    unsigned long lo = 0;
                    if (i + 6 < raw.size() && raw[i + 1] == '\\' && raw[i + 2] == 'u' && hex4(raw, i + 3, lo) && lo >= 0xDC00 && lo < 0xE000) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        i += 6;
                    } else {
                        cp = 0xFFFD;
                    }
                } else if (cp >= 0xDC00 && cp < 0xE000) {
                    cp = 0xFFFD;
                }
                appendUtf8(out, cp);
                break;
            }
            default: out += raw[i];
        }
    }
    return true;
}

inline bool getString(const std::string& obj, const std::string& key, std::string& out) {
    std::string raw;
    return find(obj, key, raw) && asString(raw, out);
}

inline bool getInt(const std::string& obj, const std::string& key, long long& out) {
    std::string raw;
    if (!find(obj, key, raw) || raw.empty()) return false;
    char* end = nullptr;
    out = std::strtoll(raw.c_str(), &end, 10);
    return end != raw.c_str();
}

inline bool getBool(const std::string& obj, const std::string& key, bool& out) {
    std::string raw;
    if (!find(obj, key, raw)) return false;
    if (raw == "true" || raw == "1") { out = true; return true; }
    if (raw == "false" || raw == "0") { out = false; return true; }
    return false;
}

// Elements of a JSON array string that are strings, skipping other elements. Always terminates.
inline std::vector<std::string> getStringArray(const std::string& obj, const std::string& key) {
    std::vector<std::string> out;
    std::string raw;
    if (!find(obj, key, raw) || raw.empty() || raw[0] != '[') return out;
    for (std::size_t i = 1; i < raw.size();) {
        i = skipWs(raw, i);
        if (i >= raw.size() || raw[i] == ']') break;
        if (raw[i] == ',') { ++i; continue; }
        const auto end = valueEnd(raw, i);
        if (end == npos || end <= i) break;
        std::string v;
        if (raw[i] == '"' && asString(raw.substr(i, end - i), v)) out.push_back(v);
        i = end;
    }
    return out;
}

// Raw text of each element of a JSON array string, or false if it is not an array or malformed.
inline bool arrayElements(const std::string& raw, std::vector<std::string>& out) {
    out.clear();
    std::size_t i = skipWs(raw, 0);
    if (i >= raw.size() || raw[i] != '[') return false;
    for (++i; i < raw.size();) {
        i = skipWs(raw, i);
        if (i >= raw.size()) return false;
        if (raw[i] == ']') return true;
        if (raw[i] == ',') { ++i; continue; }
        const auto end = valueEnd(raw, i);
        if (end == npos || end <= i) return false;
        out.push_back(raw.substr(i, end - i));
        i = end;
    }
    return false;
}

// Typed reads of request fields: absent, a well-formed value, or present with the wrong type.
enum class Presence { absent, ok, bad };

inline Presence readText(const std::string& obj, const std::string& key, std::string& out) {
    std::string raw;
    if (!find(obj, key, raw)) return Presence::absent;
    return asString(raw, out) ? Presence::ok : Presence::bad;
}

// An integer written as -?digits; values beyond 18 digits saturate.
inline Presence readInteger(const std::string& obj, const std::string& key, long long& out) {
    std::string raw;
    if (!find(obj, key, raw)) return Presence::absent;
    const bool neg = !raw.empty() && raw[0] == '-';
    const std::size_t first = neg ? 1 : 0;
    if (first >= raw.size()) return Presence::bad;
    for (std::size_t k = first; k < raw.size(); ++k) {
        if (raw[k] < '0' || raw[k] > '9') return Presence::bad;
    }
    if (raw.size() - first > 18) {
        out = neg ? LLONG_MIN : LLONG_MAX;
        return Presence::ok;
    }
    out = std::strtoll(raw.c_str(), nullptr, 10);
    return Presence::ok;
}

inline Presence readFlag(const std::string& obj, const std::string& key, bool& out) {
    std::string raw;
    if (!find(obj, key, raw)) return Presence::absent;
    if (raw == "true") { out = true; return Presence::ok; }
    if (raw == "false") { out = false; return Presence::ok; }
    return Presence::bad;
}

// An array whose elements are all strings.
inline Presence readStrings(const std::string& obj, const std::string& key, std::vector<std::string>& out) {
    out.clear();
    std::string raw;
    if (!find(obj, key, raw)) return Presence::absent;
    std::vector<std::string> elems;
    if (!arrayElements(raw, elems)) return Presence::bad;
    for (const auto& e : elems) {
        std::string v;
        if (e.empty() || e[0] != '"' || !asString(e, v)) return Presence::bad;
        out.push_back(std::move(v));
    }
    return Presence::ok;
}

// Strict JSON check: well-formed UTF-8, standard grammar, nesting at most 64 deep, one value and nothing after it.
class Checker {
public:
    explicit Checker(const std::string& s) : s_(s) {}
    bool run() {
        if (!validUtf8(s_)) return false;
        i_ = skipWs(s_, 0);
        if (!value(0)) return false;
        return skipWs(s_, i_) == s_.size();
    }

private:
    bool value(int depth) {
        if (depth > 64 || i_ >= s_.size()) return false;
        const char c = s_[i_];
        if (c == '{') return object(depth);
        if (c == '[') return array(depth);
        if (c == '"') return string();
        if (c == 't') return literal("true");
        if (c == 'f') return literal("false");
        if (c == 'n') return literal("null");
        return number();
    }
    bool literal(const char* word) {
        for (const char* p = word; *p; ++p, ++i_) {
            if (i_ >= s_.size() || s_[i_] != *p) return false;
        }
        return true;
    }
    bool digits() {
        const std::size_t start = i_;
        while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_;
        return i_ > start;
    }
    bool number() {
        if (i_ < s_.size() && s_[i_] == '-') ++i_;
        if (i_ >= s_.size()) return false;
        if (s_[i_] == '0') ++i_;
        else if (!digits()) return false;
        if (i_ < s_.size() && s_[i_] == '.') { ++i_; if (!digits()) return false; }
        if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
            ++i_;
            if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-')) ++i_;
            if (!digits()) return false;
        }
        return true;
    }
    bool string() {
        for (++i_; i_ < s_.size(); ++i_) {
            const unsigned char c = static_cast<unsigned char>(s_[i_]);
            if (c == '"') { ++i_; return true; }
            if (c < 0x20) return false;
            if (c != '\\') continue;
            if (++i_ >= s_.size()) return false;
            const char e = s_[i_];
            if (e == 'u') {
                unsigned long cp;
                if (!hex4(s_, i_ + 1, cp)) return false;
                i_ += 4;
            } else if (!(e == '"' || e == '\\' || e == '/' || e == 'b' || e == 'f' || e == 'n' || e == 'r' || e == 't')) {
                return false;
            }
        }
        return false;
    }
    void ws() { i_ = skipWs(s_, i_); }
    bool array(int depth) {
        ++i_;
        ws();
        if (i_ < s_.size() && s_[i_] == ']') { ++i_; return true; }
        while (true) {
            ws();
            if (!value(depth + 1)) return false;
            ws();
            if (i_ >= s_.size()) return false;
            if (s_[i_] == ',') { ++i_; continue; }
            if (s_[i_] == ']') { ++i_; return true; }
            return false;
        }
    }
    bool object(int depth) {
        ++i_;
        ws();
        if (i_ < s_.size() && s_[i_] == '}') { ++i_; return true; }
        while (true) {
            ws();
            if (i_ >= s_.size() || s_[i_] != '"' || !string()) return false;
            ws();
            if (i_ >= s_.size() || s_[i_] != ':') return false;
            ++i_;
            ws();
            if (!value(depth + 1)) return false;
            ws();
            if (i_ >= s_.size()) return false;
            if (s_[i_] == ',') { ++i_; continue; }
            if (s_[i_] == '}') { ++i_; return true; }
            return false;
        }
    }

    const std::string& s_;
    std::size_t i_ = 0;
};

inline bool isJsonOf(const std::string& s, char first) {
    const auto i = skipWs(s, 0);
    return i < s.size() && s[i] == first && Checker(s).run();
}
inline bool isJsonObject(const std::string& s) { return isJsonOf(s, '{'); }
inline bool isJsonArray(const std::string& s) { return isJsonOf(s, '['); }

// The object without the members for which `drop(key)` is true. `obj` must be valid JSON.
template <class Pred>
inline std::string withoutKeys(const std::string& obj, Pred drop) {
    std::vector<Member> ms;
    if (!members(obj, ms)) return obj;
    std::string out = "{";
    bool first = true;
    for (const auto& m : ms) {
        if (drop(m.key)) continue;
        if (!first) out += ",";
        first = false;
        out.append(obj, m.begin, m.end - m.begin);
    }
    return out + "}";
}

}  // namespace mini

// FNV-1a over strings, each followed by a separator byte.
class Fnv {
public:
    void add(const std::string& p) {
        for (unsigned char c : p) { h_ ^= c; h_ *= 0x100000001b3ull; }
        h_ ^= 0x1f;
        h_ *= 0x100000001b3ull;
    }
    std::string hex() const {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%llx", static_cast<unsigned long long>(h_));
        return buf;
    }

private:
    std::uint64_t h_ = 0xcbf29ce484222325ull;
};

// Hash, as lowercase hex, of the parts that change your catalog (file stamps, a version string, ids).
inline std::string revisionOf(const std::vector<std::string>& parts) {
    Fnv f;
    for (const auto& p : parts) f.add(p);
    return f.hex();
}

// Pull-model event queue. Copies share one queue, so a background job can keep posting after the Catalog that made it is gone.
class EventQueue {
public:
    EventQueue() : s_(std::make_shared<State>()) {}

    // `eventJson` is one JSON object, e.g. {"type":"loaded","id":"..."}; anything else, or over 64 KiB, is dropped. The queue keeps the newest 256.
    void post(const std::string& eventJson) {
        if (eventJson.size() > kMaxRequestBytes || !mini::isJsonObject(eventJson)) return;
        std::lock_guard<std::mutex> g(s_->m);
        ++s_->seq;
        s_->items.push_back({s_->seq, eventJson});
        if (s_->items.size() > 256) {
            s_->floor = s_->items.front().first;
            s_->items.pop_front();
        }
    }

    // `since` null: the first call, which returns no events and only the cursor. A cursor this queue did not issue, or one older than the newest
    // 256 events, answers one catalogChanged event carrying `currentRevision()` so the host rereads the catalog instead of missing changes.
    std::string reply(const std::string* since, const std::function<std::string()>& currentRevision) const {
        std::lock_guard<std::mutex> g(s_->m);
        const std::string head = "{\"ok\":true,\"events\":[";
        if (since == nullptr) return head + "],\"next\":\"" + cursor(s_->seq) + "\"}";
        long long from = -1;
        if (!parse(*since, from) || from > s_->seq || from < s_->floor) {
            return head + "{\"type\":\"catalogChanged\",\"revision\":\"" + jsonEscape(currentRevision()) + "\"}],\"next\":\"" + cursor(s_->seq) + "\"}";
        }
        std::string events;
        long long last = from;
        int n = 0;
        for (const auto& it : s_->items) {
            if (it.first <= from) continue;
            if (n >= 100) break;
            if (n++) events += ",";
            events += it.second;
            last = it.first;
        }
        return head + events + "],\"next\":\"" + cursor(last) + "\"}";
    }

private:
    struct State {
        std::mutex m;
        std::string epoch = makeEpoch();
        long long seq = 0, floor = 0;   // floor: the newest event that was dropped from the queue
        std::deque<std::pair<long long, std::string>> items;
    };

    static std::string makeEpoch() {
        static std::atomic<std::uint64_t> counter{0};
        const auto t = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        return revisionOf({std::to_string(t), std::to_string(counter.fetch_add(1))});
    }
    std::string cursor(long long n) const { return s_->epoch + "." + std::to_string(n); }
    bool parse(const std::string& c, long long& out) const {
        const auto dot = c.rfind('.');
        if (dot == std::string::npos || c.compare(0, dot, s_->epoch) != 0 || dot + 1 >= c.size() || c.size() - dot - 1 > 18) return false;
        for (std::size_t i = dot + 1; i < c.size(); ++i) {
            if (c[i] < '0' || c[i] > '9') return false;
        }
        out = std::strtoll(c.c_str() + dot + 1, nullptr, 10);
        return true;
    }

    std::shared_ptr<State> s_;
};

struct LoadOutcome {
    enum Kind { Ok, NotFound, NotAvailable, NotEntitled, Busy, Failed, NeedsAuthorization } kind = Ok;
    std::string message;
    static LoadOutcome ok() { return {Ok, {}}; }
    static LoadOutcome notFound() { return {NotFound, {}}; }
    static LoadOutcome notAvailable() { return {NotAvailable, {}}; }
    static LoadOutcome notEntitled() { return {NotEntitled, {}}; }
    static LoadOutcome busy() { return {Busy, {}}; }
    static LoadOutcome failed(std::string why) { return {Failed, std::move(why)}; }
    // The user owns it but the plugin must be authorized in its own window (spec 6).
    // reason: "expired" | "not_activated" | "offline" | "revoked" | "other" (or empty).
    static LoadOutcome needsAuthorization(std::string reason = {}) { return {NeedsAuthorization, std::move(reason)}; }
};

// What `referenceState` returns (spec 6, 5.2.3). `outcome` is anything but Ok when no state can be made. The base64 text is at most 8192 characters.
struct ReferenceState {
    LoadOutcome outcome = LoadOutcome::ok();
    std::string stateBase64;
    std::string stateKind;
    std::string requiresJson;   // optional JSON array of {kind,name,hash?}
};

// Empty id = unknown. rightsJson is the rights block (spec 7) of what is loaded now, or empty.
struct CurrentSound {
    std::string id;
    bool modified = false;
    std::string rightsJson;
};

// Runs `job` on the thread that `post` schedules work on (JUCE: [](std::function<void()> f) { juce::MessageManager::callAsync(std::move(f)); }),
// waits up to timeoutMs for it and returns its result, or LoadOutcome::busy() when it does not finish in time. The job and its result are
// shared with the posted work, so a late job finishes harmlessly after this has returned. Use it inside Catalog::runLoad.
inline LoadOutcome hopWithTimeout(const std::function<void(std::function<void()>)>& post, const std::function<LoadOutcome()>& job, int timeoutMs) {
    struct Box {
        std::mutex m;
        std::condition_variable cv;
        bool done = false;
        LoadOutcome result = LoadOutcome::busy();
    };
    auto box = std::make_shared<Box>();
    post([box, job] {
        LoadOutcome r;
        try { r = job(); } catch (...) { r = LoadOutcome::failed("the callback threw"); }
        std::lock_guard<std::mutex> g(box->m);
        box->result = std::move(r);
        box->done = true;
        box->cv.notify_all();
    });
    std::unique_lock<std::mutex> lk(box->m);
    if (!box->cv.wait_for(lk, std::chrono::milliseconds(timeoutMs < 1 ? 1 : timeoutMs), [&] { return box->done; })) return LoadOutcome::busy();
    return box->result;
}

// The settings and callbacks of a Catalog (see the threading contract at the top).
struct CatalogConfig {
    std::string kind = "instrument";                  // instrument | effect | midi-effect | other
    std::string version;
    std::string idStability = "permanent";            // permanent | per-install | session
    std::string policyJson;                           // optional JSON object, spec 3.4
    std::string packsJson;                            // optional JSON array of {id, name, rights?, policy?}, spec 7
    int timeoutMs = 10000;                            // what you promise to answer within, at most 60000
    int pageMax = 500;                                // most records in a list/collections/collection page; a larger `limit` is clamped
    bool supportsAsyncLoad = false;                   // honour "async":true on load

    std::function<std::vector<std::string>()> records;      // each a JSON object string with "id" and "name"
    std::function<std::string()> revision;                  // default: hash of the content of every record, collection and the packs
    std::function<bool()> isReady;                          // default: always ready
    std::function<std::vector<std::string>()> collections;  // JSON object strings {id,name,kind,presetIds:[...]}
    std::function<LoadOutcome(const std::string& id)> load; // apply the sound; return only after it has taken effect
    std::function<CurrentSound()> current;
    // Plugin-wide authorization state as a JSON object, e.g. {"state":"needed","reason":"expired"} (spec 6). Reported in hello and current;
    // while state is "needed", load answers needs_authorization without calling `load`.
    std::function<std::string()> authorizationJson;
    // item -> owned | not_owned | unknown. Items the callback leaves out, or answers with another value, are reported as unknown.
    std::function<std::map<std::string, std::string>(const std::vector<std::string>& items)> entitled;
    // Level 2: a small state that points at the sound inside the plugin; never the program, samples or anything decrypted.
    std::function<ReferenceState(const std::string& id)> referenceState;
    // Runs the job on your UI thread and waits for it. Default: runs it on the calling thread. See hopWithTimeout.
    std::function<LoadOutcome(const std::function<LoadOutcome()>& job)> runLoad;
};

class Catalog : public Handler, public CatalogConfig {
public:
    Catalog(std::string name, std::string id, Access access = Access::catalog, int loadsPerMinute = 10)
        : name_(std::move(name)), id_(std::move(id)), access_(access), throttle_(loadsPerMinute), shared_(std::make_shared<Shared>()) {}
    ~Catalog() override { shutdown(); }
    Catalog(const Catalog&) = delete;
    Catalog& operator=(const Catalog&) = delete;

    EventQueue events;

    // Changes settings or callbacks while requests may be running: `f` receives the CatalogConfig and runs while no request is copying it.
    template <class F>
    void reconfigure(F&& f) {
        std::lock_guard<std::mutex> g(configMutex_);
        f(static_cast<CatalogConfig&>(*this));
    }

    Access access() const { return access_.load(); }
    void setAccess(Access a) { access_ = a; }

    // Kill switch: while false, every request gets an `unsupported` error with no `relay` key, so a host concludes the plugin
    // does not take part. Can be flipped at any time.
    void setEnabled(bool on) { enabled_ = on; }
    bool isEnabled() const { return enabled_; }

    // Rejects new requests, drops queued async loads and waits up to two seconds for a running one. Safe to call more than once.
    void shutdown() {
        stopped_ = true;
        {
            std::lock_guard<std::mutex> g(shared_->m);
            shared_->stop = true;
            shared_->queue.clear();
            shared_->cv.notify_all();
        }
        std::thread worker;
        {
            std::lock_guard<std::mutex> g(workerMutex_);
            worker = std::move(worker_);
        }
        if (!worker.joinable()) return;
        std::unique_lock<std::mutex> lk(shared_->m);
        const bool exited = shared_->cv.wait_for(lk, std::chrono::seconds(2), [&] { return shared_->exited; });
        lk.unlock();
        if (exited) worker.join();
        else worker.detach();
    }

    std::string handleRequest(const std::string& req) override {
        try {
            std::string out = dispatch(req);
            if (out.size() > kMaxReplyBytes) out = errorReply("failed", "the reply is larger than 16 MiB");
            return out;
        } catch (...) {
            try {
                return errorReply("failed", "internal error");
            } catch (...) {
                return "{\"ok\":false,\"error\":{\"code\":\"failed\",\"message\":\"internal error\"}}";
            }
        }
    }

private:
    using Item = std::pair<std::string, std::string>;   // {id, record json}

    struct Snapshot {
        std::string revision;
        std::vector<Item> items;
        std::unordered_map<std::string, std::size_t> index;
    };

    struct AsyncJob {
        std::string id;
        std::function<LoadOutcome()> run;
        EventQueue events;
    };

    // State the worker thread shares with the Catalog, so the worker never touches the Catalog itself.
    struct Shared {
        std::mutex m;
        std::condition_variable cv;
        std::deque<AsyncJob> queue;
        bool stop = false, running = false, exited = false;
        std::timed_mutex loadMutex;   // one load, current or referenceState callback at a time
    };

    static constexpr std::size_t kMaxIdBytes = 512;
    static constexpr std::size_t kMaxRecordBytes = 1024 * 1024;
    static constexpr std::size_t kMaxStateChars = 8192;
    static constexpr std::size_t kMaxAsyncLoads = 16;
    static constexpr std::size_t kMaxEntitledItems = 2000;
    static constexpr long long kDefaultPage = 100;

    CatalogConfig configCopy() {
        std::lock_guard<std::mutex> g(configMutex_);
        return static_cast<const CatalogConfig&>(*this);
    }

    // Lowest access level at which an operation is offered (spec 6); 0 = unknown.
    static int opLevel(const std::string& op) {
        if (op == "hello" || op == "list" || op == "get" || op == "collections" || op == "collection") return 1;
        if (op == "load" || op == "current" || op == "entitled" || op == "events" || op == "referenceState") return 2;
        if (op == "params") return 3;
        if (op == "exportState") return 4;
        return 0;
    }

    static bool goodId(const std::string& id) { return !id.empty() && id.size() <= kMaxIdBytes && id.find('\0') == std::string::npos; }

    static std::string clip(const std::string& s) { return s.size() > 64 ? s.substr(0, 64) : s; }

    static int waitMs(const CatalogConfig& cfg) { return std::max(1, std::min(cfg.timeoutMs, 60000)); }

    std::string dispatch(const std::string& req) {
        if (!enabled_ || stopped_) return errorReply("unsupported", "the PresetRelay is switched off in this plugin");
        if (req.size() > kMaxRequestBytes) return errorReply("bad_request", "the request is larger than 64 KiB");
        if (!mini::isJsonObject(req)) return errorReply("bad_request", "the request is not a JSON object");
        std::string op;
        if (mini::readText(req, "op", op) != mini::Presence::ok) return errorReply("bad_request", "request needs a string \"op\"");
        const int need = opLevel(op);
        if (need == 0) return errorReply("unsupported", "unknown operation " + clip(op));
        if (static_cast<int>(access_.load()) < need) return errorReply("unsupported", op + " is not offered at this access level");

        const CatalogConfig cfg = configCopy();
        const bool ready = !cfg.isReady || cfg.isReady();
        if (op == "hello") return hello(cfg, ready);
        if (op == "events") return eventsReply(req, cfg, ready);

        if (op == "load" && !cfg.load) return errorReply("unsupported", "this plugin does not load by id");
        if (op == "current" && !cfg.current) return errorReply("unsupported", "not implemented");
        if (op == "entitled" && !cfg.entitled) return errorReply("unsupported", "not implemented");
        if (op == "referenceState" && !cfg.referenceState) return errorReply("unsupported", "not implemented");
        if ((op == "collections" || op == "collection") && !cfg.collections) return errorReply("unsupported", "this plugin has no collections");
        if (op == "params" || op == "exportState") return errorReply("unsupported", "not implemented");
        if (!ready) return errorReply("busy", "the catalog is still being built");

        if (op == "list") return list(req, cfg);
        if (op == "get") return get(req, cfg);
        if (op == "collections" || op == "collection") {
            const auto all = validCollections(cfg);
            if (all.empty()) return errorReply("unsupported", "this plugin has no collections");
            return op == "collections" ? collectionsReply(req, cfg, all) : collectionReply(req, cfg, all);
        }
        if (op == "load") return loadReply(req, cfg);
        if (op == "current") return currentReply(cfg);
        if (op == "entitled") return entitledReply(req, cfg);
        return referenceStateReply(req, cfg);
    }

    std::shared_ptr<const Snapshot> cachedFor(const std::string& rev) const {
        std::lock_guard<std::mutex> g(cacheMutex_);
        return cache_ && cache_->revision == rev ? cache_ : nullptr;
    }

    // Validated records for the current revision, rebuilt only when the revision changes.
    std::shared_ptr<const Snapshot> snapshot(const CatalogConfig& cfg) const {
        std::string rev;
        std::vector<std::string> raw;
        if (cfg.revision) {
            rev = normalizedRevision(cfg.revision());
            if (auto hit = cachedFor(rev)) return hit;
            if (cfg.records) raw = cfg.records();
        } else {
            if (cfg.records) raw = cfg.records();
            rev = normalizedRevision(contentRevision(raw, cfg));
            if (auto hit = cachedFor(rev)) return hit;
        }
        auto snap = std::make_shared<Snapshot>();
        snap->revision = rev;
        snap->items.reserve(raw.size());
        for (auto& r : raw) {
            std::string id, name;
            if (r.size() > kMaxRecordBytes || !mini::isJsonObject(r)) continue;
            if (mini::readText(r, "id", id) != mini::Presence::ok || !goodId(id) || mini::readText(r, "name", name) != mini::Presence::ok) continue;
            if (!snap->index.emplace(id, snap->items.size()).second) continue;
            snap->items.emplace_back(std::move(id), std::move(r));
        }
        std::lock_guard<std::mutex> g(cacheMutex_);
        cache_ = snap;
        return snap;
    }

    // "building" is what hello reports while the catalog is not ready, so a final revision never equals it.
    static std::string normalizedRevision(std::string r) {
        if (r.empty() || r == "building") r += "~";
        return r;
    }

    // Hash of everything a host can read from list, get, collections, collection and hello.packs.
    static std::string contentRevision(const std::vector<std::string>& raw, const CatalogConfig& cfg) {
        Fnv f;
        for (const auto& r : raw) f.add(r);
        f.add("packs");
        f.add(cfg.packsJson);
        f.add("collections");
        if (cfg.collections) {
            for (const auto& c : cfg.collections()) f.add(c);
        }
        return f.hex();
    }

    // Collections with a usable id and name; duplicates dropped.
    static std::vector<Item> validCollections(const CatalogConfig& cfg) {
        std::vector<Item> out;
        if (!cfg.collections) return out;
        std::set<std::string> seen;
        for (const auto& c : cfg.collections()) {
            std::string id, name;
            if (c.size() > kMaxRecordBytes || !mini::isJsonObject(c)) continue;
            if (mini::readText(c, "id", id) != mini::Presence::ok || !goodId(id) || mini::readText(c, "name", name) != mini::Presence::ok) continue;
            if (seen.insert(id).second) out.emplace_back(std::move(id), c);
        }
        return out;
    }

    // Reads `limit`; returns an error reply or an empty string.
    static std::string pageSizeOf(const std::string& req, const CatalogConfig& cfg, std::size_t& n) {
        const long long cap = std::max(1, cfg.pageMax);
        long long asked = std::min(kDefaultPage, cap);
        const auto p = mini::readInteger(req, "limit", asked);
        if (p == mini::Presence::bad || (p == mini::Presence::ok && asked < 1)) return errorReply("bad_request", "limit must be an integer of at least 1");
        n = static_cast<std::size_t>(std::min(asked, cap));
        return {};
    }

    // Reads `cursor` ("<offset>:<revision>", issued by this plugin); returns an error reply or an empty string.
    static std::string pageStartOf(const std::string& req, const std::string& rev, std::size_t total, std::size_t& start) {
        start = 0;
        std::string c;
        const auto p = mini::readText(req, "cursor", c);
        if (p == mini::Presence::absent) return {};
        if (p == mini::Presence::bad) return errorReply("bad_request", "cursor must be a string");
        const auto colon = c.find(':');
        bool shaped = colon != std::string::npos && colon >= 1 && colon <= 12 && colon + 1 < c.size();
        for (std::size_t i = 0; shaped && i < colon; ++i) shaped = c[i] >= '0' && c[i] <= '9';
        if (!shaped) return errorReply("bad_request", "bad cursor");
        if (c.compare(colon + 1, std::string::npos, rev) != 0) return errorReply("bad_request", "stale_cursor");
        const auto offset = static_cast<std::size_t>(std::strtoull(c.c_str(), nullptr, 10));
        if (offset < 1 || offset >= total) return errorReply("bad_request", "bad cursor");
        start = offset;
        return {};
    }

    static std::string nextCursor(std::size_t end, const std::string& rev) { return std::to_string(end) + ":" + rev; }

    std::string hello(const CatalogConfig& cfg, bool ready) {
        std::shared_ptr<const Snapshot> snap;
        if (ready) snap = snapshot(cfg);
        const bool hasCollections = ready && !validCollections(cfg).empty();

        std::string ops = "\"hello\",\"list\",\"get\"";
        if (hasCollections) ops += ",\"collections\",\"collection\"";
        if (static_cast<int>(access_.load()) >= static_cast<int>(Access::audition)) {
            if (cfg.load) ops += ",\"load\"";
            if (cfg.current) ops += ",\"current\"";
            if (cfg.entitled) ops += ",\"entitled\"";
            if (cfg.referenceState) ops += ",\"referenceState\"";
            ops += ",\"events\"";
        }

        std::string out = "{\"ok\":true,\"relay\":1,\"plugin\":{\"name\":\"" + jsonEscape(name_) + "\",\"id\":\"" + jsonEscape(id_) +
                          "\",\"kind\":\"" + jsonEscape(cfg.kind) + "\"";
        if (!cfg.version.empty()) out += ",\"version\":\"" + jsonEscape(cfg.version) + "\"";
        out += "},\"access\":\"" + std::string(accessName(access_.load())) + "\",\"ops\":[" + ops + "]";
        out += ",\"revision\":\"" + (snap ? jsonEscape(snap->revision) : std::string("building")) + "\"";
        out += ",\"idStability\":\"" + jsonEscape(cfg.idStability) + "\"";
        out += ",\"limits\":{\"pageMax\":" + std::to_string(std::max(1, cfg.pageMax)) + ",\"timeoutMs\":" + std::to_string(waitMs(cfg)) +
               ",\"loadsPerMinute\":" + std::to_string(throttle_.limit()) + (cfg.supportsAsyncLoad ? ",\"loadMode\":\"async\"" : "") + "}";
        out += snap ? ",\"counts\":{\"presets\":" + std::to_string(snap->items.size()) + "}" : ",\"building\":true";
        if (mini::isJsonObject(cfg.policyJson)) out += ",\"policy\":" + cfg.policyJson;
        if (mini::isJsonArray(cfg.packsJson)) out += ",\"packs\":" + cfg.packsJson;
        if (cfg.authorizationJson) {
            const auto a = cfg.authorizationJson();
            if (mini::isJsonObject(a)) out += ",\"authorization\":" + a;
        }
        return out + "}";
    }

    std::string eventsReply(const std::string& req, const CatalogConfig& cfg, bool ready) {
        std::string since;
        const auto p = mini::readText(req, "since", since);
        const std::function<std::string()> rev = [this, cfg, ready] { return ready ? snapshot(cfg)->revision : std::string("building"); };
        if (p == mini::Presence::absent) return events.reply(nullptr, rev);
        if (p == mini::Presence::bad) since = "\x01";   // not a string: no cursor this queue issued
        return events.reply(&since, rev);
    }

    std::string list(const std::string& req, const CatalogConfig& cfg) {
        const auto snap = snapshot(cfg);
        std::size_t n = 0, start = 0;
        if (auto err = pageSizeOf(req, cfg, n); !err.empty()) return err;
        if (auto err = pageStartOf(req, snap->revision, snap->items.size(), start); !err.empty()) return err;

        std::vector<std::string> keep;
        const bool filtered = mini::readStrings(req, "fields", keep) == mini::Presence::ok;
        std::set<std::string> allowed(keep.begin(), keep.end());
        allowed.insert("id");
        allowed.insert("name");

        std::string out = "{\"ok\":true,\"presets\":[";
        std::size_t end = start;
        for (std::size_t i = start; i < snap->items.size() && i < start + n; ++i) {
            const std::string rec = filtered ? mini::withoutKeys(snap->items[i].second, [&](const std::string& k) { return allowed.count(k) == 0; })
                                             : snap->items[i].second;
            if (i > start && out.size() + rec.size() > kMaxReplyBytes - 4096) break;
            if (i > start) out += ",";
            out += rec;
            end = i + 1;
        }
        out += "],\"revision\":\"" + jsonEscape(snap->revision) + "\"";
        if (end < snap->items.size()) out += ",\"next\":\"" + jsonEscape(nextCursor(end, snap->revision)) + "\"";
        return out + "}";
    }

    std::string get(const std::string& req, const CatalogConfig& cfg) {
        std::string id;
        if (mini::readText(req, "id", id) != mini::Presence::ok || !goodId(id)) return errorReply("bad_request", "get needs an id of 1 to 512 bytes");
        const auto snap = snapshot(cfg);
        const auto it = snap->index.find(id);
        if (it == snap->index.end()) return errorReply("not_found", "no such id");
        return "{\"ok\":true,\"preset\":" + snap->items[it->second].second + "}";
    }

    std::string collectionsReply(const std::string& req, const CatalogConfig& cfg, const std::vector<Item>& all) {
        const auto snap = snapshot(cfg);
        std::size_t n = 0, start = 0;
        if (auto err = pageSizeOf(req, cfg, n); !err.empty()) return err;
        if (auto err = pageStartOf(req, snap->revision, all.size(), start); !err.empty()) return err;
        std::string out = "{\"ok\":true,\"collections\":[";
        std::size_t end = start;
        for (std::size_t i = start; i < all.size() && i < start + n; ++i, ++end) {
            if (i > start) out += ",";
            out += mini::withoutKeys(all[i].second, [](const std::string& k) { return k == "presetIds"; });
        }
        out += "]";
        if (end < all.size()) out += ",\"next\":\"" + jsonEscape(nextCursor(end, snap->revision)) + "\"";
        return out + "}";
    }

    std::string collectionReply(const std::string& req, const CatalogConfig& cfg, const std::vector<Item>& all) {
        std::string id;
        if (mini::readText(req, "id", id) != mini::Presence::ok || !goodId(id)) return errorReply("bad_request", "collection needs an id of 1 to 512 bytes");
        std::size_t n = 0;
        if (auto err = pageSizeOf(req, cfg, n); !err.empty()) return err;
        const auto snap = snapshot(cfg);
        for (const auto& c : all) {
            if (c.first != id) continue;
            std::vector<std::string> ids;
            std::string raw;
            if (mini::find(c.second, "presetIds", raw)) mini::arrayElements(raw, ids);
            ids.erase(std::remove_if(ids.begin(), ids.end(), [](const std::string& e) { return e.empty() || e[0] != '"'; }), ids.end());
            std::size_t start = 0;
            if (auto err = pageStartOf(req, snap->revision, ids.size(), start); !err.empty()) return err;
            std::string page;
            std::size_t end = start;
            for (std::size_t i = start; i < ids.size() && i < start + n; ++i, ++end) {
                if (i > start) page += ",";
                page += ids[i];
            }
            std::string obj = mini::withoutKeys(c.second, [](const std::string& k) { return k == "presetIds"; });
            obj.pop_back();   // the closing brace
            if (obj.size() > 1) obj += ",";
            obj += "\"presetIds\":[" + page + "]}";
            std::string out = "{\"ok\":true,\"collection\":" + obj;
            if (end < ids.size()) out += ",\"next\":\"" + jsonEscape(nextCursor(end, snap->revision)) + "\"";
            return out + "}";
        }
        return errorReply("not_found", "no such collection");
    }

    // Runs `fn` through the load hop and the load mutex. Returns an empty string and sets `out`, or an error reply.
    template <class T>
    std::string hop(const CatalogConfig& cfg, std::function<T()> fn, T& out) {
        struct Box {
            T value;
            bool set = false;
        };
        auto box = std::make_shared<Box>();
        std::unique_lock<std::timed_mutex> lock(shared_->loadMutex, std::defer_lock);
        if (!lock.try_lock_for(std::chrono::milliseconds(waitMs(cfg)))) return errorReply("busy", "another load is in progress");
        const std::function<LoadOutcome()> job = [fn, box]() -> LoadOutcome {
            try {
                box->value = fn();
                box->set = true;
                return LoadOutcome::ok();
            } catch (...) {
                return LoadOutcome::failed("the callback threw");
            }
        };
        const LoadOutcome r = cfg.runLoad ? cfg.runLoad(job) : job();
        if (r.kind != LoadOutcome::Ok) return outcomeReply(r);
        if (!box->set) return errorReply("failed", "the callback did not run");
        out = std::move(box->value);
        return {};
    }

    std::string currentReply(const CatalogConfig& cfg) {
        CurrentSound c;
        if (auto err = hop<CurrentSound>(cfg, cfg.current, c); !err.empty()) return err;
        std::string out = "{\"ok\":true,";
        if (goodId(c.id) && validUtf8(c.id)) out += "\"id\":\"" + jsonEscape(c.id) + "\",";
        if (mini::isJsonObject(c.rightsJson)) out += "\"rights\":" + c.rightsJson + ",";
        if (cfg.authorizationJson) {
            const auto a = cfg.authorizationJson();
            if (mini::isJsonObject(a)) out += "\"authorization\":" + a + ",";
        }
        return out + "\"modified\":" + (c.modified ? "true" : "false") + "}";
    }

    std::string entitledReply(const std::string& req, const CatalogConfig& cfg) {
        std::vector<std::string> packs, ids;
        const auto pp = mini::readStrings(req, "packs", packs);
        const auto pi = mini::readStrings(req, "ids", ids);
        if (pp == mini::Presence::bad || pi == mini::Presence::bad) return errorReply("bad_request", "packs and ids must be arrays of strings");
        if (pp == mini::Presence::absent && pi == mini::Presence::absent) return errorReply("bad_request", "entitled needs packs or ids");
        std::vector<std::string> asked = packs;
        asked.insert(asked.end(), ids.begin(), ids.end());
        if (asked.size() > kMaxEntitledItems) return errorReply("bad_request", "too many items");
        for (const auto& s : asked) {
            if (!goodId(s)) return errorReply("bad_request", "each item is 1 to 512 bytes with no NUL");
        }
        const std::map<std::string, std::string> known = cfg.entitled(asked);
        std::set<std::string> done;
        std::string out = "{\"ok\":true,\"entitled\":{";
        bool first = true;
        for (const auto& item : asked) {
            if (!done.insert(item).second) continue;
            const auto it = known.find(item);
            const std::string v = it == known.end() ? "unknown" : it->second;
            const bool good = v == "owned" || v == "not_owned" || v == "unknown";
            if (!first) out += ",";
            first = false;
            out += "\"" + jsonEscape(item) + "\":\"" + (good ? v : std::string("unknown")) + "\"";
        }
        return out + "}}";
    }

    // RFC 4648 section 4: standard alphabet, "=" padding of the last quad only, no line breaks.
    static bool isBase64(const std::string& s) {
        if (s.empty() || s.size() % 4 != 0) return false;
        std::size_t body = s.size();
        if (s[body - 1] == '=') --body;
        if (body > 0 && s[body - 1] == '=') --body;
        for (std::size_t i = 0; i < body; ++i) {
            const char c = s[i];
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/')) return false;
        }
        return true;
    }

    std::string referenceStateReply(const std::string& req, const CatalogConfig& cfg) {
        std::string id;
        if (mini::readText(req, "id", id) != mini::Presence::ok || !goodId(id)) return errorReply("bad_request", "referenceState needs an id of 1 to 512 bytes");
        if (!snapshot(cfg)->index.count(id)) return errorReply("not_found", "no such id");
        ReferenceState r;
        const auto fn = cfg.referenceState;
        if (auto err = hop<ReferenceState>(cfg, [fn, id] { return fn(id); }, r); !err.empty()) return err;
        if (r.outcome.kind != LoadOutcome::Ok) return outcomeReply(r.outcome);
        if (r.stateBase64.size() > kMaxStateChars) return errorReply("failed", "the reference state is larger than 8192 characters");
        if (!isBase64(r.stateBase64) || r.stateKind.empty()) return errorReply("failed", "the plugin returned no usable reference state");
        if (!r.requiresJson.empty() && !mini::isJsonArray(r.requiresJson)) return errorReply("failed", "the plugin returned an unusable requires list");
        return "{\"ok\":true,\"state\":\"" + r.stateBase64 + "\",\"stateKind\":\"" + jsonEscape(r.stateKind) + "\",\"portability\":\"reference\"" +
               (r.requiresJson.empty() ? "" : ",\"requires\":" + r.requiresJson) + "}";
    }

    static std::string outcomeReply(const LoadOutcome& r) {
        switch (r.kind) {
            case LoadOutcome::Ok: return "{\"ok\":true}";
            case LoadOutcome::NotFound: return errorReply("not_found", "no such id");
            case LoadOutcome::NotAvailable: return errorReply("not_available", "its content is not installed on this machine");
            case LoadOutcome::NotEntitled: return errorReply("not_entitled", "the user does not own this pack");
            case LoadOutcome::NeedsAuthorization: return errorReply("needs_authorization", "open the plugin to authorize it", 0, authorizationReason(r.message));
            case LoadOutcome::Busy: return errorReply("busy", "try again shortly");
            default: return errorReply("failed", r.message.empty() ? "load failed" : r.message);
        }
    }

    // The spec's reason names pass through; any other non-empty text becomes "other".
    static std::string authorizationReason(const std::string& m) {
        if (m == "expired" || m == "not_activated" || m == "offline" || m == "revoked") return m;
        return m.empty() ? std::string() : std::string("other");
    }

    // Order: bad_request, not_found, needs_authorization, unsaved, rate_limited, then the load. Only a request that reaches the load uses the throttle.
    std::string loadReply(const std::string& req, const CatalogConfig& cfg) {
        std::string id;
        if (mini::readText(req, "id", id) != mini::Presence::ok || !goodId(id)) return errorReply("bad_request", "load needs an id of 1 to 512 bytes");
        bool discard = false, async = false;
        if (mini::readFlag(req, "discardEdits", discard) == mini::Presence::bad || mini::readFlag(req, "async", async) == mini::Presence::bad) {
            return errorReply("bad_request", "discardEdits and async must be booleans");
        }
        if (!snapshot(cfg)->index.count(id)) return errorReply("not_found", "no such id");

        if (cfg.authorizationJson) {
            const auto a = cfg.authorizationJson();
            std::string state, reason;
            if (mini::isJsonObject(a) && mini::getString(a, "state", state) && state == "needed") {
                mini::getString(a, "reason", reason);
                return errorReply("needs_authorization", "open the plugin to authorize it", 0, authorizationReason(reason));
            }
        }
        if (cfg.current && !discard) {
            CurrentSound c;
            if (auto err = hop<CurrentSound>(cfg, cfg.current, c); !err.empty()) return err;
            if (c.modified) return errorReply("unsaved", "loading would discard unsaved edits; retry with discardEdits:true");
        }
        if (const int wait = throttle_.tryUse()) return errorReply("rate_limited", "too many loads", wait);

        const auto fn = cfg.load;
        const auto hopFn = cfg.runLoad;
        const std::function<LoadOutcome()> job = [fn, id]() -> LoadOutcome {
            try { return fn(id); } catch (...) { return LoadOutcome::failed("the callback threw"); }
        };
        const std::function<LoadOutcome()> run = [hopFn, job] { return hopFn ? hopFn(job) : job(); };
        if (cfg.supportsAsyncLoad && async) return startAsyncLoad(cfg, id, run);

        std::unique_lock<std::timed_mutex> lock(shared_->loadMutex, std::defer_lock);
        if (!lock.try_lock_for(std::chrono::milliseconds(waitMs(cfg)))) return errorReply("busy", "another load is in progress");
        return outcomeReply(run());
    }

    std::string startAsyncLoad(const CatalogConfig& cfg, const std::string& id, const std::function<LoadOutcome()>& run) {
        const int lockMs = waitMs(cfg);
        const auto shared = shared_;
        AsyncJob job;
        job.id = id;
        job.events = events;
        job.run = [shared, run, lockMs]() -> LoadOutcome {
            std::unique_lock<std::timed_mutex> lock(shared->loadMutex, std::defer_lock);
            if (!lock.try_lock_for(std::chrono::milliseconds(lockMs))) return LoadOutcome::busy();
            return run();
        };

        std::lock_guard<std::mutex> workerGuard(workerMutex_);
        {
            std::lock_guard<std::mutex> g(shared->m);
            if (shared->stop) return errorReply("unsupported", "the PresetRelay is switched off in this plugin");
            if (shared->queue.size() + (shared->running ? 1 : 0) >= kMaxAsyncLoads) return errorReply("busy", "too many loads in progress");
        }
        if (!worker_.joinable()) {
            try {
                worker_ = std::thread([shared] { workerLoop(shared); });
            } catch (...) {
                return errorReply("failed", "could not start a thread for the load");
            }
        }
        {
            std::lock_guard<std::mutex> g(shared->m);
            shared->queue.push_back(std::move(job));
            shared->cv.notify_all();
        }
        return "{\"ok\":true,\"pending\":true}";
    }

    static void workerLoop(const std::shared_ptr<Shared>& shared) {
        for (;;) {
            AsyncJob job;
            {
                std::unique_lock<std::mutex> lk(shared->m);
                shared->cv.wait(lk, [&] { return shared->stop || !shared->queue.empty(); });
                if (shared->stop) break;
                job = std::move(shared->queue.front());
                shared->queue.pop_front();
                shared->running = true;
            }
            LoadOutcome r;
            try { r = job.run(); } catch (...) { r = LoadOutcome::failed("the callback threw"); }
            try {
                if (r.kind == LoadOutcome::Ok) {
                    job.events.post("{\"type\":\"loaded\",\"id\":\"" + jsonEscape(job.id) + "\"}");
                } else {
                    const std::string prefix = "{\"ok\":false,\"error\":";
                    job.events.post("{\"type\":\"loadFailed\",\"id\":\"" + jsonEscape(job.id) + "\",\"error\":" + outcomeReply(r).substr(prefix.size()));
                }
            } catch (...) {
            }
            std::lock_guard<std::mutex> g(shared->m);
            shared->running = false;
        }
        std::lock_guard<std::mutex> g(shared->m);
        shared->exited = true;
        shared->cv.notify_all();
    }

    std::string name_, id_;
    std::atomic<Access> access_;
    std::atomic<bool> enabled_{true}, stopped_{false};
    Throttle throttle_;
    std::mutex configMutex_;
    mutable std::mutex cacheMutex_;
    mutable std::shared_ptr<const Snapshot> cache_;
    std::shared_ptr<Shared> shared_;
    std::mutex workerMutex_;
    std::thread worker_;
};

}  // namespace presetrelay
