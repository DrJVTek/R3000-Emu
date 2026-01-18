cmake_minimum_required(VERSION 3.20)

project(R3000_Emu LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(r3000_emu
  src/main.cpp
  src/log/logger.cpp
  src/gte/gte.cpp
  src/loader/loader.cpp
  src/r3000/bus.cpp
  src/r3000/cpu.cpp
)

target_include_directories(r3000_emu PRIVATE src)

