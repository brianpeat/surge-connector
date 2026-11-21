// SPDX-License-Identifier: Apache-2.0
// PresetRelay catalog helper for C++ (spec v0.2 draft). Header only, C++17, depends only on PresetRelay.h.
//
// The one-file way in: give a Catalog your sounds and a few callbacks and it answers hello, list, get, collections, collection, load, current,
// entitled and events, including paging, limits, the load throttle, unsaved-edit protection, async loads and event bookkeeping. You never parse
// or write the protocol. It needs no JSON library: you hand it each sound as one JSON object string (build it with whatever you already use,
// or by hand with presetrelay::jsonEscape); the small scanner inside reads only the top-level "id" and "name" of those strings and of requests.
//
//     presetrelay::Catalog catalog("My Synth", "com.me.mysynth", presetrelay::Access::audition);   // ONE per process
//     catalog.records  = [] { return cachedRecordJson; };                    // std::vector<std::string>, each {"id":"...","name":"...",...}
//     catalog.revision = [] { return presetrelay::revisionOf({"v1", "<file stamps>"}); };
//     catalog.isReady  = [] { return catalogBuilt.load(); };                // false while building: list/get answer busy
//     catalog.load     = [](const std::string& id) { return mySynth.select(id) ? presetrelay::LoadOutcome::ok() : presetrelay::LoadOutcome::notFound(); };
//     catalog.current  = [] { return presetrelay::CurrentSound{mySynth.currentId(), mySynth.hasUnsavedEdits()}; };
//     // then hand `&catalog` to your doorway (AU property hook, VST3 doorway, CLAP extension, C API) as the presetrelay::Handler.
//
// Threading: `records`, `load` etc. are called on whatever thread the host's request arrives on (never the audio thread). If sound changes must
// happen on your UI/message thread, set `runLoad` to a function that runs the given job there and WAITS for it (JUCE: MessageManager::callFunctionOnMessageThread).
#pragma once
#include <atomic>
#include <cctype>
#include <cstdint>
#include <algorithm>
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
#include <vector>

#include "PresetRelay.h"

namespace presetrelay {

// ---- Tiny JSON helpers (flat scanning of top-level keys; not a general parser) ----
namespace mini {

inline bool isSpace(char c) { return c == ' ' || c == '\n' || c == '\t' || c == '\r'; }

// Position just past whitespace.
inline std::size_t skipWs(const std::string& s, std::size_t i) {
    while (i < s.size() && isSpace(s[i])) ++i;
    return i;
}

// End (exclusive) of the JSON value starting at i, or npos if malformed.
inline std::size_t valueEnd(const std::string& s, std::size_t i) {
    i = skipWs(s, i);
    if (i >= s.size()) return std::string::npos;
    if (s[i] == '"') {
        for (++i; i < s.size(); ++i) {
            if (s[i] == '\\') ++i;
            else if (s[i] == '"') return i + 1;
        }
        return std::string::npos;
    }
    if (s[i] == '{' || s[i] == '[') {
        int depth = 0;
        for (; i < s.size(); ++i) {
            if (s[i] == '"') {
                const auto e = valueEnd(s, i);
                if (e == std::string::npos) return e;
                i = e - 1;
            } else if (s[i] == '{' || s[i] == '[') {
                ++depth;
            } else if (s[i] == '}' || s[i] == ']') {
                if (--depth == 0) return i + 1;
            }
        }
        return std::string::npos;
    }
    while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' && !isSpace(s[i])) ++i;
    return i;
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
        const auto keyEnd = valueEnd(obj, i);
        if (keyEnd == std::string::npos || obj[i] != '"') return false;
        const std::string k = obj.substr(i + 1, keyEnd - i - 2);
        i = skipWs(obj, keyEnd);
        if (i >= obj.size() || obj[i] != ':') return false;
        i = skipWs(obj, i + 1);
        const auto end = valueEnd(obj, i);
        if (end == std::string::npos) return false;
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
                    // High surrogate: join it with the \uDC00..DFFF that follows into one code point.
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

// Array of strings, e.g. "ids":["a","b"].
inline std::vector<std::string> getStringArray(const std::string& obj, const std::string& key) {
    std::vector<std::string> out;
    std::string raw;
    if (!find(obj, key, raw) || raw.empty() || raw[0] != '[') return out;
    for (std::size_t i = 1; i < raw.size();) {
        i = skipWs(raw, i);
        if (i >= raw.size() || raw[i] == ']') break;
        if (raw[i] == ',') { ++i; continue; }
        const auto end = valueEnd(raw, i);
        if (end == std::string::npos) break;
        std::string v;
        if (raw[i] == '"' && asString(raw.substr(i, end - i), v)) out.push_back(v);
        i = end;
    }
    return out;
}

}  // namespace mini

// ---- Revision ----

// FNV-1a hash of the parts that change your catalog (file stamps, a version string, ids).
inline std::string revisionOf(const std::vector<std::string>& parts) {
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (const auto& p : parts) {
        for (unsigned char c : p) { h ^= c; h *= 0x100000001b3ull; }
        h ^= 0x1f;
        h *= 0x100000001b3ull;
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, "%llx", static_cast<unsigned long long>(h));
    return buf;
}

// ---- Events (pull model) ----

class EventQueue {
public:
    // `eventJson` is one JSON object, e.g. {"type":"loaded","id":"..."}. The queue keeps the newest 256.
    void post(const std::string& eventJson) {
        std::lock_guard<std::mutex> g(m_);
        ++seq_;
        items_.push_back({seq_, eventJson});
        if (items_.size() > 256) items_.pop_front();
    }

