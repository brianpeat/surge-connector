// Preset Connector reference header (spec v0.1 draft). MIT license. Plain C++17, no dependencies.
//
// To support the connector, a plugin implements ONE function: take a JSON request, return a JSON response.
// The same function serves every doorway (AU property, VST3 message, manifest generation, tests).
#pragma once
#include <string>

namespace presetconnector {

// AU v2 custom property ID ('PCon'). See spec section 4.3.
constexpr unsigned int kAUPropertyID = 1346457454u;

struct Handler {
    virtual ~Handler() = default;
    // request: {"op":"hello"|"list"|"get"|"load"|"exportState"|"current"|..., "v":1, ...}
    // return:  {"ok":true,...} or {"ok":false,"error":{"code":"unsupported","message":"..."}}
    // Called on a non-audio thread. Must be quick; `load` should hop to the plugin's own UI/message thread.
    virtual std::string handleRequest(const std::string& requestJson) = 0;
};

}  // namespace presetconnector
