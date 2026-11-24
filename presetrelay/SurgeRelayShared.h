// Small helpers shared by the Surge XT and Surge XT Effects PresetRelay handlers. Owned by this fork (not part of the PresetRelay
// repository). GPL-3.0-or-later like the rest of Surge.
#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <juce_events/juce_events.h>

#include <PresetRelay.h>
#include <PresetRelayCatalog.h>

namespace surgerelay
{
// Text from a file or a plugin database is untrusted: invalid UTF-8 becomes U+FFFD and the result is cut at `cap` bytes on a character
// boundary, so a hostile name cannot make a record invalid or huge.
inline std::string cleanText(const std::string &in, std::size_t cap = 256)
{
    std::string out;
    out.reserve(std::min(in.size(), cap));
    for (std::size_t i = 0; i < in.size();)
    {
        const std::size_t n = presetrelay::utf8SequenceLength(in, i);
        const std::size_t take = n == 0 ? 3 : n;
        if (out.size() + take > cap)
            break;
        if (n == 0)
        {
            out += "\xEF\xBF\xBD";
            ++i;
        }
        else
        {
            out.append(in, i, n);
            i += n;
        }
    }
    return out;
}

// A whole number in [1, maxValue] from an environment variable; anything else (unset, zero, negative, text) gives `fallback`.
inline int envPositive(const char *name, int fallback, int maxValue)
{
    const char *s = std::getenv(name);
    if (!s || !*s)
        return fallback;
    char *end = nullptr;
    const long v = std::strtol(s, &end, 10);
    return (end && *end == 0 && v >= 1 && v <= maxValue) ? static_cast<int>(v) : fallback;
}

// Short hex hash of a string, to keep ids unique when two sounds would get the same one.
inline std::string shortHash(const std::string &s)
{
    return presetrelay::revisionOf({s}).substr(0, 8);
}

// Tells work that runs later (a main-thread job, a callback) whether the plugin it touches still exists. The plugin calls retire() first
// thing in its destructor; use() fails afterwards, and retire() waits (bounded) for a job that is inside use() right now.
class Liveness
{
  public:
    class Use
    {
      public:
        Use() = default;
        explicit Use(std::unique_lock<std::timed_mutex> l) : lock(std::move(l)) {}
        explicit operator bool() const { return lock.owns_lock(); }

      private:
        std::unique_lock<std::timed_mutex> lock;
    };

    Use use()
    {
        std::unique_lock<std::timed_mutex> l(m, std::defer_lock);
        if (!alive.load() || !l.try_lock_for(std::chrono::seconds(2)))
            return Use();
        if (!alive.load())
            return Use();
        return Use(std::move(l));
    }
    bool isAlive() const { return alive.load(); }
    void retire()
    {
        alive = false;
        std::unique_lock<std::timed_mutex> l(m, std::defer_lock);
        l.try_lock_for(std::chrono::seconds(2));
    }

  private:
    std::atomic<bool> alive{true};
    std::timed_mutex m;
};

// One background thread at a time. stopAndJoin() must run before anything the thread reads is destroyed; the thread checks `stop`
// between its short steps, so the join is quick.
class Worker
{
  public:
    ~Worker() { stopAndJoin(); }
    bool start(std::function<void(const std::atomic<bool> &stop)> fn)
    {
        std::lock_guard<std::mutex> g(m);
        if (running.load())
            return false;
        if (t.joinable())
            t.join();
        stop = false;
        running = true;
        try
        {
            t = std::thread([this, fn] {
                try
                {
                    fn(stop);
                }
                catch (...)
                {
                }
                running = false;
            });
        }
        catch (...)
        {
            running = false;
            return false;
        }
        return true;
    }
    void stopAndJoin()
    {
        std::lock_guard<std::mutex> g(m);
        stop = true;
        if (t.joinable())
            t.join();
    }
    bool isRunning() const { return running.load(); }

  private:
    std::mutex m;
    std::thread t;
    std::atomic<bool> stop{false}, running{false};
};

// Sleeps up to `ms`, in short steps, and returns false as soon as `stop` is set.
inline bool pause(const std::atomic<bool> &stop, int ms)
{
    for (int left = ms; left > 0 && !stop.load(); left -= 20)
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min(20, left)));
    return !stop.load();
}

// A JSON string literal's body for an id or name (escaped, cleaned).
inline std::string jsonText(const std::string &s, std::size_t cap = 256)
{
    return presetrelay::jsonEscape(cleanText(s, cap));
}

// Runs `job` on the message thread: directly when the caller already is that thread (a host asks from it, and posting to it would wait
// for itself), otherwise through the helper's hop-with-timeout, which answers `busy` when the thread does not get to it in time.
inline presetrelay::LoadOutcome onMainThread(const std::function<presetrelay::LoadOutcome()> &job, int timeoutMs)
{
    if (juce::MessageManager::existsAndIsCurrentThread())
    {
        try
        {
            return job();
        }
        catch (...)
        {
            return presetrelay::LoadOutcome::failed("the callback threw");
        }
    }
    if (juce::MessageManager::getInstanceWithoutCreating() == nullptr)
        return presetrelay::LoadOutcome::busy();
    return presetrelay::hopWithTimeout([](std::function<void()> f) { juce::MessageManager::callAsync(std::move(f)); }, job,
                                       timeoutMs);
}

// Adds `extra` (e.g. ,"identity":{...}) just before the "}" that closes the "plugin" object of a hello reply built by the Catalog helper.
// The plugin object holds only the name, id, kind and version this fork sets (no braces), so the first "}" after it closes it.
inline void addToPluginObject(std::string &helloReply, const std::string &extra)
{
    const auto at = helloReply.find("\"plugin\":{");
    if (at == std::string::npos)
        return;
    const auto close = helloReply.find('}', at);
    if (close != std::string::npos)
        helloReply.insert(close, extra);
}

// Adds `extra` (e.g. ,"partial":true) as a top-level member at the end of a reply that ends with "}".
inline void addToReply(std::string &reply, const std::string &extra)
{
    if (!reply.empty() && reply.back() == '}')
        reply.insert(reply.size() - 1, extra);
}

// Adds one op name to the "ops" array of a hello reply.
inline void addOp(std::string &helloReply, const std::string &op)
{
    const auto at = helloReply.find("\"ops\":[");
    if (at == std::string::npos)
        return;
    const auto close = helloReply.find(']', at);
    if (close != std::string::npos)
        helloReply.insert(close, ",\"" + op + "\"");
}
} // namespace surgerelay
