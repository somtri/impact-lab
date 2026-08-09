add_library(impact_engine STATIC
    src/book.cpp
    src/engine.cpp
    src/replay.cpp
    src/synthetic.cpp
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
