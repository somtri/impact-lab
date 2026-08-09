include(FetchContent)

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.15.3
)
FetchContent_MakeAvailable(Catch2)

add_executable(tests
    test_sanity.cpp
    test_book.cpp
    test_matching.cpp
    test_snapshot.cpp
)
target_link_libraries(tests PRIVATE Catch2::Catch2WithMain impact_engine)
target_include_directories(tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(tests)
