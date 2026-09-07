# Toolchain for building Surge XT as a self-contained WebAssembly side module.
#
# Unlike the Emscripten build, this targets wasm32-wasi with plain clang and
# links libc, libc++ and compiler-rt statically into the module. That is what
# lets a host which is not itself an Emscripten program load the result: an
# Emscripten side module imports its entire C and C++ runtime from an Emscripten
# main module, and no emcc setting changes that. See
# doc/Building for WebAssembly.md.
#
# The toolchain comes from the flake in the root of this repository, which
# rebuilds the wasi runtimes as position independent code. nixpkgs ships them as
# static non-PIC builds, which cannot go into a shared library:
#
#   relocation R_WASM_MEMORY_ADDR_SLEB cannot be used against symbol `.L.str`;
#   recompile with -fPIC
#
# Configure from inside `nix develop`, which sets the variables read below:
#
#   cmake -B build-wasm -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/wasm-clang-toolchain.cmake \
#     -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-wasm --target surge-wasm

set(CMAKE_SYSTEM_NAME WASI)
set(CMAKE_SYSTEM_VERSION 1)
set(CMAKE_SYSTEM_PROCESSOR wasm32)

# CMake (4.1+) knows this system: it sets WASI, and deliberately not UNIX, so
# the build's platform branches check WASI alongside UNIX where they apply.

# Everything below is supplied by the dev shell rather than searched for, since
# there is no meaningful system-wide install of a wasm sysroot.
foreach(var WASI_SYSROOT WASI_RESOURCE_DIR WASM_CLANG WASM_CLANGXX WASM_AR WASM_RANLIB)
  if(NOT DEFINED ${var} AND DEFINED ENV{${var}})
    set(${var} "$ENV{${var}}")
  endif()
  if(NOT ${var})
    message(FATAL_ERROR
      "${var} is not set. Configure from inside `nix develop`, which provides "
      "the WebAssembly toolchain.")
  endif()
endforeach()

set(CMAKE_C_COMPILER "${WASM_CLANG}")
set(CMAKE_CXX_COMPILER "${WASM_CLANGXX}")

# The host's ar cannot index wasm objects, and lld rejects an archive without
# an index, so the static libraries have to be made with llvm-ar.
set(CMAKE_AR "${WASM_AR}")
set(CMAKE_RANLIB "${WASM_RANLIB}")
set(CMAKE_C_COMPILER_TARGET wasm32-wasi)
set(CMAKE_CXX_COMPILER_TARGET wasm32-wasi)
set(CMAKE_SYSROOT "${WASI_SYSROOT}")

# clang resolves compiler-rt through its own resource directory, whose path is
# baked into the binary and points at a build with no wasm builtins, so it has
# to be redirected at the one the flake assembles.
#
# -fwasm-exceptions selects the wasm exception handling proposal for C++
# exceptions. It changes code generation for every translation unit (landing
# pads become calls into libunwind's _Unwind_CallPersonality) and has to match
# the flag the flake built the runtimes with, so it is a toolchain-wide flag
# rather than a per-target one.
#
# -pthread likewise: it enables the atomics, bulk-memory and mutable-globals
# features for every object, and at link time asks for a shared memory, which
# lld only permits when every object in the link was built with those
# features. The runtimes were, and so must everything here be.
set(WASM_COMMON_FLAGS "-resource-dir=${WASI_RESOURCE_DIR} -fPIC -fwasm-exceptions -pthread")

# The module should export clap_entry and nothing else, and -- more to the
# point -- import nothing it defines itself. In a shared library lld treats
# every visibility-default definition as preemptible, so references to it go
# through GOT.mem imports the host would have to bind back to the module's own
# exports. Surge already compiles hidden, but libc++'s headers annotate their
# templates visibility-default, so every vtable and typeinfo of a std::function
# or std::regex instantiated in Surge's own objects came out that way: ~100
# GOT.mem imports. _LIBCPP_DISABLE_VISIBILITY_ANNOTATIONS is libc++'s switch
# for exactly this (hermetic static linking), and matches how the flake built
# libc++.a. -fvisibility=hidden covers the vendored C libraries the same way.
#
# _WASI_EMULATED_GETPID declares getpid(), which wasi has no system call for;
# the emulation library linked below answers it.
set(WASM_COMMON_COMPILE_FLAGS
  "-fvisibility=hidden -D_LIBCPP_DISABLE_VISIBILITY_ANNOTATIONS -D_WASI_EMULATED_GETPID")

