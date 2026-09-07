{
  description = "Surge XT development environment, including a WebAssembly side-module toolchain";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs =
    {
      nixpkgs,
      flake-utils,
      ...
    }:
    flake-utils.lib.eachDefaultSystem (
      system:
      let
        pkgs = import nixpkgs { inherit system; };

        # The WASM build targets wasm32-wasi rather than Emscripten, so that the
        # engine can be linked into a host that is not itself an Emscripten
        # program. See doc/Building for WebAssembly.md for why.
        #
        # Everything the module links against has to be built position
        # independent, because the result is a shared library (a "side module"):
        # its data lands at a __memory_base the loader picks, so every address in
        # it has to be relative. nixpkgs' wasi32 runtimes are static/non-PIC
        # builds, and linking them produces
        #
        #   relocation R_WASM_MEMORY_ADDR_SLEB cannot be used against symbol
        #   `.L.str`; recompile with -fPIC
        #
        # so we rebuild them here with position independent code turned on.
        wasiPkgs = pkgs.pkgsCross.wasi32;

        # These derivations carry their compiler flags in `env`, so the extra
        # -fPIC has to be merged in there rather than passed as a top level
        # derivation argument, which nix rejects as overlapping.
        withPic =
          drv:
          drv.overrideAttrs (old: {
            cmakeFlags = (old.cmakeFlags or [ ]) ++ [
              "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"
            ];
            env = (old.env or { }) // {
              NIX_CFLAGS_COMPILE = "${old.env.NIX_CFLAGS_COMPILE or ""} -fPIC";
            };
          });

        # Surge uses std::thread and std::mutex outright (patch loading runs on
        # a background thread), and libc++ refuses to even provide <mutex> when
        # built without threads, so the whole runtime stack is built threaded:
        # wasi-libc in its posix thread model, and everything compiled with
        # -pthread, which turns on the atomics, bulk-memory and mutable-globals
        # features. lld only allows a shared memory when every object was built
        # with those, so this has to be uniform across the runtimes and the
        # module itself; the toolchain file adds the same flag.
        withPosixThreads =
          drv:
          drv.overrideAttrs (old: {
            env = (old.env or { }) // {
              NIX_CFLAGS_COMPILE = "${old.env.NIX_CFLAGS_COMPILE or ""} -pthread";
            };
          });

        wasilibcPic = (wasiPkgs.wasilibc.override { enablePosixThreads = true; }).overrideAttrs (old: {
          # wasi-libc is a plain Makefile build; EXTRA_CFLAGS is its hook for
          # the compile flags of every object. It *replaces* the default
          # (-O2 -DNDEBUG) rather than adding to it, so repeat that here. A
          # value with spaces has to go through makeFlagsArray, which is also
          # how nixpkgs passes its own flags in preBuild.
          preBuild = (old.preBuild or "") + ''
            makeFlagsArray+=("EXTRA_CFLAGS=-O2 -DNDEBUG -fPIC")
          '';

          # wasi-libc diffs its own symbol lists against checked-in baselines,
          # and a position independent build legitimately does not match them:
          # it gains __table_base and __pic__, among others. The step also
          # generates the sysroot metadata we want to keep, so let it run and
          # report, but do not let the comparison fail the build.
          #
          # The one hand-written assembly file in the threaded libc, the entry
          # point a host calls on a freshly spawned thread, takes the address
          # of __thread_list_lock as an absolute constant, which a relocatable
          # module cannot contain ("relocation R_WASM_MEMORY_ADDR_SLEB cannot
          # be used against symbol __thread_list_lock"). wasi-libc's Makefile
          # has a TODO about exactly this. Rewrite it the way the compiler
          # spells a hidden symbol's address under -fPIC: relative to the
          # module's __memory_base.
          postPatch = (old.postPatch or "") + ''
            substituteInPlace Makefile \
              --replace-fail \
                'diff -wur "$(EXPECTED_TARGET_DIR)" "$(SYSROOT_SHARE)"' \
                'diff -wur "$(EXPECTED_TARGET_DIR)" "$(SYSROOT_SHARE)" || true'

            substituteInPlace libc-top-half/musl/src/thread/wasm32/wasi_thread_start.s \
              --replace-fail \
                '	.globaltype	__tls_base, i32' \
                '	.globaltype	__tls_base, i32
	.globaltype	__memory_base, i32, immutable' \
              --replace-fail \
                '	i32.const   __thread_list_lock' \
                '	global.get  __memory_base
	i32.const   __thread_list_lock@MBREL
	i32.add'
          '';
        });

        compilerRtPic = withPosixThreads (withPic wasiPkgs.llvmPackages.compiler-rt);

        # Surge relies on C++ exceptions, which on wasm means the exception
        # handling proposal driven through -fwasm-exceptions. nixpkgs builds the
        # wasi runtimes with LIBCXX_ENABLE_EXCEPTIONS=FALSE and
        # LIBCXXABI_ENABLE_EXCEPTIONS=FALSE, so both are flipped back on here,
        # and every runtime is compiled with -fwasm-exceptions so that the
        # exception-model specific code (__gxx_personality_wasm0 in libc++abi,
        # Unwind-wasm.c in libunwind, both guarded on the __WASM_EXCEPTIONS__
        # macro that flag defines) is actually compiled in. The same flag has to
        # be used for every object in the final module; the CMake toolchain file
        # adds it.
        #
        # nixpkgs' wasm cc-wrapper puts -fno-exceptions in its baked-in cflags,
        # which is why flipping the CMake options alone still fails with
        # "cannot use 'try' with exceptions disabled": the flag never appears in
        # the build's own command line. NIX_CFLAGS_COMPILE lands after the
        # wrapper's flags, so an explicit -fexceptions here is what wins.
        withWasmExceptions =
          drv:
          drv.overrideAttrs (old: {
            env = (old.env or { }) // {
              NIX_CFLAGS_COMPILE = "${old.env.NIX_CFLAGS_COMPILE or ""} -fexceptions -fwasm-exceptions";
            };
          });

        # libc++abi throws through the language-neutral _Unwind_* API, which for
        # wasm lives in libunwind's Unwind-wasm.c: _Unwind_RaiseException is the
        # wasm `throw` instruction, and _Unwind_CallPersonality is what the
        # compiler makes every landing pad call, because the VM does the
        # unwinding and cannot call the personality routine itself. nixpkgs'
        # libc++ derivation does not build libunwind for wasm at all, so it is
        # built separately here and linked in by the toolchain file.
        #
        # LIBUNWIND_HIDE_SYMBOLS is libunwind's equivalent of the hermetic
        # libc++ build below: without it the _Unwind_* entry points are
        # visibility-default and would come back as imports of the module.
        libunwindPic = (withPosixThreads (withWasmExceptions (withPic wasiPkgs.llvmPackages.libunwind))).overrideAttrs (old: {
          # Every C and C++ source in libunwind guards itself on __wasm__ and
          # compiles to nothing here, but the two register save/restore
          # assembly files include assembly.h before their own guard, and that
          # header errors out on any architecture it does not know. There are
          # no registers to save on wasm, so just leave them out of the build.
          postPatch = (old.postPatch or "") + ''
            substituteInPlace ../libunwind/src/CMakeLists.txt \
              --replace-fail "UnwindRegistersRestore.S" "" \
              --replace-fail "UnwindRegistersSave.S" ""
          '';

          cmakeFlags = (old.cmakeFlags or [ ]) ++ [
            "-DLIBUNWIND_HIDE_SYMBOLS:BOOL=TRUE"
            "-DLIBUNWIND_ENABLE_THREADS:BOOL=TRUE"
            # The same wasm accommodations nixpkgs makes for its libc++ build
            # but not for libunwind: LLVM's option handling refuses a system it
            # cannot classify, and the compiler check wants to link an
            # executable, which needs a _start this target does not have.
            "-DUNIX:BOOL=TRUE"
            "-DCMAKE_C_COMPILER_WORKS:BOOL=TRUE"
            "-DCMAKE_CXX_COMPILER_WORKS:BOOL=TRUE"
          ];
        });

        # The hermetic flags are what stops libc++ symbols leaking into the
        # module's import list. libc++ marks its ABI surface (operator new, the
        # extern-template basic_string members) visibility default, and in a
        # shared library lld emits an import for every such reference even when
        # the definition is right there in the archive, leaving it to a dynamic
        # loader to bind. That would put ~130 C++ symbols back in the imports,
        # which is the whole problem we are trying to get away from. A hermetic
        # build compiles them hidden, so they resolve internally.
        libcxxPic = (withPosixThreads (withWasmExceptions (withPic wasiPkgs.llvmPackages.libcxx))).overrideAttrs (old: {
          cmakeFlags = (old.cmakeFlags or [ ]) ++ [
            "-DLIBCXX_HERMETIC_STATIC_LIBRARY:BOOL=TRUE"
            "-DLIBCXXABI_HERMETIC_STATIC_LIBRARY:BOOL=TRUE"
            "-DLIBCXX_ENABLE_EXCEPTIONS:BOOL=TRUE"
            "-DLIBCXXABI_ENABLE_EXCEPTIONS:BOOL=TRUE"
            # The same set wasi-sdk uses for its *-threads sysroots: wasi-libc
            # exposes pthreads, so tell libc++ not to try to detect them.
            "-DLIBCXX_ENABLE_THREADS:BOOL=TRUE"
            "-DLIBCXX_HAS_PTHREAD_API:BOOL=TRUE"
            "-DLIBCXXABI_ENABLE_THREADS:BOOL=TRUE"
            "-DLIBCXXABI_HAS_PTHREAD_API:BOOL=TRUE"
            # nixpkgs leaves std::filesystem out of its wasm libc++, but wasi-libc
            # has what it needs and wasi-sdk ships it. Surge's filesystem layer
            # (sst-plugininfra) prefers the platform one where it compiles, and
            # its bundled ghc::filesystem fallback does not know wasi at all.
            "-DLIBCXX_ENABLE_FILESYSTEM:BOOL=TRUE"
          ];
        });

        # clang looks for a sysroot laid out as include/ plus lib/<triple>/, and
        # nixpkgs splits headers into separate `dev` outputs, so stitch the two
        # halves back into the single tree clang expects.
        wasiSysroot = pkgs.runCommand "wasi-sysroot-pic" { } ''
          mkdir -p $out/include $out/lib/wasm32-wasi

          cp -r ${wasilibcPic.dev}/include/. $out/include/
          cp -r ${libcxxPic.dev}/include/. $out/include/
          chmod -R u+w $out/include

          cp ${wasilibcPic}/lib/*.a $out/lib/wasm32-wasi/
          cp ${libcxxPic}/lib/*.a $out/lib/wasm32-wasi/
          cp ${libunwindPic}/lib/*.a $out/lib/wasm32-wasi/
        '';

        # clang resolves compiler-rt through its own resource directory rather
        # than the sysroot, and that path is baked into the binary, so we build a
        # replacement holding both clang's builtin headers and the wasm builtins,
        # and point -resource-dir at it.
        wasiResourceDir = pkgs.runCommand "wasi-resource-dir" { } ''
          mkdir -p $out/lib/wasm32-unknown-wasi

          cp -r ${pkgs.llvmPackages.clang-unwrapped.lib}/lib/clang/*/include $out/
          chmod -R u+w $out/include

          cp ${compilerRtPic}/lib/wasi/libclang_rt.builtins-wasm32.a \
            $out/lib/wasm32-unknown-wasi/libclang_rt.builtins.a
        '';

        # The nix cc-wrapper injects host hardening flags such as
        # -fzero-call-used-regs=used-gpr, which clang rejects outright for a wasm
        # target, so the WASM build has to drive the unwrapped compiler and be
        # told where everything lives explicitly.
        wasmClang = pkgs.llvmPackages.clang-unwrapped;

        wasmTools = [
          wasmClang
          pkgs.llvmPackages.llvm
          pkgs.lld
          pkgs.cmake
          pkgs.ninja
          pkgs.binaryen
          pkgs.wabt
        ];
      in
      {
        packages = {
          inherit
            wasiSysroot
            wasiResourceDir
            wasilibcPic
            libcxxPic
            libunwindPic
            compilerRtPic
            ;
        };

        devShells.default = pkgs.mkShell {
          packages = wasmTools ++ [
            pkgs.git
            pkgs.pkg-config
          ];

          # Consumed by cmake/wasm-clang-toolchain.cmake.
          WASI_SYSROOT = wasiSysroot;
          WASI_RESOURCE_DIR = wasiResourceDir;
          WASM_CLANG = "${wasmClang}/bin/clang";
          WASM_CLANGXX = "${wasmClang}/bin/clang++";
          WASM_AR = "${pkgs.llvmPackages.llvm}/bin/llvm-ar";
          WASM_RANLIB = "${pkgs.llvmPackages.llvm}/bin/llvm-ranlib";

          shellHook = ''
            echo "Surge XT dev shell."
            echo "  WASM side module:"
            echo "    cmake -B build-wasm -G Ninja \\"
            echo "      -DCMAKE_TOOLCHAIN_FILE=cmake/wasm-clang-toolchain.cmake \\"
            echo "      -DCMAKE_BUILD_TYPE=Release"
            echo "    cmake --build build-wasm --target surge-wasm"
          '';
        };
      }
    );
}
