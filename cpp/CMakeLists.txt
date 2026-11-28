cmake_minimum_required(VERSION 3.20)
project(robrun_esp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Чистое ядро ESP: компилируется на Linux (g++/clang) и Windows (MSVC).
# Windows-специфика (драйвер, overlay, D3D11) — вне этого таргета,
# см. docs/ANALYSIS.md (рецепт сборки overlay под MSVC + WDK).
add_library(robrun_core STATIC
    src/math.cpp
    src/esp.cpp
)
target_include_directories(robrun_core PUBLIC include)
target_compile_options(robrun_core PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
)

enable_testing()
add_executable(robrun_tests tests/test_math.cpp)
target_link_libraries(robrun_tests PRIVATE robrun_core)
add_test(NAME robrun_tests COMMAND robrun_tests)
