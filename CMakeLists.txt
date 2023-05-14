cmake_minimum_required(VERSION 3.10)

project(socket-io-uring LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_library(URING_LIBRARY uring)

add_executable(server server.cpp)
target_link_libraries(server ${URING_LIBRARY})

add_executable(client client.cpp)
target_link_libraries(client pthread)