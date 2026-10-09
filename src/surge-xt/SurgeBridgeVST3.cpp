/*
 * Preset Bridge VST3 doorway for Surge XT (spec section 4.4, https://github.com/brianpeat/PresetBridge).
 *
 * JUCE's VST3 wrapper lets a plugin answer extra interfaces on its edit controller through VST3ClientExtensions::queryIEditController,
 * so no JUCE change is needed. The same Handler that serves the Audio Unit property serves this interface. GPL-3.0-or-later.
 */
#include "SurgeSynthProcessor.h"
#include <PresetBridge_VST3.h>

int32_t SurgeSynthProcessor::queryIEditController(const Steinberg::TUID iid, void **obj)
{
    if (!presetBridgeVST3Slot)
        presetBridgeVST3Slot = std::make_shared<presetbridge::vst3::Slot>(static_cast<presetbridge::Handler *>(this));
    return presetbridge::vst3::query(std::static_pointer_cast<presetbridge::vst3::Slot>(presetBridgeVST3Slot), iid, obj);
}

void SurgeSynthProcessor::detachPresetBridgeVST3()
{
    if (presetBridgeVST3Slot)
        std::static_pointer_cast<presetbridge::vst3::Slot>(presetBridgeVST3Slot)->clear();
}
