# ============================================================
# CMakeLists.txt — backend
# ============================================================
#
# Top-level build file for backend/. The four submodules vendored
# under external/ (see .gitmodules) get pulled in via
# add_subdirectory below.
# ============================================================

cmake_minimum_required(VERSION 3.20)

project(
    RaftKV
    VERSION 0.1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# ============================================================================
# Submodules (untouched, upstream -- see .gitmodules)
# ============================================================================

# A submodule vendored under external/ often exposes its own
# BUILD_TESTS / BUILD_BENCHMARKS / BUILD_EXAMPLES options,
# defaulting ON in its own CMakeLists.txt. Force them off here
# before add_subdirectory() picks up those defaults -- this repo
# only wants the library itself, not its test/benchmark/example
# suites.
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(BUILD_REGRESSION OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

# external/raftcore      -> RaftCore     (Raft consensus: RaftNode, NodeState, interfaces)
# external/falconhttp    -> FalconHTTP   (HTTP server/client, routing, SSE)
# external/ratethrottle  -> ThrottlePro  (rate limiter for the client API)
# external/minidatabase  -> MiniDB       (WriteAheadLog backing each node's Raft log)
#
# NOTE: all four submodules vendor independent copies of shared libraries
# (VectorPro, for instance). If more than one of them compiles the *same*
# shared dependency's sources, linking can fail with "multiple definition
# of ..." -- that's a submodule-level packaging issue, not something wrong
# with this file.
add_subdirectory(external/raftcore)
add_subdirectory(external/falconhttp)
add_subdirectory(external/ratethrottle)
add_subdirectory(external/minidatabase)

# ============================================================================
# App library + executable
# ============================================================================

# Everything except main.cpp goes into one static library so the executable
# and the test binary link the exact same code.
file(GLOB_RECURSE APP_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)
list(FILTER APP_SOURCES EXCLUDE REGEX ".*/src/main\\.cpp$")

add_library(raftkv_core STATIC ${APP_SOURCES})

target_include_directories(
    raftkv_core
    PUBLIC
        include
        src
)

target_link_libraries(
    raftkv_core
    PUBLIC
        RaftCore
        FalconHTTP
        ThrottlePro
        MiniDB
)

add_executable(raftkv_backend src/main.cpp)
target_link_libraries(raftkv_backend PRIVATE raftkv_core)

# ============================================================================
# Tests
# ============================================================================

# Named RAFTKV_BUILD_TESTS, not BUILD_TESTS: the latter is force-set OFF above
# for the submodules.
option(RAFTKV_BUILD_TESTS "Build the RaftKV test suite" ON)

if(RAFTKV_BUILD_TESTS)
    file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp"
    )

    add_executable(raftkv_tests ${TEST_SOURCES})

    target_include_directories(
        raftkv_tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/tests
    )

    target_link_libraries(raftkv_tests PRIVATE raftkv_core)
endif()
