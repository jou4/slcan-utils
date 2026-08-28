cmake_minimum_required(VERSION 3.10)

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


install(TARGETS slcd slcw slcr
    RUNTIME DESTINATION bin
)

message(STATUS "--------------------------------------------")
message(STATUS "Project : ${PROJECT_NAME} ${PROJECT_VERSION}")
message(STATUS "Compiler: ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
message(STATUS "Build   : ${CMAKE_BUILD_TYPE}")
message(STATUS "Targets : serial_daemon  serial_writer  serial_reader")
message(STATUS "--------------------------------------------")

