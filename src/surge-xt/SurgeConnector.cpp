/*
 * Preset Connector handler for Surge XT (spec v0.1 draft, https://github.com/brianpeat/PresetConnector).
 *
 * Lets a host list Surge's own patch library (factory, third party and user patches, with category, author and the user's
 * favorites) and load one by id, through the plugin itself. Metadata comes from Surge's patch database (PatchDB), the same
 * index its patch browser uses. Surge XT is GPL-3.0-or-later; so is this file.
 */
#include "SurgeSynthProcessor.h"
#include "SurgeStorage.h"
#include "PatchDB.h"
#include "version.h"
#include <PresetConnector.h>
#include <fstream>
#include <unordered_set>

namespace
{
using juce::DynamicObject;
using juce::var;

const char *kRevisionPrefix = "surge-xt-patchdb-";

var errorResponse(const char *code, const juce::String &message)
{
    auto *err = new DynamicObject();
    err->setProperty("code", code);
    err->setProperty("message", message);
    auto *r = new DynamicObject();
    r->setProperty("ok", false);
    r->setProperty("error", var(err));
    return var(r);
}

std::string toJson(const var &v) { return juce::JSON::toString(v, true).toStdString(); }

struct Entry
{
    std::string id;
    std::string name;
    std::string category;
    std::string author;
    std::string file;
    std::string origin;
    bool favorite{false};
};

std::string relativeTo(const fs::path &file, const fs::path &base)
{
    std::error_code ec;
    auto rel = fs::relative(file, base, ec);
    if (ec || rel.empty() || rel.u8string().rfind("..", 0) == 0)
        return {};
    return rel.generic_u8string();
}
} // namespace

namespace
{
std::vector<Entry> readCatalog(SurgeStorage &storage)
{
    std::vector<Entry> out;
    const std::pair<const char *, fs::path> roots[] = {{"factory", storage.datapath / "patches_factory"},
                                                       {"thirdparty", storage.datapath / "patches_3rdparty"},
                                                       {"user", storage.userPatchesPath}};
    auto classify = [&](Entry &e, const std::string &file) {
        e.file = file;
        e.origin = "user";
        std::string rel;
        for (const auto &[origin, root] : roots)
        {
            rel = relativeTo(fs::path(file), root);
            if (!rel.empty())
            {
                e.origin = origin;
                break;
            }
        }
        if (rel.empty())
            rel = fs::path(file).filename().generic_u8string();
        if (rel.size() > 4 && rel.compare(rel.size() - 4, 4, ".fxp") == 0)
            rel.resize(rel.size() - 4);
        e.id = e.origin + "/" + rel;
    };

    // 1. Preferred: Surge's patch database (the index its patch browser uses), which has the author. It is built lazily,
    //    so start it ourselves and wait for a first-run build; later launches reuse the database already on disk.
    if (storage.patchDB)
    {
        storage.initializePatchDb();
        storage.patchDB->waitForJobsOutstandingComplete(60000);

        std::unordered_set<std::string> favorites;
        for (const auto &f : storage.patchDB->readUserFavorites())
            favorites.insert(f);
        for (const auto &rec : storage.patchDB->rawQueryForNameLike(""))
        {
            Entry e;
            e.name = rec.name;
            e.category = rec.cat;
            e.author = rec.author;
            classify(e, rec.file);
            e.favorite = favorites.count(rec.file) > 0;
            out.push_back(std::move(e));
        }
    }
    if (!out.empty())
        return out;

    // 2. Fallback: the in-memory patch list (always there, no author).
    for (const auto &patch : storage.patch_list)
    {
        Entry e;
        e.name = patch.name;
        if (patch.category >= 0 && patch.category < (int)storage.patch_category.size())
            e.category = storage.patch_category[(size_t)patch.category].name;
        classify(e, patch.path.u8string());
        e.favorite = patch.isFavorite;
        out.push_back(std::move(e));
    }
    return out;
}

var record(const Entry &e)
{
    auto *r = new DynamicObject();
    r->setProperty("id", juce::String(juce::CharPointer_UTF8(e.id.c_str())));
    r->setProperty("name", juce::String(juce::CharPointer_UTF8(e.name.c_str())));
    r->setProperty("origin", e.origin == "user" ? "user" : "factory");
    r->setProperty("available", true);
    if (!e.author.empty())
        r->setProperty("creator", juce::String(juce::CharPointer_UTF8(e.author.c_str())));
    if (!e.category.empty())
    {
        juce::Array<var> path;
        for (const auto &part : juce::StringArray::fromTokens(juce::String(juce::CharPointer_UTF8(e.category.c_str())), "/", ""))
            path.add(part);
        juce::Array<var> cats;
        cats.add(var(path));
        r->setProperty("categories", var(cats));
    }
    r->setProperty("favorite", e.favorite);
    auto *loc = new DynamicObject();
    loc->setProperty("kind", "file");
    loc->setProperty("uri", juce::String(juce::CharPointer_UTF8(e.file.c_str())));
    r->setProperty("location", var(loc));
    // Deliberately no "rights": Surge does not say, which exercises the "unspecified" case in the spec.
    return var(r);
}
} // namespace

