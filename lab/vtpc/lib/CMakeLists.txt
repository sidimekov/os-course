add_library(
    vtpc
    STATIC
    vtpc.c
    vtpc_cache.c
    vtpc_fd.c
    vtpc_io.c
    vtpc_state.c
)

target_include_directories(
    vtpc
    PUBLIC
    .
)
