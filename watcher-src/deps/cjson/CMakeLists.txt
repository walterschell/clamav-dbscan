cmake_minimum_required(VERSION 3.10)
project(cJSON "C")

add_library(cJSON STATIC cJSON.c)
target_include_directories(cJSON PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
