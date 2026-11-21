/*
 * PresetRelay VST3 doorway for Surge XT (spec section 4.4, https://github.com/brianpeat/PresetRelay).
 *
 * JUCE's VST3 wrapper lets a plugin answer extra interfaces on its edit controller through VST3ClientExtensions::queryIEditController,
 * so no JUCE change is needed. The same Handler that serves the Audio Unit property serves this interface. GPL-3.0-or-later.
 */
#include "SurgeSynthProcessor.h"
#include <PresetRelay_VST3.h>

int32_t SurgeSynthProcessor::queryIEditController(const Steinberg::TUID iid, void **obj)
{
    if (!presetRelayVST3Slot)
        presetRelayVST3Slot = std::make_shared<presetrelay::vst3::Slot>(static_cast<presetrelay::Handler *>(this));
    return presetrelay::vst3::query(std::static_pointer_cast<presetrelay::vst3::Slot>(presetRelayVST3Slot), iid, obj);
}

void SurgeSynthProcessor::detachPresetRelayVST3()
{
    if (presetRelayVST3Slot)
        std::static_pointer_cast<presetrelay::vst3::Slot>(presetRelayVST3Slot)->clear();
}
