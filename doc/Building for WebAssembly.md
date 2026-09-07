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

## Two toolchains

There are two ways to build the module, and they produce modules that are *not*
interchangeable:

* **Emscripten** (`-sSIDE_MODULE=2`, described below) produces a module that
  imports its entire C and C++ runtime — several hundred symbols — from an
  Emscripten main module. Use this when the host is itself an Emscripten
  program.

* **clang/wasi** (`cmake/wasm-clang-toolchain.cmake`) produces a self-contained
  module which links libc, libc++ and compiler-rt statically and imports only
  the handful of symbols that describe where the loader placed it. Use this when
  the host is not an Emscripten program — a Rust `wasm32-unknown-unknown`
  application, say.

### The clang/wasi build

The toolchain comes from the flake in the root of this repository. It rebuilds
the wasi runtimes (wasi-libc, compiler-rt, libc++/libc++abi and libunwind)
because the ones nixpkgs ships cannot be used here, for four reasons:

* **Position independence.** nixpkgs' runtimes are non-PIC, and linking them
  into a shared library fails with `relocation R_WASM_MEMORY_ADDR_SLEB cannot
  be used against symbol '.L.str'; recompile with -fPIC`. The one assembly file
  in wasi-libc, the thread entry point `wasi_thread_start`, has the same
  problem and is patched to address `__thread_list_lock` relative to
  `__memory_base`.
* **Hermetic runtimes.** libc++ marks its ABI surface visibility-default, and
  in a shared library lld emits an import for each such reference even when
  the definition is in the archive right next to it, leaving it for a dynamic
  loader to bind. Without `LIBCXX_HERMETIC_STATIC_LIBRARY` (and libunwind's
  `LIBUNWIND_HIDE_SYMBOLS`) a trivial module that touches `std::string`
  imports ~130 C++ symbols; with them, none.
* **C++ exceptions.** nixpkgs builds the wasm libc++ without them, and its
  wasm cc-wrapper bakes `-fno-exceptions` into every compile, which is why
  flipping the CMake options alone still fails with "cannot use 'try' with
  exceptions disabled". The flake compiles every runtime with `-fexceptions
  -fwasm-exceptions`, which is also what turns on the wasm-specific code in
  libc++abi (`__gxx_personality_wasm0`) and libunwind (`Unwind-wasm.c`), both
  guarded on the `__WASM_EXCEPTIONS__` macro that flag defines. nixpkgs does
  not build libunwind for wasm at all, so the flake does, and the toolchain
  links it in. Note that clang 21 emits the *legacy* exception-handling
  encoding (`try`/`catch`/`delegate`), not `exnref`.
* **Threads.** Surge uses `std::thread` and `std::mutex` outright, and libc++
  refuses to provide `<mutex>` when built without threads, so wasi-libc is
  built in its posix thread model and everything is compiled `-pthread`. lld
  only permits a shared memory when every object in the link was built with
  the atomics and bulk-memory features, which is why `-pthread` (like
  `-fwasm-exceptions` and `-fPIC`) is set toolchain-wide.

The runtimes also get `std::filesystem`, which nixpkgs leaves out of its wasm
libc++ but wasi-libc supports; Surge's filesystem layer prefers it, and the
bundled `ghc::filesystem` fallback does not know wasi at all.

```sh
nix develop
cmake -B build-wasm -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/wasm-clang-toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-wasm --target surge-wasm
```

The result lands in `build-wasm/surge_xt_products/surge-xt.clap.wasm`, as with
Emscripten. The Surge-side accommodations are small and live under
`src/platform/wasm`: a simde-backed `xmmintrin.h` so pffft keeps its SIMD path
(plain clang has no SSE headers for wasm), a wasi implementation of the
sst-plugininfra path lookups (no `dladdr()` or `$HOME` here), and a header
force-included into sqlite that declares the record-locking and ownership
calls wasi-libc leaves out.

#### What the module asks of its host

Loading goes: instantiate against the host's memory and table; call
`__wasm_apply_data_relocs`; give the main thread its thread-local storage
(allocate `__tls_size` bytes at `__tls_align`, call `__wasm_init_tls` with
the address, then `__wasi_init_tp`); then `__wasm_call_ctors`. The last step
matters: the CLAP plugin descriptor is built by a static constructor, and lld
only keeps the constructor runner because the toolchain exports it. The
exported `clap_entry` global holds the entry's offset *within the module's
data*, not its address: add `__memory_base` to it, as Emscripten's loader
does.

The module imports only what describes where it was placed, plus the WASI
system calls it happens to use:

* `env.memory` -- a **shared** memory, since the module is built `-pthread`;
  `env.__indirect_function_table`; `env.__memory_base`, `env.__table_base`
  (immutable `i32`) and `env.__stack_pointer` (mutable `i32`), as for any
  side module.
* `env.__cpp_exception` -- the C++ exception tag, a
  `WebAssembly.Tag({parameters: ["i32"]})`; a side module imports it so that
  every module in the program throws with the same tag.
* `GOT.mem.__heap_base` and `GOT.mem.__heap_end` -- **mutable** `i32` globals
  holding the bounds of the region the module's `malloc` may use. lld only
  synthesises these for executables, so the host chooses the heap. The same
  goes for `GOT.mem.__stack_low` / `__stack_high` (the main thread's stack
  region, which wasi-libc records in the main thread's pthread structure) and
  `GOT.mem.__global_base` / `__data_end` (where the module's data was placed
  and where it ends), all of which the host knows.
* `wasi_snapshot_preview1.*` -- the handful of system calls the code path
  reaches: `environ_sizes_get`/`environ_get` (must succeed, if only with zero
  counts), `fd_write`, `fd_close`, `fd_seek`, `proc_exit`, clock and random
  access, and the `path_*`/`fd_*` calls behind `std::filesystem`, which may
  fail with `ENOENT`.
* `wasi.thread-spawn` -- what `pthread_create` calls. The host is expected to
  instantiate the module again on a new thread (same memory, table and bases,
  its own `__stack_pointer`) and call the exported `wasi_thread_start(tid,
  start_arg)` there. The new thread's stack and thread-local block are already
  allocated and initialised by `pthread_create` on the parent; the entry
  point installs them from `start_arg`. Only the first instance may run the
  relocations and constructors.

The module never touches a filesystem on its own: the wrapper configures
`SurgeStorage` not to create its user data directory, since a wasi module
without a preopened directory cannot create anything and this is a headless
engine driven over the CLAP API. Preset and MIDI mapping persistence are
therefore no-ops. The `path_*` imports come from `std::filesystem` probes that
are allowed to fail.

Because the toolchain links with `--allow-undefined`, a genuinely missing
symbol becomes an import rather than a link error, so the import list of the
finished module is the thing to check, not a clean link.

## Building with Emscripten

Install and activate the [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html)
(emsdk 3.1.50+ should work, tested with 6.x), then:

```sh
emcmake cmake -Bbuild-wasm -DCMAKE_BUILD_TYPE=Release
cmake --build build-wasm --target surge-wasm
```

The result lands in `build-wasm/surge_xt_products/surge-xt.clap.wasm`.

The build is threaded (`-pthread`) and targets a shared,
`SharedArrayBuffer`-backed memory (`-sSHARED_MEMORY=1`). Both are set globally
in the top-level `CMakeLists.txt` rather than on the module target, because
atomics change code generation for every translation unit and the whole link
has to agree. A module's memory import must match the memory it is linked
against exactly, shared flag included, so a host that drives an AudioWorklet
off a `SharedArrayBuffer` can only load a module built this way.

There is also a small browser demo (parameter list with search and pinning,
virtual keyboard, WebMIDI input) in `src/surge-wasm/demo`:

```sh
cmake --build build-wasm --target surge-wasm-demo
python3 - <<'EOF'
import functools, http.server as h

