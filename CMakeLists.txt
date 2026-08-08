cmake_minimum_required(VERSION 3.25)
project(impact_lab CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(engine)

enable_testing()
add_subdirectory(tests)
