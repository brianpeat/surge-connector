// SPDX-License-Identifier: Apache-2.0
// Preset Bridge CLAP extension (spec v0.2 draft, section 4.5). Apache License 2.0. Plain C header; needs <clap/clap.h>.
//
// A plugin returns a pointer to a clap_plugin_preset_bridge_t from clap_plugin_t::get_extension("presetbridge/1"). `request` takes a UTF-8 JSON
// request (spec section 6, at most 64 KiB) and writes the JSON reply to `reply` (in as many writes as needed). It returns true when a reply
// was written, even if the reply is {"ok":false,...}. It runs on the main thread, one request at a time. A plugin that wants hosts to find it
// without loading it also lists the feature string "presetbridge" in its descriptor.
#pragma once
#include <clap/clap.h>

#ifdef __cplusplus
extern "C" {
#endif

static const char CLAP_EXT_PRESET_BRIDGE[] = "presetbridge/1";
static const char CLAP_PLUGIN_FEATURE_PRESET_BRIDGE[] = "presetbridge";

typedef struct clap_plugin_preset_bridge {
   bool (*request)(const clap_plugin_t *plugin, const char *request_json, uint32_t request_size, const clap_ostream_t *reply);
} clap_plugin_preset_bridge_t;

#ifdef __cplusplus
}
#endif