class Handler(h.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        super().end_headers()

h.test(functools.partial(Handler,
    directory="build-wasm/surge_xt_products/wasm-demo"), port=8000)
EOF
# then open http://localhost:8000/
```

The extra headers are why this is no longer a plain `python3 -m http.server`.
Now that the demo uses a shared memory it only runs in a
[cross-origin isolated](https://developer.mozilla.org/en-US/docs/Web/API/Window/crossOriginIsolated)
page; without `Cross-Origin-Opener-Policy` and `Cross-Origin-Embedder-Policy`,
`SharedArrayBuffer` is undefined and the module will not instantiate.

`SURGE_BUILD_WASM` turns on automatically under the Emscripten toolchain and
forces the JUCE-free configuration (`SURGE_SKIP_LUA`,
`SURGE_SKIP_ODDSOUND_MTS`, no JUCE wrappers).

## Using the module

The module is built with `-sSIDE_MODULE=2`, so it **must** be loaded from an
Emscripten *main module* (built with `-sMAIN_MODULE`). This is not merely the
convenient path: Emscripten side modules have no system libraries linked into
them by design, and there is no setting that changes that. They import libc,
libc++ and the C++ ABI from the main module -- several hundred symbols,
including libc++ vtables and typeinfo objects resolved through `GOT.mem`,
which have to be *addresses of objects living in the main module*. Even
`EMCC_FORCE_STDLIBS` does not help, as `SIDE_MODULE` returns from
`system_libs.get_libs_to_link()` before forced libraries are considered.

A non-Emscripten host (a Rust `wasm32-unknown-unknown` module, say) therefore
cannot link this module, and will fail with several hundred unresolved
imports. Hosting the engine from one means either building an Emscripten main
module to sit between the two, or building the engine with a toolchain that
statically links its runtime.

To load it from a main module:

```js
const surge = await loadDynamicLibrary('surge-xt.clap.wasm', {global: true, nodelete: true});
```

or from C/C++ in the main module via `dlopen()`/`dlsym("clap_entry")`. From
there, the usual CLAP hosting sequence applies: `clap_entry.init()`, get the
`clap_plugin_factory`, create the plugin with id
`org.surge-synth-team.surge-xt`, `init()`, `activate(sampleRate, ...)`, and
call `process()` from your audio callback (an AudioWorklet, typically).

Notes:

* The module is built with `-pthread` against a shared memory, so the main
  module must be built the same way and the page must be cross-origin
  isolated. Emscripten warns that dynamic linking plus pthreads is still
  experimental, because it has to keep the indirect function table in sync
  across threads. Patch loads through the state extension still happen
  synchronously rather than on a loader thread.
* `process()` is happiest with buffer sizes that are a multiple of the engine
  block size (32 samples); anything else works but interleaves event timing
  at block granularity, like the native CLAP.
* C++ exceptions are compiled as native WASM exceptions
  (`-fwasm-exceptions`); the main module must be linked with the same flag.
