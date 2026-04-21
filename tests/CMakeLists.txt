cmake_minimum_required(VERSION 3.16)
project(emblogx_lib_tests)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(FETCHCONTENT_BASE_DIR ${CMAKE_CURRENT_LIST_DIR}/vendor)

include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
)
FetchContent_MakeAvailable(googletest)

# ---------------------------------------------------------------------------
# Dependency: lib_sd (headers only — SdSink tests use the IFile/IFileSystem
# interfaces with mock implementations).
#
# By default we use the sibling directory (../../lib_sd) which is the layout
# during local development. External consumers who cloned only lib_emblogx
# can set -DUSE_LOCAL_DEPS=OFF to fetch it from GitHub instead.
# ---------------------------------------------------------------------------
option(USE_LOCAL_DEPS "Use local sibling lib_sd/ directory" ON)

if(USE_LOCAL_DEPS)
    set(LIB_SD_SRC ${CMAKE_CURRENT_LIST_DIR}/../../lib_sd/src)
else()
    FetchContent_Declare(
        ungula_sd
        GIT_REPOSITORY https://github.com/alexconesap/ungula-sd.git
        GIT_TAG main
    )
    FetchContent_MakeAvailable(ungula_sd)
    set(LIB_SD_SRC ${ungula_sd_SOURCE_DIR}/src)
endif()

set(LIB_EMBLOGX_SRC ${CMAKE_CURRENT_LIST_DIR}/../src)

include_directories(
    ${LIB_EMBLOGX_SRC}
    ${LIB_SD_SRC}
    ${CMAKE_CURRENT_LIST_DIR}/ArduinoStub
)

# Host build — STDIO backend, no FreeRTOS, async sinks fall back to sync.
# Console sink and memory sink are the only ones we need for unit tests.
add_compile_definitions(
    EMBLOGX_BACKEND_STDIO
    EMBLOGX_LOG_PREFIX="[TEST] "
    EMBLOGX_ENABLE_SINK_CONSOLE=1
    EMBLOGX_ENABLE_SINK_MEMORY=1
    EMBLOGX_ENABLE_SINK_HTTP=0
    EMBLOGX_ENABLE_SINK_SD=1
    EMBLOGX_DEBUG_ENABLED
    # Tests share one process and register sinks across cases without
    # resetting the registry, so the production cap of 8 is too low here.
    EMBLOGX_MAX_SINKS=32
)

add_executable(test_emblogx
    test_emblogx.cpp
    ${LIB_EMBLOGX_SRC}/emblogx/logger_core.cpp
    ${LIB_EMBLOGX_SRC}/emblogx/async_dispatcher.cpp
    ${LIB_EMBLOGX_SRC}/emblogx/sinks/console_sink.cpp
    ${LIB_EMBLOGX_SRC}/emblogx/sinks/memory_sink.cpp
    ${LIB_EMBLOGX_SRC}/emblogx/sinks/sd_sink.cpp
)
# Test sources include <emblogx/...> as if they were a real consumer.
target_include_directories(test_emblogx PRIVATE ${LIB_EMBLOGX_SRC})
target_link_libraries(test_emblogx gtest_main)

enable_testing()
include(GoogleTest)
gtest_discover_tests(test_emblogx)
