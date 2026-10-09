// SPDX-License-Identifier: Apache-2.0
// Preset Bridge catalog helper for C++ (spec v0.2 draft). Apache License 2.0. Header only, C++17, depends only on PresetBridge.h.
//
// The one-file way in: give a Catalog your sounds and a few callbacks and it answers hello, list, get, collections, collection, load, current,
// entitled and events, including paging, limits, the load throttle, unsaved-edit protection, async loads and event bookkeeping. You never parse
// or write the protocol. It needs no JSON library: you hand it each sound as one JSON object string (build it with whatever you already use,
// or by hand with presetbridge::jsonEscape); the small scanner inside reads only the top-level "id" and "name" of those strings and of requests.
//
//     presetbridge::Catalog catalog("My Synth", "com.me.mysynth", presetbridge::Access::audition);   // ONE per process
//     catalog.records  = [] { return cachedRecordJson; };                    // std::vector<std::string>, each {"id":"...","name":"...",...}
//     catalog.revision = [] { return presetbridge::revisionOf({"v1", "<file stamps>"}); };
//     catalog.isReady  = [] { return catalogBuilt.load(); };                // false while building: list/get answer busy
//     catalog.load     = [](const std::string& id) { return mySynth.select(id) ? presetbridge::LoadOutcome::ok() : presetbridge::LoadOutcome::notFound(); };
//     catalog.current  = [] { return presetbridge::CurrentSound{mySynth.currentId(), mySynth.hasUnsavedEdits()}; };
//     // then hand `&catalog` to your doorway (AU property hook, VST3 doorway, CLAP extension, C API) as the presetbridge::Handler.
//
// Threading: `records`, `load` etc. are called on whatever thread the host's request arrives on (never the audio thread). If sound changes must
// happen on your UI/message thread, set `runLoad` to a function that runs the given job there and WAITS for it (JUCE: MessageManager::callFunctionOnMessageThread).
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "PresetBridge.h"

namespace presetbridge {

// ---- Tiny JSON helpers (flat scanning of top-level keys; not a general parser) ------------------------------------------------------
namespace mini {
// Position just past whitespace.
inline std::size_t skipWs(const std::string& s, std::size_t i) { while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\t' || s[i] == '\r')) ++i; return i; }
// End (exclusive) of the JSON value starting at i, or npos if malformed.
inline std::size_t valueEnd(const std::string& s, std::size_t i) {
    i = skipWs(s, i);
    if (i >= s.size()) return std::string::npos;
    if (s[i] == '"') { for (++i; i < s.size(); ++i) { if (s[i] == '\\') ++i; else if (s[i] == '"') return i + 1; } return std::string::npos; }
    if (s[i] == '{' || s[i] == '[') {
        int depth = 0;
        for (; i < s.size(); ++i) {
            if (s[i] == '"') { const auto e = valueEnd(s, i); if (e == std::string::npos) return e; i = e - 1; }
            else if (s[i] == '{' || s[i] == '[') ++depth;
            else if (s[i] == '}' || s[i] == ']') { if (--depth == 0) return i + 1; }
        }
        return std::string::npos;
    }
    while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' && s[i] != ' ' && s[i] != '\n' && s[i] != '\t' && s[i] != '\r') ++i;
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
        const auto ke = valueEnd(obj, i);
        if (ke == std::string::npos || obj[i] != '"') return false;
        const std::string k = obj.substr(i + 1, ke - i - 2);
        i = skipWs(obj, ke);
        if (i >= obj.size() || obj[i] != ':') return false;
        i = skipWs(obj, i + 1);
        const auto ve = valueEnd(obj, i);
        if (ve == std::string::npos) return false;
        if (k == key) { raw = obj.substr(i, ve - i); return true; }
        i = ve;
    }
}
inline bool asString(const std::string& raw, std::string& out) {
    if (raw.size() < 2 || raw.front() != '"') return false;
    out.clear();
    for (std::size_t i = 1; i + 1 < raw.size(); ++i) {
        if (raw[i] != '\\') { out += raw[i]; continue; }
        if (++i + 1 > raw.size()) break;
        switch (raw[i]) {
            case 'n': out += '\n'; break; case 't': out += '\t'; break; case 'r': out += '\r'; break; case 'b': out += '\b'; break; case 'f': out += '\f'; break;
            case 'u': if (i + 4 < raw.size()) { const auto cp = std::strtoul(raw.substr(i + 1, 4).c_str(), nullptr, 16); i += 4;
                          if (cp < 0x80) out += static_cast<char>(cp); else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
                          else { out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); } } break;
            default: out += raw[i];
        }
    }
    return true;
}
inline bool getString(const std::string& obj, const std::string& key, std::string& out) { std::string raw; return find(obj, key, raw) && asString(raw, out); }
inline bool getInt(const std::string& obj, const std::string& key, long long& out) { std::string raw; if (!find(obj, key, raw) || raw.empty()) return false; char* e = nullptr; out = std::strtoll(raw.c_str(), &e, 10); return e != raw.c_str(); }
inline bool getBool(const std::string& obj, const std::string& key, bool& out) { std::string raw; if (!find(obj, key, raw)) return false; if (raw == "true" || raw == "1") { out = true; return true; } if (raw == "false" || raw == "0") { out = false; return true; } return false; }
// Array of strings, e.g. "ids":["a","b"].
inline std::vector<std::string> getStringArray(const std::string& obj, const std::string& key) {
    std::vector<std::string> out; std::string raw;
    if (!find(obj, key, raw) || raw.empty() || raw[0] != '[') return out;
    for (std::size_t i = 1; i < raw.size();) {
        i = skipWs(raw, i);
        if (i >= raw.size() || raw[i] == ']') break;
        if (raw[i] == ',') { ++i; continue; }
        const auto e = valueEnd(raw, i); if (e == std::string::npos) break;
        std::string v; if (raw[i] == '"' && asString(raw.substr(i, e - i), v)) out.push_back(v);
        i = e;
    }
    return out;
}
}  // namespace mini

