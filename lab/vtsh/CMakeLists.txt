cmake_minimum_required(VERSION 3.12)

project(
    vtsh
    VERSION 0.1
    DESCRIPTION "VT Shell"
    LANGUAGES C
)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_subdirectory(bin)
add_subdirectory(lib)

add_executable(proc-clone
    src/proc/proc_clone.c
)

set_target_properties(proc-clone PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

target_compile_options(proc-clone PRIVATE -O0 -g -Wall -Wextra)