    // `since` empty: only establish the cursor.
    std::string reply(const std::string& since) {
        std::lock_guard<std::mutex> g(m_);
        const std::string head = "{\"ok\":true,\"events\":[";
        if (since.empty()) return head + "],\"next\":\"" + std::to_string(seq_) + "\"}";
        const long long from = std::atoll(since.c_str());
        // A cursor from before a restart: start over from here.
        if (from > seq_) return head + "],\"next\":\"" + std::to_string(seq_) + "\"}";
        std::string events;
        long long last = from;
        int n = 0;
        for (const auto& it : items_) {
            if (it.first <= from || n >= 100) continue;
            if (n++) events += ",";
            events += it.second;
            last = it.first;
        }
        return head + events + "],\"next\":\"" + std::to_string(last) + "\"}";
    }

private:
    std::mutex m_;
    long long seq_ = 0;
    std::deque<std::pair<long long, std::string>> items_;
};

// ---- Callback results ----

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

// What `referenceState` returns (spec 6, 5.2.3). `outcome` is anything but Ok when no state can be made.
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

// ---- The catalog ----

class Catalog : public Handler {
public:
    Catalog(std::string name, std::string id, Access access = Access::catalog, int loadsPerMinute = 10)
        : name_(std::move(name)), id_(std::move(id)), access_(access), throttle_(loadsPerMinute) {}
    ~Catalog() override {
        for (auto& j : asyncJobs_) if (j.t.joinable()) j.t.join();
    }

    // What you describe.
    std::string kind = "instrument";                  // instrument | effect | midi-effect | other
    std::string version;
    std::string idStability = "permanent";            // permanent | per-install | session
    std::string policyJson;                           // optional JSON object, spec 3.4
    std::string packsJson;                            // optional JSON array of {id, name, rights?, policy?}, spec 7
    int timeoutMs = 10000;                            // what you promise to answer within, at most 60000
    bool supportsAsyncLoad = false;                   // honour "async":true on load

    // What you provide (only `records` is required).
    std::function<std::vector<std::string>()> records;      // each a JSON object string with "id" and "name"
    std::function<std::string()> revision;                  // default: hash of the ids
    std::function<bool()> isReady;                          // default: always ready
    std::function<std::vector<std::string>()> collections;  // JSON object strings {id,name,kind,presetIds:[...]}
    std::function<LoadOutcome(const std::string& id)> load; // apply the sound; return only after it has taken effect
    std::function<CurrentSound()> current;
    // Plugin-wide authorization state as a JSON object, e.g. {"state":"needed","reason":"expired"} (spec 6). Reported in hello and current.
    std::function<std::string()> authorizationJson;
    // item -> owned | not_owned | unknown
    std::function<std::map<std::string, std::string>(const std::vector<std::string>& items)> entitled;
    // Level 2: a small state that points at the sound inside the plugin; never the program, samples or anything decrypted.
    std::function<ReferenceState(const std::string& id)> referenceState;
    // Runs the job on your UI thread and waits for it. Default: runs it on the calling thread.
    std::function<LoadOutcome(const std::function<LoadOutcome()>& job)> runLoad;
    EventQueue events;

