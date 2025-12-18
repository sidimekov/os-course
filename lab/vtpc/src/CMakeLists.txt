add_executable(
    cpu-ema-hash-join-vtpc
    cpu_ema_hash_join_vtpc.c
)

target_compile_options(cpu-ema-hash-join-vtpc PRIVATE -O3 -march=native -g -Wall -Wextra)

target_link_libraries(cpu-ema-hash-join-vtpc PRIVATE vtpc)