set(CMAKE_C_FLAGS_INIT "${WASM_COMMON_FLAGS} ${WASM_COMMON_COMPILE_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${WASM_COMMON_FLAGS} ${WASM_COMMON_COMPILE_FLAGS}")

# -shared asks lld for a relocatable module with a dylink.0 section describing
# how much memory and table space it needs. --no-entry because a side module has
# no _start, and --export=clap_entry because the whole point of the module is
# that one symbol; a side module exports data symbols as globals holding their
# address, which is what a loader reads.
#
# --allow-undefined turns what is left unresolved into imports rather than
# errors. That is load bearing rather than lazy: wasi-libc's allocator refers to
# __heap_base and __heap_end, which lld only synthesises when linking an
# executable, because a shared library does not own the heap. As imports they
# become GOT.mem entries the host fills in, which is the right answer anyway --
# it lets the plugin allocate out of a region the host chose. The cost is that a
# genuinely missing symbol also becomes an import instead of a link error, so
# check the import list of the finished module rather than trusting the link.
#
# wasi-libc's threading code refers to __wasilibc_futex_wait_maybe_busy only
# weakly, and a weak reference does not pull the member of libc.a that defines
# it, so in a shared library it would come out as a weak import for the host
# to satisfy. It is right there in the archive; ask for it by name.
#
# --export-if-defined=__wasm_call_ctors: a side module's static constructors
# are run by its loader, after it has applied the data relocations, and lld
# only keeps the function that runs them if something exports it. Without
# this the constructors are garbage collected and every static with a dynamic
# initializer (the CLAP plugin descriptor, for one) stays zero.
#
# The TLS exports: lld keeps the thread-local template as a passive segment
# and synthesises __wasm_init_tls(base) to instantiate it for a thread, but a
# shared library exports none of that unless asked, and with nothing to
# initialise it __tls_base stays 0 -- every thread-local (errno, the pthread
# self pointer, libunwind's exception hand-off block) would then sit at the
# bottom of the host's memory. The loader allocates __tls_size bytes at
# __tls_align for each thread and calls __wasm_init_tls there; on the main
# thread it also calls __wasi_init_tp, which is what wasi-libc's crt would have
# done to set up the main thread's pthread structure.
set(WASM_TLS_EXPORTS
  "-Wl,--export-if-defined=__wasm_init_tls -Wl,--export-if-defined=__tls_size -Wl,--export-if-defined=__tls_align -Wl,--export-if-defined=__tls_base -Wl,--export-if-defined=__wasi_init_tp")
set(CMAKE_SHARED_LINKER_FLAGS_INIT
  "${WASM_COMMON_FLAGS} -nostartfiles -Wl,--no-entry -Wl,--export=clap_entry -Wl,--export-if-defined=__wasm_call_ctors ${WASM_TLS_EXPORTS} -Wl,--allow-undefined -Wl,--undefined=__wasilibc_futex_wait_maybe_busy")

# clang adds -lc++ -lc++abi -lc itself, but not the unwinder that libc++abi
# throws through, nor wasi-libc's getpid() emulation; lld binds archive
# members on demand regardless of order, so it is fine for these to sit ahead
# of the objects on the link line.
set(CMAKE_CXX_STANDARD_LIBRARIES_INIT "-lunwind -lwasi-emulated-getpid")

# There is no host filesystem to search for libraries or programs.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# CMake's default compiler check links an executable, which needs a _start and
# a crt this toolchain deliberately does not use.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
