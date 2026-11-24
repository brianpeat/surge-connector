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
Audio Unit (property, needs the JUCE patch), VST3 (`SurgeBridgeVST3.cpp`, no JUCE patch) and CLAP (`SurgeBridgeCLAP.cpp`, needs the small clap-juce-extensions hook above, which is generic: a processor can return its own plugin extension). Surge XT Effects has AU and VST3 (`src/surge-fx/SurgeFXBridge*.cpp`), `kind: effect`. Check any build with `tools/relaycheck/relaycheck` from the PresetRelay repo.
