cmake_minimum_required(VERSION 3.14)
project(brainium)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_EXTENSIONS OFF)

add_compile_definitions(_POSIX_C_SOURCE=200112L)

find_package(DynamoRIO REQUIRED)

# launcher

add_executable(launcher launcher.c shm.c)

set_target_properties(launcher PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

configure_DynamoRIO_standalone(launcher)

target_link_libraries(
    launcher
    drinjectlib
    drconfiglib
)

# client

add_library(client SHARED client.c shm.c)

configure_DynamoRIO_client(client)

use_DynamoRIO_extension(client drmgr)
use_DynamoRIO_extension(client drreg)
use_DynamoRIO_extension(client drutil)
use_DynamoRIO_extension(client drx)