    Access access() const { return access_; }
    void setAccess(Access a) { access_ = a; }

    // Kill switch: while false, every request gets an `unsupported` error with no `relay` key, so a host concludes the plugin
    // does not take part. Can be flipped at any time.
    void setEnabled(bool on) { enabled_ = on; }
    bool isEnabled() const { return enabled_; }

    std::string handleRequest(const std::string& req) override {
        if (!enabled_) return errorReply("unsupported", "the PresetRelay is switched off in this plugin");
        std::string op;
        if (!mini::getString(req, "op", op)) return errorReply("bad_request", "request needs a string \"op\"");
        const int need = opLevel(op);
        if (need == 0) return errorReply("unsupported", "unknown operation " + op);
        if (static_cast<int>(access_) < need) return errorReply("unsupported", op + " is not offered at this access level");
        if (op == "hello") return hello();
        if (op == "events") {
            std::string since;
            mini::getString(req, "since", since);
            return events.reply(since);
        }
        if ((op == "list" || op == "get" || op == "load") && !ready()) return errorReply("busy", "the catalog is still being built");
        if (op == "list") return list(req);
        if (op == "get") return get(req);
        if ((op == "collections" || op == "collection") && (!collections || collections().empty())) return errorReply("unsupported", "this plugin has no collections");
        if (op == "collections") return collectionsReply(req);
        if (op == "collection") return collectionReply(req);
        if (op == "load") return loadReply(req);
        if (op == "current") return currentReply();
        if (op == "entitled") return entitledReply(req);
        if (op == "referenceState") return referenceStateReply(req);
        return errorReply("unsupported", "not implemented");
    }

private:
    using Item = std::pair<std::string, std::string>;   // {id, record json}

    // Lowest access level at which an operation is offered (spec 6); 0 = unknown.
    static int opLevel(const std::string& op) {
        if (op == "hello" || op == "list" || op == "get" || op == "collections" || op == "collection") return 1;
        if (op == "load" || op == "current" || op == "entitled" || op == "events" || op == "referenceState") return 2;
        if (op == "params" || op == "search") return 3;
        if (op == "exportState") return 4;
        return 0;
    }

    bool ready() const { return !isReady || isReady(); }

    // Records with a usable id and name, duplicates dropped (a host drops ids over 512 bytes, so none are sent).
    std::vector<Item> valid() const {
        std::vector<Item> out;
        std::set<std::string> seen;
        if (!records) return out;
        for (auto& r : records()) {
            std::string id, name;
            if (!mini::getString(r, "id", id) || id.empty() || id.size() > kMaxIdBytes || !mini::getString(r, "name", name)) continue;
            if (seen.insert(id).second) out.emplace_back(id, std::move(r));
        }
        return out;
    }

    bool hasId(const std::string& id) const {
        for (auto& p : valid()) if (p.first == id) return true;
        return false;
    }

    std::string revisionFor(const std::vector<Item>& items) const {
        if (revision) return revision();
        std::vector<std::string> ids;
        for (auto& p : items) ids.push_back(p.first);
        return revisionOf(ids);
    }

    static std::size_t pageStart(const std::string& req) {
        std::string c;
        if (!mini::getString(req, "cursor", c)) return 0;
        return static_cast<std::size_t>(std::max(0LL, std::atoll(c.c_str())));
    }

    static std::size_t pageSize(const std::string& req) {
        long long n = 100;
        mini::getInt(req, "limit", n);
        return static_cast<std::size_t>(std::max(1LL, std::min<long long>(n, kMaxPage)));
    }

