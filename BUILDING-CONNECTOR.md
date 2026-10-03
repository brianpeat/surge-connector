# Surge XT + Preset Connector (test fork)

Adds a Preset Connector handler (https://github.com/brianpeat/PresetConnector) to Surge XT's AU. A host can list Surge's own
patch library (factory, third party and user patches: name, category, author, favorite, file location), read the user's
favorites as a collection, and load or export a patch by id, through the plugin itself.

Changes: `src/surge-xt/SurgeConnector.cpp` (the handler; metadata from Surge's `PatchDB`, falls back to the in-memory patch
list), `SurgeSynthProcessor.h` (implements `presetconnector::Handler`), `src/surge-xt/CMakeLists.txt` (source + include path),
`connector/` (reference headers), and one patch to the bundled JUCE in `patches/juce-au-preset-connector-hook.patch`
(lets a plugin that implements the handler answer the AU property; it applies to Surge's JUCE 8 era; Odin 2's JUCE 6 needs the
other variant in the PresetConnector repo).

Surge-specific notes: Surge builds its patch index lazily, so the handler calls `initializePatchDb()` and waits for it (first
call can take a few seconds; the count can change while it is still indexing). `load` and `exportState` use the patch file
itself, which is Surge's own state format (`enqueuePatchForLoad` / `setStateInformation` take it unchanged). No `rights` are
reported (Surge doesn't say), which exercises the "unspecified rights" case.

Build (macOS, arm64):
```
git submodule update --init --recursive --depth 1
git -C libs/JUCE apply ../../patches/juce-au-preset-connector-hook.patch
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_SYSROOT=$(xcrun --show-sdk-path) -DSURGE_BUILD_CLAP=OFF -DSURGE_BUILD_TESTRUNNER=OFF -DSURGE_COPY_AFTER_BUILD=OFF
cmake --build build --target surge-xt_AU
```