// ---- Revision (hash of the things that change your catalog) --------------------------------------------------------------------------
inline std::string revisionOf(const std::vector<std::string>& parts) {
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (const auto& p : parts) { for (unsigned char c : p) { h ^= c; h *= 0x100000001b3ull; } h ^= 0x1f; h *= 0x100000001b3ull; }
    char buf[32]; std::snprintf(buf, sizeof buf, "%llx", static_cast<unsigned long long>(h));
    return buf;
}

// ---- Events (pull model) -----------------------------------------------------------------------------------------------------------
class EventQueue {
public:
    // `eventJson` is one JSON object, e.g. {"type":"loaded","id":"..."}.
    void post(const std::string& eventJson) { std::lock_guard<std::mutex> g(m_); ++seq_; items_.push_back({seq_, eventJson}); if (items_.size() > 256) items_.pop_front(); }
    // `since` empty: only establish the cursor.
    std::string reply(const std::string& since) {
        std::lock_guard<std::mutex> g(m_);
        if (since.empty()) return "{\"ok\":true,\"events\":[],\"next\":\"" + std::to_string(seq_) + "\"}";
        const long long from = std::atoll(since.c_str());
        std::string events; long long last = from; int n = 0;
        for (const auto& it : items_) if (it.first > from && n < 100) { if (n++) events += ","; events += it.second; last = it.first; }
        return "{\"ok\":true,\"events\":[" + events + "],\"next\":\"" + std::to_string(last) + "\"}";
    }
private:
    std::mutex m_; long long seq_ = 0; std::deque<std::pair<long long, std::string>> items_;
};

