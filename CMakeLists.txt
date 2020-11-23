cmake_minimum_required(VERSION 3.13.1)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(smartvan)

FILE(GLOB app_sources src/*.c)
target_sources(app PRIVATE ${app_sources})

# Application directories
add_subdirectory(src/cloud)
add_subdirectory(src/ble)