std::string SurgeSynthProcessor::handleRequest(const std::string &requestJson)
{
    const var req = juce::JSON::parse(juce::String(requestJson));
    if (!req.isObject())
        return toJson(errorResponse("bad_request", "request is not a JSON object"));
    if (!surge)
        return toJson(errorResponse("busy", "synth not ready"));
    const juce::String op = req["op"].toString();
    auto *res = new DynamicObject();
    var out(res);
    res->setProperty("ok", true);

    if (op == "hello")
    {
        const auto catalog = readCatalog(surge->storage);
        res->setProperty("connector", 1);
        auto *plugin = new DynamicObject();
        plugin->setProperty("name", "Surge XT");
        plugin->setProperty("id", "org.surge-synth-team.surge-xt");
        plugin->setProperty("version", juce::String(Surge::Build::FullVersionStr));
        res->setProperty("plugin", var(plugin));
        juce::Array<var> ops;
        for (auto *o : {"hello", "list", "get", "load", "exportState", "collections", "collection"})
            ops.add(juce::String(o));
        res->setProperty("ops", var(ops));
        res->setProperty("revision", juce::String(kRevisionPrefix) + juce::String((int)catalog.size()));
        auto *counts = new DynamicObject();
        counts->setProperty("presets", (int)catalog.size());
        res->setProperty("counts", var(counts));
        auto *limits = new DynamicObject();
        limits->setProperty("pageMax", 500);
        res->setProperty("limits", var(limits));
    }
    else if (op == "list")
    {
        const auto catalog = readCatalog(surge->storage);
        const int cursor = juce::jmax(0, (int)req["cursor"]);
        const int limit = juce::jlimit(1, 500, req.hasProperty("limit") ? (int)req["limit"] : 500);
        juce::Array<var> presets;
        int i = cursor;
        for (; i < (int)catalog.size() && presets.size() < limit; ++i)
            presets.add(record(catalog[(size_t)i]));
        res->setProperty("presets", var(presets));
        if (i < (int)catalog.size())
            res->setProperty("next", juce::String(i));
        res->setProperty("revision", juce::String(kRevisionPrefix) + juce::String((int)catalog.size()));
    }
    else if (op == "get" || op == "load" || op == "exportState")
    {
        const auto catalog = readCatalog(surge->storage);
        const juce::String wanted = req["id"].toString();
        const Entry *found = nullptr;
        for (const auto &e : catalog)
            if (juce::String(juce::CharPointer_UTF8(e.id.c_str())) == wanted)
            {
                found = &e;
                break;
            }
        if (!found)
            return toJson(errorResponse("not_found", "no such preset"));
        if (op == "get")
        {
            res->setProperty("preset", record(*found));
        }
        else
        {
            // A Surge patch file IS Surge's state format: setStateInformation / loadRaw take it as is.
            std::ifstream in(fs::path(found->file), std::ios::binary);
            std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (bytes.empty())
                return toJson(errorResponse("failed", "could not read the patch file"));
            if (op == "load")
            {
                surge->enqueuePatchForLoad(bytes.data(), (int)bytes.size());   // safe from any thread
                surge->processAudioThreadOpsWhenAudioEngineUnavailable();
                m_connector_current_id = found->id;
            }
            else
            {
                res->setProperty("stateKind", "juce.setStateInformation/surge-fxp");
                res->setProperty("state", juce::Base64::toBase64(bytes.data(), bytes.size()));
            }
        }
    }
    else if (op == "collections" || op == "collection")
    {
        // The only real collection Surge keeps outside the file layout: the user's favorites.
        const auto catalog = readCatalog(surge->storage);
        juce::Array<var> ids;
        for (const auto &e : catalog)
            if (e.favorite)
                ids.add(juce::String(juce::CharPointer_UTF8(e.id.c_str())));
        auto *c = new DynamicObject();
        c->setProperty("id", "favorites");
        c->setProperty("name", "Favorites");
        c->setProperty("kind", "user");
        if (op == "collections")
        {
            juce::Array<var> list;
            list.add(var(c));
            res->setProperty("collections", var(list));
        }
        else
        {
            if (req["id"].toString() != "favorites")
                return toJson(errorResponse("not_found", "no such collection"));
            c->setProperty("presetIds", var(ids));
            res->setProperty("collection", var(c));
        }
    }
    else if (op == "current")
    {
        if (!m_connector_current_id.empty())
            res->setProperty("id", juce::String(juce::CharPointer_UTF8(m_connector_current_id.c_str())));
    }
    else
    {
        return toJson(errorResponse("unsupported", "unknown op: " + op));
    }
    return toJson(out);
}
