/*
 * PresetRelay handler for Surge XT (spec https://github.com/brianpeat/PresetRelay).
 *
 * Lets a host list Surge's own patch library (factory, third party and user patches, with category, author and the user's
 * favorites) and load one by id, through the plugin itself. Built on presetrelay::Catalog, so the protocol is not in this file;
 * what is here is how Surge's patches become records and how a patch is applied. Metadata comes from Surge's patch database
 * (PatchDB), the same index its patch browser uses. Surge XT is GPL-3.0-or-later; so is this file.
 */
#include "SurgeSynthProcessor.h"
#include "SurgeStorage.h"
#include "PatchDB.h"
#include "PatchFileHeaderStructs.h"
#include "version.h"
#include <PresetRelay.h>
#include <PresetRelayCatalog.h>
#include <PresetRelay_VST3.h>
#include <SurgeRelayShared.h>
#include <cstring>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

namespace
{
using presetrelay::LoadOutcome;

constexpr int kHopMs = 10000;                               // wait for the message thread (at most 25 s, spec 3.2)
constexpr int kApplyMs = 3000;                              // wait for the audio thread to take an enqueued patch
constexpr std::uintmax_t kMaxPatchBytes = 8u * 1024 * 1024; // a hostile file cannot make us allocate more; base64 of it fits the reply cap
constexpr std::size_t kMaxEntries = 100000;
constexpr int kRefreshSeconds = 60; // a hello this long after the last scan starts a new one in the background
constexpr int kRetrySeconds = 20;   // after a scan that did not finish
const char *const kListenerKey = "presetrelay";

// Debug and test only: PRESETRELAY_ACCESS=catalog|audition|insight|open and PRESETRELAY_LOADS_PER_MINUTE=N (1 to 600; anything else is
// ignored, so a bad value never switches the limit off). Surge XT is open source, so by default it shares everything without a limit.
struct Config
{
    presetrelay::Access access{presetrelay::Access::open};
    int loadsPerMinute{0};
    Config()
    {
        if (const char *a = std::getenv("PRESETRELAY_ACCESS"))
            access = presetrelay::accessFromName(a, presetrelay::Access::open);
        loadsPerMinute =
            (access == presetrelay::Access::audition || access == presetrelay::Access::insight) ? 10 : 0;
        loadsPerMinute = surgerelay::envPositive("PRESETRELAY_LOADS_PER_MINUTE", loadsPerMinute, 600);
    }
};

struct Entry
{
    std::string id, name, category, author;
    std::string file;    // absolute path: used internally to load, never sent to a host
    std::string relFile; // path under its content folder, e.g. "factory/Basses/Attacky.fxp": what a host may see
    std::string origin;
    bool favorite{false};
    std::int64_t mtime{0};
};

// One finished scan. Readers hold a shared_ptr, so a new scan replaces it without touching a request in progress.
struct Catalogue
{
    std::vector<Entry> entries;
    std::unordered_map<std::string, std::size_t> byId, byFile;
    std::vector<std::string> records;
    std::string favorites; // the "favorites" collection, as JSON
    std::string revision;
    bool partial{false}; // some content could not be read
};

struct FallbackPatch
{
    std::string name, category, file;
    bool favorite{false};
};

// True when `rel` (a path under a content folder) has no ".." component; a name like "..x.fxp" is fine.
bool staysInside(const std::string &rel)
{
    std::size_t start = 0;
    while (start <= rel.size())
    {
        auto end = rel.find('/', start);
        if (end == std::string::npos)
            end = rel.size();
        if (rel.compare(start, end - start, "..") == 0)
            return false;
        start = end + 1;
    }
    return !rel.empty();
}

std::string withoutSlash(std::string s)
{
    while (s.size() > 1 && s.back() == '/')
        s.pop_back();
    return s;
}

bool unreadableFolder(const fs::path &p)
{
    std::error_code ec;
    if (!fs::exists(p, ec))
        return false; // not created yet (for example no user patches): empty, not unreadable
    fs::directory_iterator it(p, ec);
    return (bool)ec;
}

std::uint32_t be32(const void *v)
{
    const auto *p = static_cast<const std::uint8_t *>(v);
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | std::uint32_t(p[3]);
}

// Reads a patch file and returns the state Surge's setStateInformation takes (the chunk after the .fxp header), checking everything
// before any Surge code sees it: a regular file, 1 byte to 8 MiB, the .fxp header, a chunk that fits and starts with Surge's own tag.
LoadOutcome readPatchState(const std::string &file, std::vector<char> &state)
{
    std::error_code ec;
    const fs::path path = string_to_path(file);
    const auto st = fs::status(path, ec);
    if (ec || !fs::exists(st))
        return LoadOutcome::notAvailable();
    if (!fs::is_regular_file(st))
        return LoadOutcome::failed("the patch is not a regular file");
    const auto size = fs::file_size(path, ec);
    if (ec || size == 0 || size > kMaxPatchBytes)
        return LoadOutcome::failed("the patch file has an unusable size");
    std::vector<char> bytes(static_cast<std::size_t>(size));
    {
        std::ifstream in(path, std::ios::binary);
        if (!in.read(bytes.data(), static_cast<std::streamsize>(bytes.size())))
            return LoadOutcome::failed("could not read the patch file");
    }
    const std::size_t head = sizeof(sst::io::fxChunkSetCustom);
    if (bytes.size() <= head + 4)
        return LoadOutcome::failed("not a Surge patch");
    sst::io::fxChunkSetCustom fxp;
    std::memcpy(&fxp, bytes.data(), head);
    if (be32(&fxp.chunkMagic) != 'CcnK' || be32(&fxp.fxMagic) != 'FPCh' || be32(&fxp.fxID) != 'cjs3')
        return LoadOutcome::failed("not a Surge patch");
    const std::uint32_t chunk = be32(&fxp.chunkSize);
    if (chunk < 4 || chunk > bytes.size() - head || std::memcmp(bytes.data() + head, "sub3", 4) != 0)
        return LoadOutcome::failed("not a Surge patch");
    state.assign(bytes.begin() + static_cast<std::ptrdiff_t>(head),
                 bytes.begin() + static_cast<std::ptrdiff_t>(head + chunk));
    return LoadOutcome::ok();
}

class RelayState : public std::enable_shared_from_this<RelayState>
{
  public:
    explicit RelayState(SurgeSynthProcessor &p) : proc(p) {}