// ---- Callback results ------------------------------------------------------------------------------------------------------------------
struct LoadOutcome {
    enum Kind { Ok, NotFound, NotAvailable, NotEntitled, Busy, Failed } kind = Ok;
    std::string message;
    static LoadOutcome ok() { return {Ok, {}}; }
    static LoadOutcome notFound() { return {NotFound, {}}; }
    static LoadOutcome notAvailable() { return {NotAvailable, {}}; }
    static LoadOutcome notEntitled() { return {NotEntitled, {}}; }
    static LoadOutcome busy() { return {Busy, {}}; }
    static LoadOutcome failed(std::string why) { return {Failed, std::move(why)}; }
};
struct CurrentSound { std::string id; bool modified = false; };   // empty id = unknown

// ---- The catalog ---------------------------------------------------------------------------------------------------------------------------
class Catalog : public Handler {
public:
    Catalog(std::string name, std::string id, Access access = Access::catalog, int loadsPerMinute = 10)
        : name_(std::move(name)), id_(std::move(id)), access_(access), throttle_(loadsPerMinute) {}
    ~Catalog() override { for (auto& t : asyncJobs_) if (t.joinable()) t.join(); }

    // What you describe.
    std::string kind = "instrument";                  // instrument | effect | midi-effect | other
    std::string version;
    std::string idStability = "permanent";            // permanent | per-install | session
    std::string policyJson;                           // optional JSON object, spec 3.4
    int timeoutMs = 10000;                            // what you promise to answer within, at most 60000
    bool supportsAsyncLoad = false;                   // honour "async":true on load

    // What you provide (only `records` is required).
    std::function<std::vector<std::string>()> records;                         // each a JSON object string with "id" and "name"
    std::function<std::string()> revision;                                     // default: hash of the ids
    std::function<bool()> isReady;                                             // default: always ready
    std::function<std::vector<std::string>()> collections;                     // JSON object strings {id,name,kind,presetIds:[...]}
    std::function<LoadOutcome(const std::string& id)> load;                    // apply the sound; return only after it has taken effect
    std::function<CurrentSound()> current;
    std::function<std::map<std::string, std::string>(const std::vector<std::string>& items)> entitled;   // item -> owned|not_owned|unknown
    std::function<LoadOutcome(const std::function<LoadOutcome()>& job)> runLoad;  // run the job on your UI thread and wait; default: run here
    EventQueue events;

    Access access() const { return access_; }
    void setAccess(Access a) { access_ = a; }

    std::string handleRequest(const std::string& req) override {
        std::string op;
        if (!mini::getString(req, "op", op)) return errorReply("bad_request", "request needs a string \"op\"");
        const int need = opLevel(op);
        if (need == 0) return errorReply("unsupported", "unknown operation " + op);
        if (static_cast<int>(access_) < need) return errorReply("unsupported", op + " is not offered at this access level");
        if (op == "hello") return hello();
        if (op == "events") { std::string since; mini::getString(req, "since", since); return events.reply(since); }
        if (op == "list" || op == "get" || op == "load") if (ready() == false) return errorReply("busy", "the catalog is still being built");
        if (op == "list") return list(req);
        if (op == "get") return get(req);
        if (op == "collections") return collectionsReply(req);
        if (op == "collection") return collectionReply(req);
        if (op == "load") return loadReply(req);
        if (op == "current") { if (!current) return errorReply("unsupported", "not implemented"); const auto c = current(); return std::string("{\"ok\":true,") + (c.id.empty() ? "" : "\"id\":\"" + jsonEscape(c.id) + "\",") + "\"modified\":" + (c.modified ? "true" : "false") + "}"; }
        if (op == "entitled") return entitledReply(req);
        return errorReply("unsupported", "not implemented");
    }

private:
    static int opLevel(const std::string& op) {
        if (op == "hello" || op == "list" || op == "get" || op == "collections" || op == "collection") return 1;
        if (op == "load" || op == "current" || op == "entitled" || op == "events") return 2;
        if (op == "params" || op == "search") return 3;
        if (op == "exportState") return 4;
        return 0;
    }
    bool ready() const { return !isReady || isReady(); }

