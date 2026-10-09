/*
 * Preset Bridge handler for Surge XT Effects (spec https://github.com/brianpeat/PresetBridge): the effect reference.
 *
 * Lists the effect presets (.srgfx files, factory and user) of every effect type and loads one by id, switching the effect type first
 * when the preset belongs to another effect. Built on presetbridge::Catalog, so the protocol itself is not in this file. GPL-3.0-or-later.
 */
#include "SurgeFXProcessor.h"
#include <cstdlib>
#include <sstream>

namespace
{
std::string presetId(const Surge::Storage::FxUserPreset::Preset &p)
{
    std::string sub = p.subPath.generic_string();
    return std::string(p.isFactory ? "factory/" : "user/") + fx_type_names[p.type] + "/" + (sub.empty() ? "" : sub + "/") + p.name;
}

std::string recordJson(const Surge::Storage::FxUserPreset::Preset &p)
{
    using presetbridge::jsonEscape;
    std::ostringstream o;
    o << "{\"id\":\"" << jsonEscape(presetId(p)) << "\",\"name\":\"" << jsonEscape(p.name) << "\",\"origin\":\"" << (p.isFactory ? "factory" : "user")
      << "\",\"categories\":[[\"" << jsonEscape(fx_type_names[p.type]);
    std::string sub = p.subPath.generic_string();
    std::istringstream ss(sub);
    for (std::string part; std::getline(ss, part, '/');)
        if (!part.empty())
            o << "\",\"" << jsonEscape(part);
    o << "\"]],\"fields\":{\"effect\":[\"" << jsonEscape(fx_type_names[p.type]) << "\"]}}";
    return o.str();
}

struct MainThreadJob
{
    std::function<presetbridge::LoadOutcome()> job;
    presetbridge::LoadOutcome result;
};
} // namespace

void SurgefxAudioProcessor::buildPresetBridge()
{
    if (bridgeCatalog)
        return;
    bridgeCatalog = std::make_unique<presetbridge::Catalog>("Surge XT Effects", "org.surge-synth-team.surge-xt-fx", presetbridge::Access::audition, 30);
    auto &cat = *bridgeCatalog;
    cat.kind = "effect";
    cat.version = "";

    // The preset index is built once, on the first request (a scan of the factory and user .srgfx folders).
    bridgeScanner.doPresetRescan(storage.get(), true);
    for (auto &kv : bridgeScanner.getPresetsByType())
        for (auto &p : kv.second)
            if (p.type > 0 && p.type < n_fx_types && bridgePresets.emplace(presetId(p), p).second)
                bridgeRecords.push_back(recordJson(p));

    cat.records = [this] { return bridgeRecords; };
    cat.load = [this](const std::string &id) -> presetbridge::LoadOutcome {
        auto it = bridgePresets.find(id);
        if (it == bridgePresets.end())
            return presetbridge::LoadOutcome::notFound();
        const auto p = it->second;
        if (p.type != getEffectType())
            resetFxType(p.type, true);
        loadFxPreset(p);
        setCurrentPresetName(p.name);
        std::lock_guard<std::mutex> g(bridgeMutex);
        bridgeCurrentId = id;
        return presetbridge::LoadOutcome::ok();
    };
    cat.current = [this] {
        std::lock_guard<std::mutex> g(bridgeMutex);
        return presetbridge::CurrentSound{bridgeCurrentId, false};
    };
    // Changing the effect and writing its parameters belongs on the message thread; wait for it.
    cat.runLoad = [](const std::function<presetbridge::LoadOutcome()> &job) {
        MainThreadJob m{job, presetbridge::LoadOutcome::busy()};
        juce::MessageManager::getInstance()->callFunctionOnMessageThread(
            [](void *p) -> void * {
                auto *mj = static_cast<MainThreadJob *>(p);
                mj->result = mj->job();
                return nullptr;
            },
            &m);
        return m.result;
    };
}

std::string SurgefxAudioProcessor::handleRequest(const std::string &requestJson)
{
    static std::mutex buildMutex;
    {
        std::lock_guard<std::mutex> g(buildMutex);
        buildPresetBridge();
    }
    return bridgeCatalog->handleRequest(requestJson);
}
