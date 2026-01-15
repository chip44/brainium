cmake_minimum_required(VERSION 3.10)
project(client)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_EXTENSIONS OFF)

add_library(client SHARED client.c utils.c)

find_package(DynamoRIO CONFIG REQUIRED)
configure_DynamoRIO_client(client)
use_DynamoRIO_extension(client drmgr)
use_DynamoRIO_extension(client drreg)
use_DynamoRIO_extension(client drutil)
use_DynamoRIO_extension(client drx)

