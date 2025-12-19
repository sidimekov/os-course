add_executable(
    io-load
    io_load.c
    io_backend.c
)

target_compile_options(io-load PRIVATE -O3 -march=native -g -Wall -Wextra)

target_link_libraries(io-load PRIVATE vtpc)