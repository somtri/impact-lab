include(FetchContent)

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.15.3
)
FetchContent_MakeAvailable(Catch2)

add_executable(tests
    test_sanity.cpp
    test_header_standalone.cpp
    test_book.cpp
    test_matching.cpp
    test_snapshot.cpp
    test_property.cpp
    test_replay_golden.cpp
    test_tardis_adapter.cpp
    test_validation.cpp
    test_lobster_adapter.cpp
)
target_link_libraries(tests PRIVATE Catch2::Catch2WithMain impact_engine)
target_include_directories(tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(tests PRIVATE
    IMPACT_SYNTHETIC_TAPE="${IMPACT_SYNTHETIC_TAPE}"
    IMPACT_SYNTHETIC_GOLDEN="${IMPACT_SYNTHETIC_GOLDEN}"
)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(tests)

# The Catch2 case above replays the tape in process. This one runs the real engine_replay binary
# twice and compares the two outputs and the golden file byte for byte, which is the claim the
# stage is judged on.
add_test(NAME replay_binary_is_deterministic
    COMMAND ${CMAKE_COMMAND}
        -DREPLAY=$<TARGET_FILE:engine_replay>
        -DGOLDEN=${IMPACT_SYNTHETIC_GOLDEN}
        -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/replay_determinism.cmake
)
