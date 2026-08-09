cmake_minimum_required(VERSION 3.25)
project(impact_wasm CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT EMSCRIPTEN)
    message(FATAL_ERROR
        "demo/wasm builds only under the Emscripten toolchain. Configure with "
        "'emcmake cmake -S demo/wasm -B demo/wasm/build -G Ninja' after sourcing "
        "emsdk_env.sh -- see demo/wasm/README.md.")
endif()

# engine/include and engine/src are read-only for this build (PLAN.md Stage 5 step 3): this
# project compiles the unmodified engine sources it needs directly, rather than editing
# engine/CMakeLists.txt or the root CMakeLists.txt.
set(IMPACT_ENGINE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../engine)

add_executable(impact_wasm
    bindings.cpp
    ${IMPACT_ENGINE_DIR}/src/book.cpp
    ${IMPACT_ENGINE_DIR}/src/engine.cpp
    ${IMPACT_ENGINE_DIR}/src/replay.cpp
    ${IMPACT_ENGINE_DIR}/src/synthetic.cpp
)
target_include_directories(impact_wasm PRIVATE ${IMPACT_ENGINE_DIR}/include)
target_compile_options(impact_wasm PRIVATE -fexceptions)

# --bind activates Embind. -sWASM_BIGINT: int64/uint64 cross the JS boundary as BigInt, exact,
# no float ever touches a price or a size. MODULARIZE + EXPORT_NAME give a factory function that
# loads cleanly under Node (the golden/smoke tests) and in a browser (the eventual frontend).
target_link_options(impact_wasm PRIVATE
    --bind
    -fexceptions
    -sWASM_BIGINT
    -sMODULARIZE=1
    -sEXPORT_NAME=ImpactWasm
    -sENVIRONMENT=node,web
    -sALLOW_MEMORY_GROWTH=1
)