    // Called once, right after construction.
    void init()
    {
        const Config cfg;
        catalog = std::make_unique<presetrelay::Catalog>("Surge XT", "org.surge-synth-team.surge-xt",
                                                         cfg.access, cfg.loadsPerMinute);
        const std::weak_ptr<RelayState> w = weak_from_this();
        auto &c = *catalog;
        c.kind = "instrument";
        c.version = Surge::Build::FullVersionStr;
        c.timeoutMs = 30000;
        c.supportsAsyncLoad = true;
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
        c.collections = [w] {
            auto s = w.lock();
            auto cat = s ? s->snapshot() : nullptr;
            return cat ? std::vector<std::string>{cat->favorites} : std::vector<std::string>();
        };
        c.load = [w](const std::string &id) {
            auto s = w.lock();
            return s ? s->load(id) : LoadOutcome::busy();
        };
        c.current = [w] {
            auto s = w.lock();
            return s ? s->currentSound() : presetrelay::CurrentSound{};
        };
        // The patch is applied on the message thread (as setStateInformation is); `busy` when it does not answer in time.
        c.runLoad = [](const std::function<LoadOutcome()> &job) {
            return surgerelay::onMainThread(job, kHopMs);
        };

        // A patch chosen in Surge's own browser: post presetChanged. Registered here, before any load thread exists, so the listener
        // table is not changed while Surge iterates it.
        proc.surge->addPatchLoadedListener(kListenerKey, [w](const fs::path &p) {
            if (auto s = w.lock())
                s->noteLoadedFile(path_to_string(p));
        });

        // Surge's own copy of the loaded patch's path is not safe to read from another thread, so every change of it (browser, host
        // preset change, restored state, relay load) is copied here; `current` and the state marker read the copy.
        setLastFile(proc.surge->storage.lastLoadedPatch.u8string());
        proc.surge->lastLoadedPatchHook = [w](const fs::path &p) {
            if (auto s = w.lock())
                s->setLastFile(p.u8string());
        };
    }

