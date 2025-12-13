add_library(
    libvtsh
    STATIC
    vtsh_core.c
    vtsh_parse.c
    vtsh_exec.c
    vtsh_redir.c
)

target_include_directories(
    libvtsh
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
