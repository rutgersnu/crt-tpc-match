cmake_minimum_required(VERSION 3.16)
PROJECT(scratch)

SET(CMAKE_CXX_STANDARD 17)

INCLUDE_DIRECTORIES("/home/schwandr/root/include")

add_executable(scratch
        src/wires.cpp
        src/plot.cpp
        src/structs.h
        src/utils.h)