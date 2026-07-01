# Building Surge XT for WebAssembly

The Surge XT engine can be compiled as a WebAssembly *side module* which
exposes the same C API as the native CLAP plugin: a single exported
`clap_entry` symbol, driven through the [CLAP](https://github.com/free-audio/clap)
plugin ABI. This is intended for embedding the engine in a web page (or any
WASM runtime) behind a custom UI.

## What is and isn't in the module

Included:

* The full synthesis engine (oscillators, filters, effects, modulation),
  vectorized with WASM SIMD via Emscripten's SSE intrinsics support.
* The CLAP parameter, audio-ports, note-ports and state extensions, mirroring
  the native CLAP's behavior: normalized 0..1 parameter values plus eight
  macros, a stereo main out, scene A/B outs, a stereo sidechain in, and a
  note port accepting CLAP note/note-expression events and MIDI 1.0 bytes.
  Patches stream through the CLAP state extension in the regular `.fxp`-style
  raw patch format, so patches are interchangeable with the desktop plugins.

Excluded, to keep the binary small and browser-friendly:

* The GUI (JUCE does not target WASM) and all of its resources; there is no
  `clap.gui` extension. Bring your own UI.
* Factory content: patches, wavetables and samples are not bundled, and no
  content scan happens at startup. The engine boots on the init patch. If you
  need factory wavetables or patches, mount them into the Emscripten virtual
  filesystem (the engine looks at `/SurgeXTData` when no data path is
  supplied) and load patches via the state extension.
* Lua (so no Formula modulators or wavetable scripts) — LuaJIT cannot target
  WASM, and an interpreter would add considerable size.
* MTS-ESP support, OSC, and the MIDI learn persistence that requires a user
  data directory.

## Building

Install and activate the [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html)
(emsdk 3.1.50+ should work, tested with 6.x), then:

```sh
emcmake cmake -Bbuild-wasm -DCMAKE_BUILD_TYPE=Release
cmake --build build-wasm --target surge-wasm
```

The result lands in `build-wasm/surge_xt_products/surge-xt.clap.wasm`.

There is also a small browser demo (parameter list with search and pinning,
virtual keyboard, WebMIDI input) in `src/surge-wasm/demo`:

```sh
cmake --build build-wasm --target surge-wasm-demo
python3 -m http.server -d build-wasm/surge_xt_products/wasm-demo
# then open http://localhost:8000/
```

`SURGE_BUILD_WASM` turns on automatically under the Emscripten toolchain and
forces the JUCE-free configuration (`SURGE_SKIP_LUA`,
`SURGE_SKIP_ODDSOUND_MTS`, no JUCE wrappers).

## Using the module

The module is built with `-sSIDE_MODULE=2`, so it must be loaded from an
Emscripten *main module* (built with `-sMAIN_MODULE`), for example:

```js
const surge = await loadDynamicLibrary('surge-xt.clap.wasm', {global: true, nodelete: true});
```

or from C/C++ in the main module via `dlopen()`/`dlsym("clap_entry")`. From
there, the usual CLAP hosting sequence applies: `clap_entry.init()`, get the
`clap_plugin_factory`, create the plugin with id
`org.surge-synth-team.surge-xt`, `init()`, `activate(sampleRate, ...)`, and
call `process()` from your audio callback (an AudioWorklet, typically).

Notes:

* The main module decides the pthread configuration; the side module is built
  single-threaded. Patch loads through the state extension happen
  synchronously rather than on a loader thread.
* `process()` is happiest with buffer sizes that are a multiple of the engine
  block size (32 samples); anything else works but interleaves event timing
  at block granularity, like the native CLAP.
* C++ exceptions are compiled as native WASM exceptions
  (`-fwasm-exceptions`); the main module must be linked with the same flag.
