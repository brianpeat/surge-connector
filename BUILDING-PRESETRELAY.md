# Surge XT + PresetRelay (test fork)

Adds a PresetRelay handler (https://github.com/brianpeat/PresetRelay) to Surge XT's AU. A host can list Surge's own
patch library (factory, third party and user patches: name, category, author, favorite, file location), read the user's
favorites as a collection, and load or export a patch by id, through the plugin itself.

Changes: `src/surge-xt/SurgeBridge.cpp` (the handler on `presetrelay::Catalog`; metadata from Surge's `PatchDB`), `src/surge-fx/SurgeFXBridge.cpp`
(effect presets), `presetrelay/` (reference headers plus `SurgeRelayShared.h`, which belongs to this fork), the processor headers (implement
`presetrelay::Handler`), the CMake files (sources and include path), and one patch to the bundled JUCE in `patches/juce-au-presetrelay-hook.patch`
(lets a plugin that implements the handler answer the AU property; it applies to Surge's JUCE 8 era; Odin 2's JUCE 6 needs the other variant in the PresetRelay repo).

Surge-specific notes: Surge builds its patch index lazily, so the catalog is built once on a background thread (`initializePatchDb()` and a wait for its
jobs); until it is done every op except `hello` answers `busy` and `hello` says `building:true`. `load` and `exportState` use the state inside the patch
file (the chunk after the .fxp header), which is what `setStateInformation` takes. No `rights` are reported (Surge doesn't say).

Build (macOS, arm64):
```
git submodule update --init --recursive --depth 1
git -C libs/JUCE apply ../../patches/juce-au-presetrelay-hook.patch
git -C libs/clap-juce-extensions apply ../../patches/clap-juce-extensions-extension-hook.patch   # only for the CLAP build
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_SYSROOT=$(xcrun --show-sdk-path) -DSURGE_BUILD_CLAP=ON -DSURGE_BUILD_TESTRUNNER=OFF -DSURGE_COPY_AFTER_BUILD=OFF
cmake --build build --target surge-xt_AU
```


## Formats
Audio Unit (property, needs the JUCE patch), VST3 (`SurgeBridgeVST3.cpp`, no JUCE patch) and CLAP (`SurgeBridgeCLAP.cpp`, needs the small clap-juce-extensions hook above, which is generic: a processor can return its own plugin extension). Surge XT Effects has AU and VST3 (`src/surge-fx/SurgeFXBridge*.cpp`), `kind: effect`. Check any build with `build/relaycheck` from the PresetRelay repo.

## State marker
Surge XT and Surge XT Effects (Audio Unit only) record which catalog sound is loaded in the state a host saves, so a host can label an
instance, a sound in a saved project or the sound behind a saved channel strip without loading the plugin (PresetRelay spec, Appendix E.4).
The JUCE patch adds the top-level key `PresetRelay` to the Audio Unit wrapper's saved state, filled from `Handler::stateMarkerJson()`.

- **Written:** every time the host saves state, `{"relay":1,"id":"<catalog id>","modified":<bool>,"revision":"<catalog revision>"}`, under 1 KiB.
  For Surge XT the id is the catalog entry whose file is the patch Surge last loaded (its own browser, a host preset change, a restored
  state or a relay load all update it) and `modified` is Surge's dirty flag. For Surge XT Effects the id is the one preset with the loaded
  preset's name and effect type, and `modified` compares the parameters with their values at the last preset load or save.
- **Never written:** no marker when the patch is not a catalog sound (a file from elsewhere, a patch that was never loaded from a file), the
  catalog is not built yet, a load is queued or running, the host changed the effect type, or two effect presets share the name. `modified`
  is left out for Surge XT Effects after a state restore, because edits made before the save are unknown. No paths, names or parameter values.
  VST3 and CLAP get nothing: their state is an opaque stream.
- **Restore:** unchanged. The wrapper ignores the key when it loads state and never hands a restored marker back; the next save writes it
  again from what is loaded.
- **Off:** set `c.stateMarker = false;` beside the other `c.` settings in `RelayState::init()` (`SurgeBridge.cpp`, `SurgeFXBridge.cpp`), or
  build without `patches/juce-au-presetrelay-hook.patch`, which also removes the Audio Unit property.
- **Verify:** install both Audio Units, then run `build/relaycheck` from the PresetRelay repo on each (the "state marker" group must not be skipped)
  and `make marker-test MARKER_ARGS="aumu SgXT VmbA"` and `"aufx SFXT VmbA"` there. The test calls `list` on a fresh instance at once; Surge builds
  its catalog on the first request and answers `busy` until it is done, so the test needs a wait for `list` (a hello and a retry) before it runs.
- **Limits:** the state-save path reads a copy of Surge's loaded-patch path kept by `SurgeSynthesizer::lastLoadedPatchHook`; Surge does not
  update that path on "Save Patch As", so the marker can keep naming the earlier patch until another one is loaded (read from the code, not tested live).
  A save during a patch load answers without a marker.
