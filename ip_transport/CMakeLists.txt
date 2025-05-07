cmake_minimum_required(VERSION 3.20.0)
project(IP_TRANSPORT)

include_directories(./include)
include_directories(./interface)
include_directories(./types)
include_directories(./util)

file(GLOB_RECURSE IP_SRC "./src/*.cpp")

add_executable(main main.cpp ${IP_SRC}) 