    std::string hello() const {
        const bool isReadyNow = ready();
        std::vector<Item> items;
        if (isReadyNow) items = valid();

        std::string ops = "\"hello\",\"list\",\"get\"";
        if (collections && !collections().empty()) ops += ",\"collections\",\"collection\"";
        if (static_cast<int>(access_) >= static_cast<int>(Access::audition)) {
            if (load) ops += ",\"load\"";
            if (current) ops += ",\"current\"";
            if (entitled) ops += ",\"entitled\"";
            if (referenceState) ops += ",\"referenceState\"";
            ops += ",\"events\"";
        }

        std::string out = "{\"ok\":true,\"relay\":1,\"plugin\":{\"name\":\"" + jsonEscape(name_) + "\",\"id\":\"" + jsonEscape(id_) +
                          "\",\"kind\":\"" + jsonEscape(kind) + "\"";
        if (!version.empty()) out += ",\"version\":\"" + jsonEscape(version) + "\"";
        out += "},\"access\":\"" + std::string(accessName(access_)) + "\",\"ops\":[" + ops + "]";
        out += ",\"revision\":\"" + (isReadyNow ? jsonEscape(revisionFor(items)) : std::string("building")) + "\"";
        out += ",\"idStability\":\"" + jsonEscape(idStability) + "\"";
        out += ",\"limits\":{\"pageMax\":" + std::to_string(kMaxPage) + ",\"timeoutMs\":" + std::to_string(std::min(timeoutMs, 60000)) +
               ",\"loadsPerMinute\":" + std::to_string(throttle_.limit()) + (supportsAsyncLoad ? ",\"loadMode\":\"async\"" : "") + "}";
        out += isReadyNow ? ",\"counts\":{\"presets\":" + std::to_string(items.size()) + "}" : ",\"building\":true";
        if (!policyJson.empty()) out += ",\"policy\":" + policyJson;
        if (!packsJson.empty()) out += ",\"packs\":" + packsJson;
        if (authorizationJson) {
            const auto a = authorizationJson();
            if (!a.empty()) out += ",\"authorization\":" + a;
        }
        return out + "}";
    }

    std::string list(const std::string& req) const {
        const auto items = valid();
        const auto rev = revisionFor(items);
        const auto start = pageStart(req);
        const auto n = pageSize(req);
        std::string out = "{\"ok\":true,\"presets\":[";
        std::size_t end = start;
        for (std::size_t i = start; i < items.size() && i < start + n; ++i, ++end) {
            if (i > start) out += ",";
            out += items[i].second;
        }
        out += "],\"revision\":\"" + jsonEscape(rev) + "\"";
        if (end < items.size()) out += ",\"next\":\"" + std::to_string(end) + "\"";
        return out + "}";
    }

    std::string get(const std::string& req) const {
        std::string id;
        if (!mini::getString(req, "id", id)) return errorReply("bad_request", "get needs an id");
        for (auto& p : valid()) {
            if (p.first == id) return "{\"ok\":true,\"preset\":" + p.second + "}";
        }
        return errorReply("not_found", "no such id");
    }

    std::string collectionsReply(const std::string& req) const {
        const auto all = collections ? collections() : std::vector<std::string>{};
        const auto start = pageStart(req);
        const auto n = pageSize(req);
        std::string out = "{\"ok\":true,\"collections\":[";
        std::size_t end = start;
        for (std::size_t i = start; i < all.size() && i < start + n; ++i, ++end) {
            if (i > start) out += ",";
            out += all[i];
        }
        out += "]";
        if (end < all.size()) out += ",\"next\":\"" + std::to_string(end) + "\"";
        return out + "}";
    }

    std::string collectionReply(const std::string& req) const {
        std::string id;
        if (!mini::getString(req, "id", id)) return errorReply("bad_request", "collection needs an id");
        for (auto& c : (collections ? collections() : std::vector<std::string>{})) {
            std::string cid;
            if (!mini::getString(c, "id", cid) || cid != id) continue;
            // presetIds go out as the plugin gave them; paging a very large collection is the plugin's choice.
            return "{\"ok\":true,\"collection\":" + c + "}";
        }
        return errorReply("not_found", "no such collection");
    }

    std::string currentReply() const {
        if (!current) return errorReply("unsupported", "not implemented");
        const auto c = current();
        std::string out = "{\"ok\":true,";
        if (!c.id.empty()) out += "\"id\":\"" + jsonEscape(c.id) + "\",";
        if (!c.rightsJson.empty()) out += "\"rights\":" + c.rightsJson + ",";
        if (authorizationJson) {
            const auto a = authorizationJson();
            if (!a.empty()) out += "\"authorization\":" + a + ",";
        }
        return out + "\"modified\":" + (c.modified ? "true" : "false") + "}";
    }

