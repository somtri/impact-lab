add_library(impact_engine STATIC
    src/book.cpp
    src/engine.cpp
    src/lobster.cpp
    src/replay.cpp
    src/synthetic.cpp
    src/tardis.cpp
    src/validation.cpp
)
target_include_directories(impact_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# The committed synthetic tape is a source artefact, not build output: engine_replay and the
# golden test both read this one path so they cannot drift apart.
set(IMPACT_SYNTHETIC_TAPE "${CMAKE_SOURCE_DIR}/tests/data/synthetic_tape.txt"
    CACHE INTERNAL "committed synthetic tape")
set(IMPACT_SYNTHETIC_GOLDEN "${CMAKE_SOURCE_DIR}/tests/data/synthetic_golden.txt"
    CACHE INTERNAL "committed golden replay report")

add_executable(engine src/main.cpp)

add_executable(engine_replay src/replay_main.cpp)
target_link_libraries(engine_replay PRIVATE impact_engine)
target_compile_definitions(engine_replay PRIVATE IMPACT_SYNTHETIC_TAPE="${IMPACT_SYNTHETIC_TAPE}")

# lobster_replay reads plain-text CSVs in place (LOBSTER ships uncompressed), so it needs no
# zlib and always builds, unlike tardis_replay below.
add_executable(lobster_replay src/lobster_main.cpp)
target_link_libraries(lobster_replay PRIVATE impact_engine)

# tardis_replay reads the gz files in place, so it needs zlib. The adapter and the validation
# comparator themselves take only text, so they live in impact_engine and are unit-tested with no
# zlib and no data file. A machine without zlib still builds and passes the whole test suite; it
# just cannot run a real day.
find_package(ZLIB QUIET)
if(ZLIB_FOUND)
    add_executable(tardis_replay src/tardis_main.cpp)
    target_link_libraries(tardis_replay PRIVATE impact_engine ZLIB::ZLIB)

    # Stage 2 step 3-4 benchmark harness (docs/BENCHMARKS.md). Same zlib dependency as
    # tardis_replay, for the same reason: it streams the real gz day.
    add_executable(tardis_bench src/bench_main.cpp)
    target_link_libraries(tardis_bench PRIVATE impact_engine ZLIB::ZLIB)
else()
    message(STATUS "zlib not found: skipping tardis_replay and tardis_bench (adapter tests still build)")
endif()
