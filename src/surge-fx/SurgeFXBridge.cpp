/*
 * PresetRelay handler for Surge XT Effects (spec https://github.com/brianpeat/PresetRelay): the effect reference.
 *
 * Lists the effect presets (.srgfx files, factory and user) of every effect type and loads one by id, switching the effect type first
 * when the preset belongs to another effect. Built on presetrelay::Catalog, so the protocol itself is not in this file. GPL-3.0-or-later.
 */
#include "SurgeFXProcessor.h"
#include <PresetRelay.h>
#include <PresetRelayCatalog.h>
#include <PresetRelay_VST3.h>
#include <SurgeRelayShared.h>
#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace
{
using presetrelay::LoadOutcome;
using FxPreset = Surge::Storage::FxUserPreset::Preset;

constexpr int kHopMs = 10000;       // wait for the message thread (at most 25 s, spec 3.2)
constexpr int kTypeSwitchMs = 400;  // how long the audio thread gets to start switching the effect type before the editor's own path is used
constexpr int kTypeFinishMs = 2000; // how long it gets to finish
constexpr int kRefreshSeconds = 60; // a hello this long after the last scan starts a new one in the background
constexpr int kRetrySeconds = 20;   // after a scan that failed

struct Entry
{
    std::string id;
    FxPreset preset;
};

// One finished scan. Readers hold a shared_ptr, so a new scan replaces it without touching a request in progress.
struct Catalogue
{
    std::vector<Entry> entries;
    std::unordered_map<std::string, std::size_t> byId;
    std::vector<std::string> records;
    std::string revision;
    bool partial{false};
};

std::string typeName(const FxPreset &p) { return fx_type_names[p.type]; }

std::string baseId(const FxPreset &p)
{
    const std::string sub = surgerelay::cleanText(p.subPath.generic_string(), 200);
    return std::string(p.isFactory ? "factory/" : "user/") + typeName(p) + "/" + (sub.empty() ? "" : sub + "/") +
           surgerelay::cleanText(p.name, 200);
}

std::string recordJson(const Entry &e)
{
    using presetrelay::jsonEscape;
    const FxPreset &p = e.preset;
    std::string o = "{\"id\":\"" + jsonEscape(e.id) + "\",\"name\":\"" + surgerelay::jsonText(p.name) +
                    "\",\"origin\":\"" + (p.isFactory ? "factory" : "user") + "\",\"categories\":[[\"" +
                    jsonEscape(typeName(p));
    const std::string sub = p.subPath.generic_string();
    std::size_t start = 0;
    while (start <= sub.size())
    {
        auto end = sub.find('/', start);
        if (end == std::string::npos)
            end = sub.size();
        const std::string part = surgerelay::cleanText(sub.substr(start, end - start), 128);
        if (!part.empty())
            o += "\",\"" + jsonEscape(part);
        start = end + 1;
    }
    return o + "\"]],\"fields\":{\"effect\":[\"" + jsonEscape(typeName(p)) + "\"]}}";
}

bool unreadableFolder(const fs::path &p)
{
    std::error_code ec;
    if (!fs::exists(p, ec))
        return false;
    fs::directory_iterator it(p, ec);
    return (bool)ec;
}

class RelayState : public std::enable_shared_from_this<RelayState>
{
  public:
    explicit RelayState(SurgefxAudioProcessor &p) : proc(p) {}

    void init()
    {
        catalog = std::make_unique<presetrelay::Catalog>("Surge XT Effects", "org.surge-synth-team.surge-xt-fx",
                                                         presetrelay::Access::audition, 30);
        const std::weak_ptr<RelayState> w = weak_from_this();
        auto &c = *catalog;
        c.kind = "effect";
        c.timeoutMs = 30000;
        c.isReady = [w] {
            auto s = w.lock();
            return s && s->ready.load();
        };
        c.records = [w] {
            auto s = w.lock();
            auto cat = s ? s->snapshot() : nullptr;
            return cat ? cat->records : std::vector<std::string>();
        };
        c.revision = [w] {
            auto s = w.lock();
            auto cat = s ? s->snapshot() : nullptr;
            return cat ? cat->revision : std::string("building");
        };
        c.load = [w](const std::string &id) {
            auto s = w.lock();
            return s ? s->load(id) : LoadOutcome::busy();
        };
        c.current = [w] {
            auto s = w.lock();
            return s ? s->currentSound() : presetrelay::CurrentSound{};
        };
        // Changing the effect and writing its parameters belongs on the message thread; `busy` when it does not answer in time.
        c.runLoad = [](const std::function<LoadOutcome()> &job) {
            return surgerelay::onMainThread(job, kHopMs);
        };
        // The preset name, effect type and parameter values at every preset load, save or state restore, kept here because the processor's
        // own copies are not safe to read from another thread; `current` and the state marker read this copy.
        notePreset(proc.getCurrentPresetName(), true); // the untouched instance: nothing to lose yet
        proc.presetNameHook = [w](const std::string &name, bool fresh) {
            if (auto s = w.lock())
                s->notePreset(name, fresh);
        };
    }

    void retire()
    {
        life.retire();
        if (catalog)
            catalog->shutdown();
        worker.stopAndJoin();
    }

    std::string handle(const std::string &req)
    {
        if (!life.isAlive())
            return presetrelay::errorReply("busy", "the plugin is closing");
        std::string op;
        const bool parsed = req.size() <= presetrelay::kMaxRequestBytes &&
                            presetrelay::mini::readText(req, "op", op) == presetrelay::mini::Presence::ok;
        scanIfNeeded(parsed && op == "hello");
        std::string out = catalog->handleRequest(req);
        if (parsed && op == "hello" && out.compare(0, 11, "{\"ok\":true,") == 0)
            if (auto c = snapshot(); c && c->partial)
                surgerelay::addToReply(out, ",\"partial\":true");
        return out;
    }

  private:
    std::shared_ptr<const Catalogue> snapshot()
    {
        std::lock_guard<std::mutex> g(m);
        return cat;
    }

    // ---- the catalogue: built on a background thread, never inside a request ----------------------------------------------
    void scanIfNeeded(bool isHello)
    {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> g(m);
        const bool first = !ready.load();
        const auto age = std::chrono::duration_cast<std::chrono::seconds>(now - lastScan).count();
        if ((first && (!everScanned || age >= kRetrySeconds)) || (!first && isHello && age >= kRefreshSeconds))
        {
            everScanned = true;
            lastScan = now;
            worker.start([this](const std::atomic<bool> &stop) { scan(stop); });
        }
    }

    void scan(const std::atomic<bool> &stop)
    {
        try
        {
            auto result = buildCatalogue(stop);
            if (!result || stop.load())
                return;
            std::lock_guard<std::mutex> g(m);
            cat = std::move(result); // published last: a scan that throws leaves the earlier state as it was
            lastScan = std::chrono::steady_clock::now();
            ready = true;
        }
        catch (...)
        {
            // ready stays as it was; the next request after kRetrySeconds tries again
        }
    }

    std::shared_ptr<Catalogue> buildCatalogue(const std::atomic<bool> &stop)
    {
        auto out = std::make_shared<Catalogue>();
        if (unreadableFolder(proc.storage->userFXPath) || unreadableFolder(proc.storage->datapath / "fx_presets"))
            out->partial = true;

        // A scanner of its own, so a rescan never changes the list a request is reading.
        Surge::Storage::FxUserPreset scanner;
        scanner.doPresetRescan(proc.storage.get(), true);
        if (stop.load())
            return nullptr;

        std::vector<Entry> all;
        std::unordered_map<std::string, int> count;
        for (const auto &kv : scanner.getPresetsByType())
            for (const auto &p : kv.second)
            {
                if (p.type <= 0 || p.type >= n_fx_types)
                    continue;
                Entry e;
                e.preset = p;
                e.id = baseId(p);
                ++count[e.id];
                all.push_back(std::move(e));
            }
        for (auto &e : all)
            if (count[e.id] > 1) // two presets with one id: both get a suffix from the full path
                e.id += "#" + surgerelay::shortHash(e.preset.file);
        std::sort(all.begin(), all.end(), [](const Entry &a, const Entry &b) { return a.id < b.id; });

        presetrelay::Fnv rev;
        rev.add("surge-fx-relay-1");
        for (auto &e : all)
        {
            out->byId.emplace(e.id, out->entries.size());
            const std::string rec = recordJson(e);
            out->records.push_back(rec);
            rev.add(rec);
            out->entries.push_back(std::move(e));
        }
        rev.add(out->partial ? "partial" : "complete");
        out->revision = rev.hex();
        return out;
    }

    // ---- modified: the parameters against what the last preset load, save (or a fresh instance) left ----------------------------
    std::vector<float> parameterValues()
    {
        std::vector<float> v;
        for (auto *p : proc.getParameters())
            v.push_back(p->getValue());
        return v;
    }

    // Any thread: the processor calls it after every preset name change.
    void notePreset(const std::string &name, bool fresh)
    {
        auto values = parameterValues();
        const int type = proc.getEffectType();
        std::lock_guard<std::mutex> g(snapM);
        snapName = name;
        snapType = type;
        snapKnown = fresh; // restored settings may have been edited before they were saved: `modified` is then unknown
        baseline = std::move(values);
    }

    juce::RangedAudioParameter *findTypeParameter()
    {
        for (auto *p : proc.getParameters())
            if (auto *r = dynamic_cast<juce::RangedAudioParameter *>(p); r && r->paramID == "fxtype")
                return r;
        return nullptr;
    }

    // ---- load: runs on the message thread (Catalog::runLoad) ----------------------------------------------------------------------
    LoadOutcome load(const std::string &id)
    {
        const auto c = snapshot();
        if (!c)
            return LoadOutcome::busy();
        const auto it = c->byId.find(id);
        if (it == c->byId.end())
            return LoadOutcome::notFound();
        const FxPreset p = c->entries[it->second].preset;

        auto use = life.use();
        if (!use)
            return LoadOutcome::busy();

        if (p.type != proc.getEffectType())
        {
            // Changing the effect deletes the running one. When the audio thread is running, ask it to do the change, as a host
            // does by automating the "FX Type" parameter (processBlock switches the effect on its own thread, and its last step
            // marks the preset name as no longer valid, which the marker below turns into a completion signal). When it does not
            // answer in time nothing is running, and the editor's own call is used.
            proc.setCurrentPresetName("\x01");
            if (auto *typeParam = findTypeParameter())
                typeParam->setValueNotifyingHost(typeParam->convertTo0to1((float)p.type));
            const auto began = std::chrono::steady_clock::now();
            for (;;)
            {
                const bool typeDone = proc.getEffectType() == p.type;
                if (typeDone && proc.getCurrentPresetName().empty())
                    break;
                const auto waited = std::chrono::steady_clock::now() - began;
                if (!typeDone && waited > std::chrono::milliseconds(kTypeSwitchMs))
                {
                    proc.resetFxType(p.type, true);
                    break;
                }
                if (waited > std::chrono::milliseconds(kTypeFinishMs))
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        if (proc.getEffectType() != p.type)
            return LoadOutcome::busy(); // changed under us
        proc.loadFxPreset(p);
        proc.setCurrentPresetName(p.name);

        {
            std::lock_guard<std::mutex> g(m);
            loadedId = id;
            reportedId = id;
        }
        catalog->events.post("{\"type\":\"presetChanged\",\"id\":\"" + presetrelay::jsonEscape(id) + "\"}");
        return LoadOutcome::ok();
    }

    // ---- current: the relay's own request runs on the message thread; the state marker calls it on the thread that saves state --------
    // Never waits on a lock a load holds. The id is the one catalog preset with the remembered name and effect type, nothing else: a
    // type changed by the host, a catalog not scanned yet or two presets with that name give no id.
    presetrelay::CurrentSound currentSound()
    {
        presetrelay::CurrentSound out;
        auto use = life.tryUse();
        if (!use)
            return out;
        std::string name;
        int type = 0;
        {
            std::lock_guard<std::mutex> g(snapM);
            name = snapName;
            type = snapType;
            out.modifiedKnown = snapKnown;
            out.modified = !snapKnown || differsFromBaseline();
        }
        std::string id;
        const auto c = snapshot();
        if (c && !name.empty() && name[0] != '\x01' && !proc.fxTypeChangedByHost.load())
        {
            std::string preferred;
            {
                std::lock_guard<std::mutex> g(m);
                preferred = loadedId;
            }
            const auto pit = c->byId.find(preferred);
            if (pit != c->byId.end() && c->entries[pit->second].preset.type == type &&
                c->entries[pit->second].preset.name == name)
                id = preferred;
            else
            {
                int matches = 0;
                for (const auto &e : c->entries)
                    if (e.preset.type == type && e.preset.name == name && ++matches == 1)
                        id = e.id;
                if (matches != 1)
                    id.clear();
            }
        }
        out.id = id;
        // Events are posted from the message thread only, not from the thread that saves state.
        auto *mm = juce::MessageManager::getInstanceWithoutCreating();
        bool changed = false;
        if (mm && mm->isThisTheMessageThread())
        {
            std::lock_guard<std::mutex> g(m);
            if (id != reportedId)
            {
                reportedId = id;
                changed = !id.empty();
            }
        }
        if (changed)
            catalog->events.post("{\"type\":\"presetChanged\",\"id\":\"" + presetrelay::jsonEscape(id) + "\"}");
        return out;
    }

    // Caller holds snapM.
    bool differsFromBaseline()
    {
        const auto now = parameterValues();
        if (now.size() != baseline.size())
            return true;
        for (std::size_t i = 0; i < now.size(); ++i)
            if (std::fabs(now[i] - baseline[i]) > 1e-4f)
                return true;
        return false;
    }

  public:
    // The state marker (spec E.4) for the Audio Unit's saved state, "" for none. Any thread, no wait.
    std::string stateMarker() { return life.isAlive() && catalog ? catalog->stateMarkerJson() : std::string(); }
    // The loaded sound's name (spec E.5) from the same snapshot as the marker, "" for none. Any thread, no wait.
    std::string soundName() { return life.isAlive() && catalog ? catalog->currentSoundName() : std::string(); }

  private:
    SurgefxAudioProcessor &proc;
    surgerelay::Liveness life;
    std::unique_ptr<presetrelay::Catalog> catalog;
    std::mutex m; // guards cat, the scan times and the ids
    std::shared_ptr<const Catalogue> cat;
    std::atomic<bool> ready{false};
    bool everScanned{false};
    std::chrono::steady_clock::time_point lastScan{};
    std::string loadedId, reportedId;
    std::mutex snapM; // guards the four members below: the preset hook may run on any thread
    std::string snapName;
    int snapType{0};
    bool snapKnown{true};
    std::vector<float> baseline;
    surgerelay::Worker worker;   // last member: destroyed (and joined) first
};
} // namespace

void SurgefxAudioProcessor::initPresetRelay()
{
    auto s = std::make_shared<RelayState>(*this);
    s->init();
    presetRelayState = s;
    // Created here, not on the first query, so two threads asking at once cannot both create it.
    presetRelayVST3Slot = std::make_shared<presetrelay::vst3::Slot>(static_cast<presetrelay::Handler *>(this));
}

void SurgefxAudioProcessor::detachPresetRelay()
{
    if (auto s = std::static_pointer_cast<RelayState>(presetRelayState))
        s->retire();
}

std::string SurgefxAudioProcessor::stateMarkerJson()
{
    try
    {
        auto s = std::static_pointer_cast<RelayState>(presetRelayState);
        return s ? s->stateMarker() : std::string();
    }
    catch (...)
    {
        return std::string();
    }
}

std::string SurgefxAudioProcessor::currentSoundName()
{
    try
    {
        auto s = std::static_pointer_cast<RelayState>(presetRelayState);
        return s ? s->soundName() : std::string();
    }
    catch (...)
    {
        return std::string();
    }
}

std::string SurgefxAudioProcessor::handleRequest(const std::string &requestJson)
{
    auto s = std::static_pointer_cast<RelayState>(presetRelayState);
    if (!s)
        return presetrelay::errorReply("busy", "the plugin is not ready");
    try
    {
        return s->handle(requestJson);
    }
    catch (...)
    {
        return presetrelay::errorReply("failed", "internal error");
    }
}