    std::string entitledReply(const std::string& req) const {
        if (!entitled) return errorReply("unsupported", "not implemented");
        auto asked = mini::getStringArray(req, "packs");
        for (auto& s : mini::getStringArray(req, "ids")) asked.push_back(s);
        std::string out = "{\"ok\":true,\"entitled\":{";
        bool first = true;
        for (auto& kv : entitled(asked)) {
            if (!first) out += ",";
            first = false;
            out += "\"" + jsonEscape(kv.first) + "\":\"" + jsonEscape(kv.second) + "\"";
        }
        return out + "}}";
    }

    static bool isBase64(const std::string& s) {
        if (s.empty() || s.size() % 4 != 0) return false;
        for (unsigned char c : s) {
            if (!(std::isalnum(c) || c == '+' || c == '/' || c == '=')) return false;
        }
        return true;
    }

    std::string referenceStateReply(const std::string& req) const {
        if (!referenceState) return errorReply("unsupported", "not implemented");
        std::string id;
        if (!mini::getString(req, "id", id)) return errorReply("bad_request", "referenceState needs an id");
        if (!ready()) return errorReply("busy", "the catalog is still being built");
        if (!hasId(id)) return errorReply("not_found", "no such id");
        const auto r = referenceState(id);
        if (r.outcome.kind != LoadOutcome::Ok) return outcomeReply(r.outcome);
        if (!isBase64(r.stateBase64) || r.stateKind.empty()) return errorReply("failed", "the plugin returned no usable reference state");
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

    std::string loadReply(const std::string& req) {
        if (!load) return errorReply("unsupported", "this plugin does not load by id");
        std::string id;
        if (!mini::getString(req, "id", id)) return errorReply("bad_request", "load needs an id");
        if (!hasId(id)) return errorReply("not_found", "no such id");

        bool discard = false;
        mini::getBool(req, "discardEdits", discard);
        if (current) {
            const auto c = current();
            if (c.modified && !discard) return errorReply("unsaved", "loading would discard unsaved edits; retry with discardEdits:true");
        }
        if (const int wait = throttle_.tryUse()) return errorReply("rate_limited", "too many loads", wait);

        auto job = [this, id] { return load(id); };
        auto run = [this, job] { return runLoad ? runLoad(job) : job(); };
        bool async = false;
        mini::getBool(req, "async", async);
        if (supportsAsyncLoad && async) return startAsyncLoad(id, run);
        return outcomeReply(run());
    }

    std::string startAsyncLoad(const std::string& id, std::function<LoadOutcome()> run) {
        std::lock_guard<std::mutex> g(jobsMutex_);
        std::size_t running = 0;
        for (auto it = asyncJobs_.begin(); it != asyncJobs_.end();) {
            if (it->done->load()) {
                if (it->t.joinable()) it->t.join();
                it = asyncJobs_.erase(it);
            } else {
                ++running;
                ++it;
            }
        }
        if (running >= kMaxAsyncLoads) return errorReply("busy", "too many loads in progress");

        auto done = std::make_shared<std::atomic<bool>>(false);
        asyncJobs_.push_back({std::thread(), done});
        asyncJobs_.back().t = std::thread([this, id, run, done] {
            struct Finish {
                std::shared_ptr<std::atomic<bool>> d;
                ~Finish() { d->store(true); }
            } finish{done};
            const auto r = run();
            if (r.kind == LoadOutcome::Ok) {
                events.post("{\"type\":\"loaded\",\"id\":\"" + jsonEscape(id) + "\"}");
            } else {
                const std::string prefix = "{\"ok\":false,\"error\":";
                events.post("{\"type\":\"loadFailed\",\"id\":\"" + jsonEscape(id) + "\",\"error\":" + outcomeReply(r).substr(prefix.size()));
            }
        });
        return "{\"ok\":true,\"pending\":true}";
    }

    struct AsyncJob {
        std::thread t;
        std::shared_ptr<std::atomic<bool>> done;
    };

    static constexpr std::size_t kMaxIdBytes = 512;
    static constexpr long long kMaxPage = 500;
    static constexpr std::size_t kMaxAsyncLoads = 16;

    std::string name_, id_;
    Access access_;
    std::atomic<bool> enabled_{true};
    mutable Throttle throttle_;
    std::mutex jobsMutex_;
    std::vector<AsyncJob> asyncJobs_;
};

}  // namespace presetrelay
