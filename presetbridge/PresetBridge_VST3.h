// SPDX-License-Identifier: Apache-2.0
// Preset Bridge VST3 doorway (spec v0.2 draft, section 4.4). Apache License 2.0.
// Needs the VST3 SDK headers (pluginterfaces/base/funknown.h and ibstream.h) on the include path and PresetBridge.h from reference/cpp.
// Header only; it does not need any SDK .cpp file, so it also links into builds that carry no SDK sources.
//
// How a plugin takes part: implement presetbridge::Handler (one function, JSON in, JSON out, as for every format), and when a host calls
// queryInterface on your IEditController (or IComponent) with IPresetBridge's IID, return a VST3Doorway:
//
//     // in your IEditController::queryInterface (or, with JUCE 8, in VST3ClientExtensions::queryIEditController):
//     return presetbridge::vst3::query(slot, targetIID, obj);
//
// `slot` is a std::shared_ptr<presetbridge::vst3::Slot> you own next to your Handler; call slot->clear() in your destructor so a host that
// keeps the interface alive longer than the plugin gets an error instead of a crash.
#pragma once
#include <pluginterfaces/base/funknown.h>
#include <pluginterfaces/base/ibstream.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

#include "PresetBridge.h"

namespace presetbridge {
namespace vst3 {

// The interface id (random, assigned 2026-10-08; the spec's "IID to be assigned").
inline const Steinberg::TUID kInterfaceId = INLINE_UID(0x7d3b9921, 0xa409477c, 0xbbb28829, 0x60c819d2);
// FUnknown's own id, written out so no SDK .cpp file is needed to compare against it.
inline const Steinberg::TUID kFUnknownId = INLINE_UID(0x00000000, 0x00000000, 0xC0000000, 0x00000046);

class IPresetBridge : public Steinberg::FUnknown {
public:
    // requestJson: UTF-8 JSON request (spec 6), requestSize bytes (at most 64 KiB). The JSON reply is written to `reply`.
    // Returns kResultOk when a reply was written (it may itself be {"ok":false,...}); kResultFalse if the plugin is gone.
    virtual Steinberg::tresult PLUGIN_API request(const char* requestJson, Steinberg::int32 requestSize, Steinberg::IBStream* reply) = 0;
};

// Holds the handler for the doorway; clear() it when the plugin goes away.
class Slot {
public:
    explicit Slot(Handler* h) : handler_(h) {}
    void clear() { std::lock_guard<std::mutex> g(m_); handler_ = nullptr; }
    std::string call(const std::string& req, bool& alive) {
        std::lock_guard<std::mutex> g(m_);   // serialises requests: one at a time, like the AU property doorway
        alive = handler_ != nullptr;
        return alive ? handler_->handleRequest(req) : std::string();
    }
private:
    std::mutex m_;
    Handler* handler_;
};

class Doorway : public IPresetBridge {
public:
    explicit Doorway(std::shared_ptr<Slot> slot) : slot_(std::move(slot)) {}
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** obj) override {
        if (std::memcmp(iid, kInterfaceId, sizeof(Steinberg::TUID)) == 0 || std::memcmp(iid, kFUnknownId, sizeof(Steinberg::TUID)) == 0) {
            *obj = static_cast<IPresetBridge*>(this);
            addRef();
            return Steinberg::kResultOk;
        }
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }
    Steinberg::uint32 PLUGIN_API addRef() override { return ++refs_; }
    Steinberg::uint32 PLUGIN_API release() override {
        const auto n = --refs_;
        if (n == 0) delete this;
        return n;
    }
    Steinberg::tresult PLUGIN_API request(const char* json, Steinberg::int32 size, Steinberg::IBStream* reply) override {
        if (reply == nullptr || json == nullptr || size < 0) return Steinberg::kInvalidArgument;
        std::string out;
        if (static_cast<std::size_t>(size) > kMaxRequestBytes) {
            out = errorReply("bad_request", "request too large");
        } else {
            bool alive = true;
            out = slot_->call(std::string(json, static_cast<std::size_t>(size)), alive);
            if (!alive) return Steinberg::kResultFalse;
            if (out.size() > kMaxReplyBytes) out = errorReply("failed", "reply too large");
        }
        std::size_t done = 0;
        while (done < out.size()) {
            Steinberg::int32 wrote = 0;
            const auto chunk = static_cast<Steinberg::int32>(std::min<std::size_t>(out.size() - done, 1u << 20));
            if (reply->write(const_cast<char*>(out.data()) + done, chunk, &wrote) != Steinberg::kResultOk || wrote <= 0) return Steinberg::kResultFalse;
            done += static_cast<std::size_t>(wrote);
        }
        return Steinberg::kResultOk;
    }
private:
    std::atomic<Steinberg::uint32> refs_{1};
    std::shared_ptr<Slot> slot_;
};

// Call from queryInterface: answers for IPresetBridge's id, otherwise returns kNoInterface (let your normal handling continue).
inline Steinberg::tresult query(const std::shared_ptr<Slot>& slot, const Steinberg::TUID iid, void** obj) {
    if (slot && std::memcmp(iid, kInterfaceId, sizeof(Steinberg::TUID)) == 0) {
        *obj = static_cast<IPresetBridge*>(new Doorway(slot));   // reference count 1, owned by the caller of queryInterface
        return Steinberg::kResultOk;
    }
    return Steinberg::kNoInterface;
}

}  // namespace vst3
}  // namespace presetbridge
