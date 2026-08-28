cmake_minimum_required(VERSION 3.14)   # FetchContent_MakeAvailable() (see the Lua block below) needs 3.14+

project(slcan-utils
    VERSION     0.0.1
    DESCRIPTION "SLCAN utility for Windows"
    LANGUAGES   C
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT WIN32)
    message(FATAL_ERROR "This project targets Windows only (Named Pipe / Win32 API)")
endif()

# Single-config generators (Makefiles, Ninja, ...) leave CMAKE_BUILD_TYPE
# empty unless -DCMAKE_BUILD_TYPE=... is passed on the command line, which
# builds with no optimization / no NDEBUG (Debug-like) by default. Default
# it to Release here so a forgotten -D still gets an optimized build.
#
# Multi-config generators (Visual Studio, ...) ignore CMAKE_BUILD_TYPE
# entirely; the configuration is chosen at build time instead (IDE's
# Solution Configuration dropdown, or `cmake --build . --config Release`),
# so this guard intentionally does nothing for them.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_library(slcan STATIC
    src/slcan.c
)
target_include_directories(slcan PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:src>
)

# ---- Lua (embedded scripting for slcgw's gateway rules) --------------
#
# Lua's own release tarball ships plain .c/.h files with no CMake (or
# even a from-source library target in its Makefile beyond the CLI
# lua/luac tools), so FetchContent just downloads+extracts it and the
# library's source list below is maintained by hand against Lua's own
# makefile (CORE_O + AUX_O + LIB_O, minus lua.o/luac.o -- the
# standalone CLI frontends -- and ltests.o -- Lua's own internal test
# harness -- neither of which is needed when embedding Lua as a
# library). Re-check that list if LUA_VERSION is ever bumped.
#
# No URL_HASH is pinned below: lua.org doesn't publish an official
# sha256 for this release at time of writing. If integrity pinning is
# wanted, run `sha256sum` on a verified download of the tarball once
# and add `URL_HASH SHA256=<that value>` to the FetchContent_Declare()
# call.
include(FetchContent)

set(LUA_VERSION 5.5.1)
FetchContent_Declare(
    lua
    URL https://www.lua.org/ftp/lua-${LUA_VERSION}.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(lua)

set(LUA_SOURCES
    # core interpreter (CORE_O in Lua's makefile)
    lapi.c lcode.c lctype.c ldebug.c ldo.c ldump.c lfunc.c lgc.c llex.c
    lmem.c lobject.c lopcodes.c lparser.c lstate.c lstring.c ltable.c
    ltm.c lundump.c lvm.c lzio.c
    # auxiliary library (AUX_O)
    lauxlib.c
    # standard libraries (LIB_O)
    lbaselib.c ldblib.c liolib.c lmathlib.c loslib.c ltablib.c lstrlib.c
    lutf8lib.c loadlib.c lcorolib.c linit.c
)
list(TRANSFORM LUA_SOURCES PREPEND "${lua_SOURCE_DIR}/src/")

add_library(lua STATIC ${LUA_SOURCES})
target_include_directories(lua PUBLIC ${lua_SOURCE_DIR}/src)
if(MSVC)
    # Lua's own liolib.c/loslib.c call fopen()/getenv()/etc.; silence
    # MSVC's C4996 "unsafe function" deprecation warnings for Lua's own
    # source rather than patching a fetched third-party tree.
    target_compile_definitions(lua PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(slcd
    src/serial_daemon.c
)
target_link_libraries(slcd PRIVATE slcan)

add_executable(slcw
    src/serial_writer.c
)
target_link_libraries(slcw PRIVATE slcan)

add_executable(slcr
    src/serial_reader.c
)
target_link_libraries(slcr PRIVATE slcan)

add_executable(slcplay
    src/slcan_player.c
)
target_link_libraries(slcplay PRIVATE slcan)

add_executable(slcgw
    src/slcan_gateway.c
)
target_link_libraries(slcgw PRIVATE slcan lua)

install(TARGETS slcd slcw slcr slcplay slcgw
    RUNTIME DESTINATION bin
)

message(STATUS "--------------------------------------------")
message(STATUS "Project : ${PROJECT_NAME} ${PROJECT_VERSION}")
message(STATUS "Compiler: ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
message(STATUS "Build   : ${CMAKE_BUILD_TYPE}")
message(STATUS "Targets : serial_daemon  serial_writer  serial_reader  slcan_player  slcan_gateway")
message(STATUS "--------------------------------------------")

