# vi:set sw=2 et:
# Surge XT as a WebAssembly side module exposing the CLAP C API.
#
# Build with the Emscripten toolchain:
#   emcmake cmake -Bbuild-wasm -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-wasm --target surge-wasm
#
# The result is surge_xt_products/surge-xt.clap.wasm, a relocatable side
# module which exports clap_entry, the same C entry point the native CLAP
# plugin exports. Load it from a main module (dlopen()/loadDynamicLibrary())
# and drive it through the CLAP plugin API. There is no GUI extension and no
# bundled content; patches travel through the CLAP state extension.
project(surge-wasm)

# The CLAP headers normally arrive via clap-juce-extensions, which the WASM
# build skips along with the rest of JUCE, so pull them in directly
if(NOT TARGET clap)
  add_subdirectory(${CMAKE_SOURCE_DIR}/libs/clap-juce-extensions/clap-libs/clap
    ${CMAKE_BINARY_DIR}/libs/clap EXCLUDE_FROM_ALL)
endif()

if(NOT TARGET clap-helpers)
  add_subdirectory(${CMAKE_SOURCE_DIR}/libs/clap-juce-extensions/clap-libs/clap-helpers
    ${CMAKE_BINARY_DIR}/libs/clap-helpers EXCLUDE_FROM_ALL)
endif()

add_library(${PROJECT_NAME} SHARED SurgeWasmCLAP.cpp)

target_link_libraries(${PROJECT_NAME} PRIVATE
  surge::surge-common
  clap
  clap-helpers
)

set_target_properties(${PROJECT_NAME} PROPERTIES
  OUTPUT_NAME "surge-xt.clap"
  PREFIX ""
  SUFFIX ".wasm"
  LIBRARY_OUTPUT_DIRECTORY ${SURGE_PRODUCT_DIR}
)

target_link_options(${PROJECT_NAME} PRIVATE
  # A side module: relocatable, linked into a host-provided main module at
  # runtime. =2 applies dead code elimination, keeping only the symbols
  # below (and their dependencies) rather than everything visible.
  -sSIDE_MODULE=2
  -sEXPORTED_FUNCTIONS=_clap_entry
)

# A small browser demo hosting the side module: a main module with a flat C
# API plus a dependency-free HTML page. Serve the wasm-demo output directory
# over HTTP and open index.html.
add_executable(surge-wasm-demo demo/demo-host.c)
target_link_libraries(surge-wasm-demo PRIVATE clap)
add_dependencies(surge-wasm-demo ${PROJECT_NAME})

set_target_properties(surge-wasm-demo PROPERTIES
  OUTPUT_NAME "demo-host"
  RUNTIME_OUTPUT_DIRECTORY ${SURGE_PRODUCT_DIR}/wasm-demo
)

target_link_options(surge-wasm-demo PRIVATE
  -sMAIN_MODULE=1
  -sMODULARIZE=1
  -sEXPORT_NAME=createSurgeDemo
  -sALLOW_MEMORY_GROWTH
  -sSTACK_SIZE=2097152
  -sEXPORTED_RUNTIME_METHODS=cwrap,FS,HEAPF32
  # the sh_* API is exported via EMSCRIPTEN_KEEPALIVE; the page also needs
  # malloc for its audio transfer buffers
  -sEXPORTED_FUNCTIONS=_malloc,_free
  --no-entry
)

add_custom_command(TARGET surge-wasm-demo POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_CURRENT_SOURCE_DIR}/demo/index.html
    $<TARGET_FILE:${PROJECT_NAME}>
    ${SURGE_PRODUCT_DIR}/wasm-demo/
)
