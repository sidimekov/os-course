add_executable(
    cpu-linreg
    cpu_linreg.c
)

add_executable(
    cpu-ema-hash-join
    cpu_ema_hash_join.c
)

target_compile_options(
    cpu-linreg PRIVATE -O0 -g -Wall -Wextra
    cpu-ema-hash-join PRIVATE -O0 -g -Wall -Wextra
)

target_link_libraries(
    cpu-linreg m
)