    // First thing in the processor's destructor: after this nothing touches the processor or the synth.
    void retire()
    {
        life.retire();
        if (proc.surge)
            proc.surge->deletePatchLoadedListener(kListenerKey);
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

        if (parsed && op == "exportState")
            return exportState(req);
        std::string out = catalog->handleRequest(req);
        if (parsed && op == "hello" && out.compare(0, 11, "{\"ok\":true,") == 0)
            decorateHello(out);
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
        // A patch added or a favorite toggled shows up on the next hello after a minute; requests keep using the old catalogue
        // until the new one is swapped in.
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
            cat = std::move(result);
            lastScan = std::chrono::steady_clock::now();
            ready = true;
        }
        catch (...)
        {
        }
    }

    std::shared_ptr<Catalogue> buildCatalogue(const std::atomic<bool> &stop)
    {
        SurgeStorage &storage = proc.surge->storage;
        auto out = std::make_shared<Catalogue>();
        const std::pair<const char *, fs::path> rootPaths[] = {
            {"factory", storage.datapath / "patches_factory"},
            {"thirdparty", storage.datapath / "patches_3rdparty"},
            {"user", storage.userPatchesPath}};
        std::vector<std::pair<std::string, std::string>> roots;
        for (const auto &[origin, path] : rootPaths)
        {
            roots.emplace_back(origin, withoutSlash(fs::path(path).generic_u8string()));
            if (unreadableFolder(path))
                out->partial = true;
        }

        struct Row
        {
            std::string name, category, author, file;
            bool favorite{false};
            std::int64_t mtime{0};
        };
        std::vector<Row> rows;

        // 1. Surge's patch database, which has the author. It is built lazily: start it (the same call the patch browser makes) and let
        //    it finish in this thread; a first run takes a while, and until then every request but hello answers `busy`.
        if (storage.patchDB)
        {
            storage.initializePatchDb();
            const auto began = std::chrono::steady_clock::now();
            while (storage.patchDB->waitForJobsOutstandingComplete(100) > 0)
            {
                if (stop.load())
                    return nullptr;
                if (std::chrono::steady_clock::now() - began > std::chrono::minutes(10))
                    return nullptr; // the index is not finishing: try again later rather than publish half of it
            }
            std::unordered_set<std::string> favorites;
            for (const auto &f : storage.patchDB->readUserFavorites())
                favorites.insert(f);
            const auto stamps = storage.patchDB->readAllPatchPathsWithIdAndModTime();
            for (const auto &rec : storage.patchDB->rawQueryForNameLike(""))
            {
                Row r{rec.name, rec.cat, rec.author, rec.file, favorites.count(rec.file) > 0, 0};
                const auto it = stamps.find(rec.file);
                if (it != stamps.end())
                    r.mtime = it->second.second;
                rows.push_back(std::move(r));
            }
        }

        // 2. Fallback: the in-memory patch list (no author). It belongs to the message thread, so it is copied there; if that thread
        //    does not answer, the catalogue is built from what the database gave and marked partial.
        if (rows.empty() && !stop.load())
        {
            auto copy = std::make_shared<std::vector<FallbackPatch>>();
            const std::weak_ptr<RelayState> w = weak_from_this();
            const auto r = surgerelay::onMainThread(
                [w, copy]() -> LoadOutcome {
                    auto s = w.lock();
                    if (!s)
                        return LoadOutcome::busy();
                    auto use = s->life.use();
                    if (!use)
                        return LoadOutcome::busy();
                    SurgeStorage &st = s->proc.surge->storage;
                    for (const auto &patch : st.patch_list)
                    {
                        FallbackPatch f;
                        f.name = patch.name;
                        if (patch.category >= 0 && patch.category < (int)st.patch_category.size())
                            f.category = st.patch_category[(size_t)patch.category].name;
                        f.file = patch.path.u8string();
                        f.favorite = patch.isFavorite;
                        copy->push_back(std::move(f));
                    }
                    return LoadOutcome::ok();
                },
                2000);
            if (r.kind == LoadOutcome::Ok)
                for (auto &f : *copy)
                    rows.push_back(Row{f.name, f.category, std::string(), f.file, f.favorite, 0});
            else
                out->partial = true;
        }

        // 3. Records. The id is "<origin>/<path under the content folder without .fxp>", decided by string prefix and path
        //    components; a file outside every content folder keeps only its file name.
        std::unordered_map<std::string, int> idCount;
        for (auto &row : rows)
        {
            if (out->entries.size() >= kMaxEntries)
            {
                out->partial = true;
                break;
            }
            Entry e;
            e.name = surgerelay::cleanText(row.name);
            if (e.name.empty())
                e.name = "(unnamed)";
            e.category = row.category;
            e.author = surgerelay::cleanText(row.author);
            e.favorite = row.favorite;
            e.mtime = row.mtime;
            e.file = row.file;
            const std::string generic = fs::path(string_to_path(row.file)).generic_u8string();
            std::string rel;
            e.origin = "user";
            for (const auto &[origin, root] : roots)
            {
                if (generic.size() > root.size() + 1 && generic.compare(0, root.size(), root) == 0 &&
                    generic[root.size()] == '/' && staysInside(generic.substr(root.size() + 1)))
                {
                    e.origin = origin;
                    rel = generic.substr(root.size() + 1);
                    break;
                }
            }
            if (rel.empty())
            {
                const auto slash = generic.rfind('/');
                rel = slash == std::string::npos ? generic : generic.substr(slash + 1);
            }
            rel = surgerelay::cleanText(rel, 400);
            e.relFile = e.origin + "/" + rel;
            std::string stem = rel;
            if (stem.size() > 4 && stem.compare(stem.size() - 4, 4, ".fxp") == 0)
                stem.resize(stem.size() - 4);
            e.id = e.origin + "/" + stem;
            ++idCount[e.id];
            out->entries.push_back(std::move(e));
        }
        for (auto &e : out->entries)
            if (idCount[e.id] > 1) // two sounds with one id (a name cut short, a clash): both get a suffix from the full path
                e.id += "#" + surgerelay::shortHash(e.file);
        presetrelay::Fnv rev;
        rev.add("surge-xt-relay-1");
        std::string favIds;
        for (std::size_t i = 0; i < out->entries.size(); ++i)
        {
            const Entry &e = out->entries[i];
            out->byId.emplace(e.id, i);
            out->byFile.emplace(e.file, i);
            const std::string rec = recordJson(e);
            out->records.push_back(rec);
            rev.add(rec);
            rev.add(std::to_string(e.mtime));
            if (e.favorite)
                favIds += (favIds.empty() ? "\"" : ",\"") + presetrelay::jsonEscape(e.id) + "\"";
        }
        out->favorites =
            "{\"id\":\"favorites\",\"name\":\"Favorites\",\"kind\":\"user\",\"presetIds\":[" + favIds + "]}";
        rev.add(out->favorites);
        rev.add(out->partial ? "partial" : "complete");
        out->revision = rev.hex();
        return out;
    }