    // Records with a usable id and name, duplicates dropped (a host drops ids over 512 bytes, so none are sent). Each entry is {id, json}.
    std::vector<std::pair<std::string, std::string>> valid() const {
        std::vector<std::pair<std::string, std::string>> out; std::set<std::string> seen;
        if (!records) return out;
        for (auto& r : records()) {
            std::string id, nm;
            if (!mini::getString(r, "id", id) || id.empty() || id.size() > kMaxIdBytes || !mini::getString(r, "name", nm)) continue;
            if (seen.insert(id).second) out.emplace_back(id, std::move(r));
        }
        return out;
    }
    std::string revisionFor(const std::vector<std::pair<std::string, std::string>>& items) const {
        if (revision) return revision();
        std::vector<std::string> ids; for (auto& p : items) ids.push_back(p.first);
        return revisionOf(ids);
    }
    static std::size_t pageStart(const std::string& req) { std::string c; return mini::getString(req, "cursor", c) ? static_cast<std::size_t>(std::max(0LL, std::atoll(c.c_str()))) : 0; }
    static std::size_t pageSize(const std::string& req) { long long n = 100; mini::getInt(req, "limit", n); return static_cast<std::size_t>(std::max(1LL, std::min<long long>(n, kMaxPage))); }

    std::string hello() const {
        const bool r = ready();
        std::vector<std::pair<std::string, std::string>> items; if (r) items = valid();
        std::string ops = "\"hello\",\"list\",\"get\"";
        const bool hasCollections = collections && !collections().empty();
        if (hasCollections) ops += ",\"collections\",\"collection\"";
        if (static_cast<int>(access_) >= 2) { if (load) ops += ",\"load\""; if (current) ops += ",\"current\""; if (entitled) ops += ",\"entitled\""; ops += ",\"events\""; }
        std::string out = "{\"ok\":true,\"connector\":1,\"plugin\":{\"name\":\"" + jsonEscape(name_) + "\",\"id\":\"" + jsonEscape(id_) + "\",\"kind\":\"" + jsonEscape(kind) + "\"";
        if (!version.empty()) out += ",\"version\":\"" + jsonEscape(version) + "\"";
        out += "},\"access\":\"" + std::string(accessName(access_)) + "\",\"ops\":[" + ops + "],\"revision\":\"" + (r ? jsonEscape(revisionFor(items)) : std::string("building")) +
               "\",\"idStability\":\"" + jsonEscape(idStability) + "\",\"limits\":{\"pageMax\":" + std::to_string(kMaxPage) + ",\"timeoutMs\":" + std::to_string(std::min(timeoutMs, 60000)) +
               ",\"loadsPerMinute\":" + std::to_string(throttle_.limit()) + (supportsAsyncLoad ? ",\"loadMode\":\"async\"" : "") + "}";
        out += r ? ",\"counts\":{\"presets\":" + std::to_string(items.size()) + "}" : ",\"building\":true";
        if (!policyJson.empty()) out += ",\"policy\":" + policyJson;
        return out + "}";
    }
    std::string list(const std::string& req) const {
        const auto items = valid(); const auto rev = revisionFor(items);
        std::string since; if (mini::getString(req, "since", since) && since == rev) return "{\"ok\":true,\"unchanged\":true,\"revision\":\"" + jsonEscape(rev) + "\"}";
        const auto start = pageStart(req), n = pageSize(req);
        std::string out = "{\"ok\":true,\"presets\":[";
        std::size_t end = start;
        for (std::size_t i = start; i < items.size() && i < start + n; ++i, ++end) { if (i > start) out += ","; out += items[i].second; }
        out += "],\"revision\":\"" + jsonEscape(rev) + "\"";
        if (end < items.size()) out += ",\"next\":\"" + std::to_string(end) + "\"";
        return out + "}";
    }
    std::string get(const std::string& req) const {
        std::string id; if (!mini::getString(req, "id", id)) return errorReply("bad_request", "get needs an id");
        for (auto& p : valid()) if (p.first == id) return "{\"ok\":true,\"preset\":" + p.second + "}";
        return errorReply("not_found", "no such id");
    }
    std::string collectionsReply(const std::string& req) const {
        const auto all = collections ? collections() : std::vector<std::string>{};
        const auto start = pageStart(req), n = pageSize(req);
        std::string out = "{\"ok\":true,\"collections\":["; std::size_t end = start;
        for (std::size_t i = start; i < all.size() && i < start + n; ++i, ++end) { if (i > start) out += ","; out += all[i]; }
        out += "]"; if (end < all.size()) out += ",\"next\":\"" + std::to_string(end) + "\"";
        return out + "}";
    }
    std::string collectionReply(const std::string& req) const {
        std::string id; if (!mini::getString(req, "id", id)) return errorReply("bad_request", "collection needs an id");
        for (auto& c : (collections ? collections() : std::vector<std::string>{})) {
            std::string cid; if (!mini::getString(c, "id", cid) || cid != id) continue;
            return "{\"ok\":true,\"collection\":" + c + "}";   // presetIds as the plugin gave them (paging of very large collections is the plugin's choice)
        }
        return errorReply("not_found", "no such collection");
    }
    std::string entitledReply(const std::string& req) const {
        if (!entitled) return errorReply("unsupported", "not implemented");
        auto asked = mini::getStringArray(req, "packs"); for (auto& s : mini::getStringArray(req, "ids")) asked.push_back(s);
        std::string out = "{\"ok\":true,\"entitled\":{"; bool first = true;
        for (auto& kv : entitled(asked)) { if (!first) out += ","; first = false; out += "\"" + jsonEscape(kv.first) + "\":\"" + jsonEscape(kv.second) + "\""; }
        return out + "}}";
    }
    static std::string outcomeReply(const LoadOutcome& r) {
        switch (r.kind) {
            case LoadOutcome::Ok: return "{\"ok\":true}";
            case LoadOutcome::NotFound: return errorReply("not_found", "no such id");
            case LoadOutcome::NotAvailable: return errorReply("not_available", "its content is not installed on this machine");
            case LoadOutcome::NotEntitled: return errorReply("not_entitled", "the user does not own this pack");
            case LoadOutcome::Busy: return errorReply("busy", "try again shortly");
            default: return errorReply("failed", r.message.empty() ? "load failed" : r.message);
        }
    }
    std::string loadReply(const std::string& req) {
        if (!load) return errorReply("unsupported", "this plugin does not load by id");
        std::string id; if (!mini::getString(req, "id", id)) return errorReply("bad_request", "load needs an id");
        bool found = false; for (auto& p : valid()) if (p.first == id) { found = true; break; }
        if (!found) return errorReply("not_found", "no such id");
        bool discard = false; mini::getBool(req, "discardEdits", discard);
        if (current) { const auto c = current(); if (c.modified && !discard) return errorReply("unsaved", "loading would discard unsaved edits; retry with discardEdits:true"); }
        if (const int wait = throttle_.tryUse()) return errorReply("rate_limited", "too many loads", wait);
        auto job = [this, id] { return load(id); };
        auto run = [this, job] { return runLoad ? runLoad(job) : job(); };
        bool async = false; mini::getBool(req, "async", async);
        if (supportsAsyncLoad && async) {
            std::lock_guard<std::mutex> g(jobsMutex_);
            asyncJobs_.emplace_back([this, id, run] {
                const auto r = run();
                if (r.kind == LoadOutcome::Ok) events.post("{\"type\":\"loaded\",\"id\":\"" + jsonEscape(id) + "\"}");
                else events.post("{\"type\":\"loadFailed\",\"id\":\"" + jsonEscape(id) + "\",\"error\":" + outcomeReply(r).substr(std::string("{\"ok\":false,\"error\":").size()));
            });
            return "{\"ok\":true,\"pending\":true}";
        }
        return outcomeReply(run());
    }

    static constexpr std::size_t kMaxIdBytes = 512;
    static constexpr long long kMaxPage = 500;
    std::string name_, id_;
    Access access_;
    mutable Throttle throttle_;
    std::mutex jobsMutex_;
    std::vector<std::thread> asyncJobs_;
};

}  // namespace presetbridge
