/*
 * Preset Bridge CLAP extension for Surge XT (spec section 4.5, https://github.com/brianpeat/PresetBridge).
 *
 * clap-juce-extensions gets a small generic hook (patches/clap-juce-extensions-extension-hook.patch): a processor can return its own
 * plugin extension. The extension's callbacks receive only the clap_plugin_t, so a registry maps that pointer back to this processor.
 * The same Handler that serves the Audio Unit property and the VST3 interface serves this extension. GPL-3.0-or-later.
 */
#include "SurgeSynthProcessor.h"

#if HAS_CLAP_JUCE_EXTENSIONS
#include <clap/clap.h>
#include <preset_bridge.h>
#include <cstring>
#include <map>
#include <mutex>

namespace
{
std::mutex registryMutex;
std::map<const clap_plugin_t *, presetbridge::Handler *> registry;

bool bridgeRequest(const clap_plugin_t *plugin, const char *json, uint32_t size, const clap_ostream_t *reply)
{
    if (!json || !reply)
        return false;
    std::lock_guard<std::mutex> g(registryMutex); // one request at a time, like the other doorways
    auto it = registry.find(plugin);
    if (it == registry.end())
        return false;
    const std::string out = size > presetbridge::kMaxRequestBytes
                                ? presetbridge::errorReply("bad_request", "request too large")
                                : it->second->handleRequest(std::string(json, size));
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
const clap_plugin_preset_bridge_t bridgeExtension = {bridgeRequest};
} // namespace

const void *SurgeSynthProcessor::pluginExtension(const char *id, const clap_plugin_t *plugin) noexcept
{
    if (std::strcmp(id, CLAP_EXT_PRESET_BRIDGE) != 0)
        return nullptr;
    std::lock_guard<std::mutex> g(registryMutex);
    registry[plugin] = static_cast<presetbridge::Handler *>(this);
    return &bridgeExtension;
}

void SurgeSynthProcessor::detachPresetBridgeCLAP()
{
    std::lock_guard<std::mutex> g(registryMutex);
    for (auto it = registry.begin(); it != registry.end();)
        it = it->second == static_cast<presetbridge::Handler *>(this) ? registry.erase(it) : std::next(it);
}
#endif
