cmake_minimum_required(VERSION 3.12)
project(watcher)

add_subdirectory("deps/cjson")
add_subdirectory("deps/libuv")

function(asan target)
    # target_compile_options(${target} PRIVATE -fsanitize=address -fsanitize=undefined -fno-sanitize-recover=all)
    # target_link_options(${target} PRIVATE -fsanitize=address -fsanitize=undefined -fno-sanitize-recover=all -static-libasan)
    target_compile_options(${target} PRIVATE -fsanitize=address -fno-sanitize-recover=all)
    target_link_options(${target} PRIVATE -fsanitize=address -fno-sanitize-recover=all -static-libasan)
endfunction()

function(ubsan target)
    target_compile_options(${target} PRIVATE -fsanitize=undefined -fno-sanitize-recover=all)
    target_link_options(${target} PRIVATE -fsanitize=undefined -fno-sanitize-recover=all -static-libubsan)
endfunction()


function(static target)
    target_compile_options(${target} PRIVATE -static)
    target_link_options(${target} PRIVATE -static)
endfunction()




add_executable(watcher watcher.c)
target_link_libraries(watcher cJSON uv_a)
target_compile_options(watcher PRIVATE -Wall -Wextra -Wpedantic -Werror)

ubsan(watcher)
#asan(watcher)

# static(watcher)

