add_library(impact_engine STATIC
    src/book.cpp
    src/engine.cpp
)
target_include_directories(impact_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_executable(engine src/main.cpp)
