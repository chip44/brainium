cmake_minimum_required(VERSION 3.14)
project(brainium)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_compile_options(
    -Wall
    -Wextra
    -Wconversion
    -Wshadow
    -Wstrict-prototypes
    -Wmissing-prototypes
    -Wold-style-definition
)

add_compile_definitions(
    _GNU_SOURCE
)

set(CMAKE_C_FLAGS_RELWITHDEBINFO "-DNDEBUG -g -O3 -march=native -ffast-math -flto")

find_package(DynamoRIO REQUIRED)

# launcher

add_executable(launcher
    launcher.c
    ring.c
    shm.c
)

set_target_properties(launcher PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)

configure_DynamoRIO_standalone(launcher)

target_link_libraries(launcher
    drinjectlib
    drconfiglib
)

# client

add_library(client SHARED
    client.c
    ring.c
    shm.c
)

configure_DynamoRIO_client(client)

use_DynamoRIO_extension(client drmgr_static)
use_DynamoRIO_extension(client drreg_static)
use_DynamoRIO_extension(client drutil_static)
use_DynamoRIO_extension(client drx_static)

