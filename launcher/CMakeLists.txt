cmake_minimum_required(VERSION 3.14)
project(launcher)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_EXTENSIONS OFF)

find_package(DynamoRIO REQUIRED)

add_executable(launcher main.c)

set_target_properties(launcher PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

configure_DynamoRIO_standalone(launcher)

target_link_libraries(
    launcher
    drinjectlib
    drconfiglib
)

