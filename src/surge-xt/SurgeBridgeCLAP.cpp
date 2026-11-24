/*
 * PresetRelay CLAP extension for Surge XT (spec section 4.5, https://github.com/brianpeat/PresetRelay).
 *
 * clap-juce-extensions gets a small generic hook (patches/clap-juce-extensions-extension-hook.patch): a processor can return its own
 * plugin extension. The extension's callbacks receive only the clap_plugin_t, so a registry maps that pointer back to this processor.
 * The same Handler that serves the Audio Unit property and the VST3 interface serves this extension. GPL-3.0-or-later.
 */
#include "SurgeSynthProcessor.h"

#if HAS_CLAP_JUCE_EXTENSIONS
#include <clap/clap.h>
#include <preset_relay.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace
{
// One entry per plugin instance: requests to different instances never wait for each other. The map lock is held only to look an
// entry up, never while a request runs.
struct Instance
{
    std::atomic<presetrelay::Handler *> handler{nullptr};
    std::timed_mutex turn; // one request at a time per instance, like the other doorways
};
std::mutex mapMutex;
std::map<const clap_plugin_t *, std::shared_ptr<Instance>> registry;

bool bridgeRequest(const clap_plugin_t *plugin, const char *json, uint32_t size, const clap_ostream_t *reply)
{
    if (!json || !reply)
        return false;
    std::shared_ptr<Instance> inst;
    {
        std::lock_guard<std::mutex> g(mapMutex);
        const auto it = registry.find(plugin);
        if (it == registry.end())
            return false;
        inst = it->second;
    }
    std::string out;
    {
        std::unique_lock<std::timed_mutex> turn(inst->turn, std::defer_lock);
        if (size > presetrelay::kMaxRequestBytes)
            out = presetrelay::errorReply("bad_request", "request too large");
        else if (!turn.try_lock_for(std::chrono::seconds(10)))
            out = presetrelay::errorReply("busy", "another request is still running");
        else if (auto *h = inst->handler.load())
            out = h->handleRequest(std::string(json, size));
        else
            return false; // the plugin is gone
    }
    std::size_t done = 0;
    while (done < out.size())
    {
        const int64_t n = reply->write(reply, out.data() + done, out.size() - done);
        if (n <= 0)
            return false;
        done += static_cast<std::size_t>(n);
    }
    return true;
}
const clap_plugin_preset_relay_t bridgeExtension = {bridgeRequest};
} // namespace

const void *SurgeSynthProcessor::pluginExtension(const char *id, const clap_plugin_t *plugin) noexcept
{
    if (std::strcmp(id, CLAP_EXT_PRESET_RELAY) != 0)
        return nullptr;
    try
    {
        std::lock_guard<std::mutex> g(mapMutex);
        auto &slot = registry[plugin];
        if (!slot)
            slot = std::make_shared<Instance>();
        slot->handler = static_cast<presetrelay::Handler *>(this);
    }
    catch (...)
    {
        return nullptr;
    }
    return &bridgeExtension;
}

void SurgeSynthProcessor::detachPresetRelayCLAP()
{
    std::vector<std::shared_ptr<Instance>> mine;
    {
        std::lock_guard<std::mutex> g(mapMutex);
        for (auto it = registry.begin(); it != registry.end();)
        {
            if (it->second->handler.load() == static_cast<presetrelay::Handler *>(this))
            {
                mine.push_back(it->second);
                it = registry.erase(it);
            }
            else
                ++it;
        }
    }
    // No new request finds the instance now; wait (bounded) for one that is running, then cut the handler.
    for (auto &inst : mine)
    {
        std::unique_lock<std::timed_mutex> turn(inst->turn, std::defer_lock);
        turn.try_lock_for(std::chrono::seconds(2));
        inst->handler = nullptr;
    }
}
#endif