    static std::string recordJson(const Entry &e)
    {
        using presetrelay::jsonEscape;
        std::string r = "{\"id\":\"" + jsonEscape(e.id) + "\",\"name\":\"" + jsonEscape(e.name) + "\",\"origin\":\"" +
                        (e.origin == "user" ? "user" : "factory") + "\",\"available\":true";
        if (!e.author.empty())
            r += ",\"creator\":\"" + jsonEscape(e.author) + "\"";
        std::string path;
        std::size_t start = 0;
        while (start <= e.category.size())
        {
            auto end = e.category.find('/', start);
            if (end == std::string::npos)
                end = e.category.size();
            const std::string part = surgerelay::cleanText(e.category.substr(start, end - start), 128);
            if (!part.empty())
                path += (path.empty() ? "\"" : ",\"") + jsonEscape(part) + "\"";
            start = end + 1;
        }
        if (!path.empty())
            r += ",\"categories\":[[" + path + "]]";
        r += std::string(",\"favorite\":") + (e.favorite ? "true" : "false");
        // Informational only, and never an absolute path (it would carry the user's account name): the path under the content folder.
        r += ",\"location\":{\"kind\":\"file\",\"uri\":\"" + jsonEscape(e.relFile) + "\"}}";
        // Deliberately no "rights": Surge does not say, which exercises the "unspecified" case in the spec.
        return r;
    }

