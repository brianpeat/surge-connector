// SPDX-License-Identifier: Apache-2.0
// PresetRelay VST3 doorway (spec v0.2 draft, section 4.4).
// Needs the VST3 SDK headers (pluginterfaces/base/funknown.h and ibstream.h) on the include path and PresetRelay.h from reference/cpp.
// Header only; it does not need any SDK .cpp file, so it also links into builds that carry no SDK sources.
//
// How a plugin takes part: implement presetrelay::Handler (one function, JSON in, JSON out, as for every format), and when a host calls
// queryInterface on your IEditController (or IComponent) with IPresetRelay's IID, return a VST3Doorway:
//
//     // in your IEditController::queryInterface (or, with JUCE 8, in VST3ClientExtensions::queryIEditController):
//     return presetrelay::vst3::query(slot, targetIID, obj);
//
// `slot` is a std::shared_ptr<presetrelay::vst3::Slot> you own next to your Handler; call slot->clear() in your destructor so a host that
// keeps the interface alive longer than the plugin gets an error instead of a crash. Nothing here throws across PLUGIN_API.
#pragma once
#include <pluginterfaces/base/funknown.h>
#include <pluginterfaces/base/ibstream.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>

#include "PresetRelay.h"

namespace presetrelay {
namespace vst3 {

// The interface id of IPresetRelay.
inline const Steinberg::TUID kInterfaceId = INLINE_UID(0x7d3b9921, 0xa409477c, 0xbbb28829, 0x60c819d2);
// FUnknown's id, written out so no SDK .cpp file is needed to compare against it.
inline const Steinberg::TUID kFUnknownId = INLINE_UID(0x00000000, 0x00000000, 0xC0000000, 0x00000046);

class IPresetRelay : public Steinberg::FUnknown {
public:
    // requestJson: UTF-8 JSON request (spec 6), requestSize bytes (at most 64 KiB). The JSON reply is written to `reply`.
    // Returns kResultOk when a reply was written (it may itself be {"ok":false,...}); kResultFalse if the plugin is gone.
    virtual Steinberg::tresult PLUGIN_API request(const char* requestJson, Steinberg::int32 requestSize, Steinberg::IBStream* reply) = 0;
};

// Holds the handler for the doorway; clear() it when the plugin goes away. Requests run one at a time, like the AU property doorway, and no lock
// is held while a request runs, so clear() never waits behind a slow request for longer than its bound.
class Slot {
public:
    explicit Slot(Handler* h) : handler_(h) {}

    // Detaches the handler, then waits up to two seconds for requests that already started. Call it from the plugin's destructor.
    void clear() {
        std::unique_lock<std::mutex> g(m_);
        handler_ = nullptr;
        cv_.wait_for(g, std::chrono::seconds(2), [&] { return inFlight_ == 0; });
    }

    // `alive` is false when the handler is gone (the reply is then empty). A request that waits more than ten seconds behind another gets `busy`.
    std::string call(const std::string& req, bool& alive) {
        alive = false;
        {
            std::lock_guard<std::mutex> g(m_);
            if (handler_ == nullptr) return std::string();
            ++inFlight_;
        }
        struct Leave {
            Slot* s;
            ~Leave() {
                std::lock_guard<std::mutex> g(s->m_);
                --s->inFlight_;
                s->cv_.notify_all();
            }
        } leave{this};

        std::unique_lock<std::timed_mutex> turn(serial_, std::defer_lock);
        if (!turn.try_lock_for(std::chrono::seconds(10))) {
            alive = true;
            return errorReply("busy", "another request is still running");
        }
        Handler* h;
        {
            std::lock_guard<std::mutex> g(m_);
            h = handler_;
        }
        if (h == nullptr) return std::string();
        alive = true;
        try {
            return h->handleRequest(req);
        } catch (...) {
            return errorReply("failed", "internal error");
        }
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    std::timed_mutex serial_;
    Handler* handler_;
    int inFlight_ = 0;
};

class Doorway : public IPresetRelay {
public:
    explicit Doorway(std::shared_ptr<Slot> slot) : slot_(std::move(slot)) {}
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** obj) override {
        if (obj == nullptr) return Steinberg::kInvalidArgument;
        if (iid != nullptr && std::memcmp(iid, kInterfaceId, sizeof(Steinberg::TUID)) == 0 || std::memcmp(iid, kFUnknownId, sizeof(Steinberg::TUID)) == 0) {
            *obj = static_cast<IPresetRelay*>(this);
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
        try {
            std::string out;
            if (static_cast<std::size_t>(size) > kMaxRequestBytes) {
                out = errorReply("bad_request", "request too large");
            } else {
                bool alive = true;
                out = slot_->call(std::string(json, static_cast<std::size_t>(size)), alive);
                if (!alive) return Steinberg::kResultFalse;
                if (out.empty()) out = errorReply("failed", "the plugin returned no reply");
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
        } catch (...) {
            return Steinberg::kInternalError;
        }
    }
private:
    std::atomic<Steinberg::uint32> refs_{1};
    std::shared_ptr<Slot> slot_;
};

// Call from queryInterface: answers for IPresetRelay's id, otherwise returns kNoInterface (let your normal handling continue).
inline Steinberg::tresult query(const std::shared_ptr<Slot>& slot, const Steinberg::TUID iid, void** obj) {
    if (obj == nullptr || iid == nullptr) return Steinberg::kInvalidArgument;
    if (slot && std::memcmp(iid, kInterfaceId, sizeof(Steinberg::TUID)) == 0) {
        auto* doorway = new (std::nothrow) Doorway(slot);
        if (doorway == nullptr) return Steinberg::kOutOfMemory;
        *obj = static_cast<IPresetRelay*>(doorway);   // reference count 1, owned by the caller of queryInterface
        return Steinberg::kResultOk;
    }
    return Steinberg::kNoInterface;
}

}  // namespace vst3
}  // namespace presetrelay