    // ---- hello additions the helper has no hook for ------------------------------------------------------------------------
    void decorateHello(std::string &out)
    {
        surgerelay::addToPluginObject(
            out, ",\"identity\":{\"au\":{\"type\":\"aumu\",\"subtype\":\"SgXT\",\"manufacturer\":\"VmbA\"},"
                 "\"clap\":{\"id\":\"org.surge-synth-team.surge-xt\"}}");
        if (presetrelay::atLeast(catalog->access(), presetrelay::Access::open))
            surgerelay::addOp(out, "exportState");
        if (auto c = snapshot(); c && c->partial)
            surgerelay::addToReply(out, ",\"partial\":true");
    }

    // ---- exportState (level 4): the patch's own state, without switching the loaded sound ------------------------------------------
    std::string exportState(const std::string &req)
    {
        if (!presetrelay::atLeast(catalog->access(), presetrelay::Access::open))
            return presetrelay::errorReply("unsupported", "exportState is not offered at this access level");
        const auto c = snapshot();
        if (!ready.load() || !c)
            return presetrelay::errorReply("busy", "the catalog is still being built");
        std::string id;
        if (presetrelay::mini::readText(req, "id", id) != presetrelay::mini::Presence::ok || id.empty() ||
            id.size() > 512 || id.find('\0') != std::string::npos)
            return presetrelay::errorReply("bad_request", "exportState needs an id of 1 to 512 bytes");
        const auto it = c->byId.find(id);
        if (it == c->byId.end())
            return presetrelay::errorReply("not_found", "no such preset");
        std::vector<char> state;
        const LoadOutcome r = readPatchState(c->entries[it->second].file, state);
        if (r.kind == LoadOutcome::NotAvailable)
            return presetrelay::errorReply("not_available", "the patch file is not there");
        if (r.kind != LoadOutcome::Ok)
            return presetrelay::errorReply("failed", r.message);
        // The state is the chunk Surge's getStateInformation produces for that patch, so setStateInformation takes it as is.
        return "{\"ok\":true,\"stateKind\":\"juce.setStateInformation/surge-fxp\",\"state\":\"" +
               juce::Base64::toBase64(state.data(), state.size()).toStdString() + "\"}";
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
        std::vector<char> state;
        if (auto r = readPatchState(c->entries[it->second].file, state); r.kind != LoadOutcome::Ok)
            return r;

        {
            auto use = life.use();
            if (!use || !proc.surge)
                return LoadOutcome::busy();
            // The same two calls setStateInformation makes: the patch is applied here when the audio engine is idle, otherwise the
            // audio thread takes it from the queue at its next block.
            proc.surge->enqueuePatchForLoad(state.data(), (int)state.size());
            proc.surge->processAudioThreadOpsWhenAudioEngineUnavailable();
        }

        // `load` answers ok only once the patch is in effect: wait for the audio thread to take it, and take it back (busy) when it
        // does not, so a refused load leaves nothing queued.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kApplyMs);
        for (;;)
        {
            {
                auto use = life.use();
                if (!use)
                    return LoadOutcome::busy();
                SurgeSynthesizer &s = *proc.surge;
                if (s.rawLoadEnqueued.load() && std::chrono::steady_clock::now() > deadline)
                {
                    std::lock_guard<std::mutex> g(s.rawLoadQueueMutex);
                    if (s.rawLoadEnqueued.load())
                    {
                        s.rawLoadEnqueued = false;
                        s.enqueuedLoadData.reset();
                        return LoadOutcome::busy();
                    }
                }
                if (!s.rawLoadEnqueued.load())
                {
                    std::lock_guard<std::mutex> g(s.rawLoadQueueMutex); // the load itself runs under this lock
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        {
            auto use = life.use();
            if (!use)
                return LoadOutcome::busy();
            // A patch file has no record of where it came from, so the loaded path is set here as Surge's own browser sets it; the
            // saved state then names this patch and a restore resolves to it again.
            proc.surge->storage.lastLoadedPatch = string_to_path(c->entries[it->second].file);
            proc.surge->noteLastLoadedPatch();
            std::lock_guard<std::mutex> g(m);
            reportedId = id;
        }
        catalog->events.post("{\"type\":\"presetChanged\",\"id\":\"" + presetrelay::jsonEscape(id) + "\"}");
        return LoadOutcome::ok();
    }

    // ---- current: the relay's own request runs on the message thread; the state marker calls it on the thread that saves state --------
    // Never waits on a lock a load holds. The id is the catalog sound whose file is the loaded patch's file, nothing else: a patch from
    // elsewhere, a catalog not scanned yet, or a load still in flight gives no id.
    presetrelay::CurrentSound currentSound()
    {
        presetrelay::CurrentSound out;
        auto use = life.tryUse();
        if (!use || !proc.surge || !settled())
            return out;
        out.modified = proc.surge->storage.getPatch().isDirty.load();
        std::string file;
        {
            std::lock_guard<std::mutex> g(fileM);
            file = lastFile;
        }
        std::string id;
        if (const auto c = snapshot(); c && !file.empty())
        {
            const auto it = c->byFile.find(file);
            if (it != c->byFile.end())
                id = c->entries[it->second].id;
        }
        out.id = id;
        // Events are posted from the message thread only, not from the thread that saves state.
        if (auto *mm = juce::MessageManager::getInstanceWithoutCreating(); mm && mm->isThisTheMessageThread())
            announce(id);
        return out;
    }

    // True when no patch is queued or being applied, so lastFile and isDirty describe the patch that is in effect.
    bool settled()
    {
        SurgeSynthesizer &s = *proc.surge;
        if (s.rawLoadEnqueued.load() || s.has_patchid_file.load() || s.patchid_queue.load() >= 0)
            return false;
        std::unique_lock<std::mutex> g(s.rawLoadQueueMutex, std::try_to_lock);
        return g.owns_lock();
    }

    void setLastFile(std::string f)
    {
        std::lock_guard<std::mutex> g(fileM);
        lastFile = std::move(f);
    }

  public:
    // The state marker (spec E.4) for the Audio Unit's saved state, "" for none. Any thread, no wait.
    std::string stateMarker() { return life.isAlive() && catalog ? catalog->stateMarkerJson() : std::string(); }
    // The loaded sound's name (spec E.5) from the same snapshot as the marker, "" for none. Any thread, no wait.
    std::string soundName() { return life.isAlive() && catalog ? catalog->currentSoundName() : std::string(); }

  private:
    void announce(const std::string &id)
    {
        {
            std::lock_guard<std::mutex> g(m);
            if (id == reportedId)
                return;
            reportedId = id;
        }
        if (!id.empty())
            catalog->events.post("{\"type\":\"presetChanged\",\"id\":\"" + presetrelay::jsonEscape(id) + "\"}");
    }

    // From Surge's patch-load thread: a patch picked in the plugin's own browser.
    void noteLoadedFile(const std::string &file)
    {
        const auto c = snapshot();
        if (!c)
            return;
        const auto it = c->byFile.find(file);
        if (it != c->byFile.end())
            announce(c->entries[it->second].id);
    }

    SurgeSynthProcessor &proc;
    surgerelay::Liveness life;
    std::unique_ptr<presetrelay::Catalog> catalog;
    std::mutex m; // guards cat, the scan times and reportedId
    std::mutex fileM; // guards lastFile alone: the patch-load hook may run on the audio thread
    std::string lastFile; // path of the patch Surge last loaded, as its patch-load hook reported it
    std::shared_ptr<const Catalogue> cat;
    std::atomic<bool> ready{false};
    bool everScanned{false};
    std::chrono::steady_clock::time_point lastScan{};
    std::string reportedId;
    surgerelay::Worker worker; // last member: destroyed (and joined) first
};
} // namespace

void SurgeSynthProcessor::initPresetRelay()
{
    if (!surge)
        return;
    auto s = std::make_shared<RelayState>(*this);
    s->init();
    presetRelayState = s;
    // Created here, not on the first query, so two threads asking at once cannot both create it.
    presetRelayVST3Slot = std::make_shared<presetrelay::vst3::Slot>(static_cast<presetrelay::Handler *>(this));
}

void SurgeSynthProcessor::detachPresetRelay()
{
    if (auto s = std::static_pointer_cast<RelayState>(presetRelayState))
        s->retire();
}

std::string SurgeSynthProcessor::stateMarkerJson()
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

std::string SurgeSynthProcessor::currentSoundName()
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

std::string SurgeSynthProcessor::handleRequest(const std::string &requestJson)
{
    auto s = std::static_pointer_cast<RelayState>(presetRelayState);
    if (!s)
        return presetrelay::errorReply("busy", "synth not ready");
    try
    {
        return s->handle(requestJson);
    }
    catch (...)
    {
        return presetrelay::errorReply("failed", "internal error");
    }
}
