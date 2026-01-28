cmake_minimum_required(VERSION 3.20)

project(R3000_Emu LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(r3000_emu
  src/main.cpp
)

add_library(r3000_core
  src/audio/spu.cpp
  src/audio/spu_voice.cpp
  src/audio/xa_decoder.cpp
  src/audio/wav_writer.cpp
  src/cdrom/cdrom.cpp
  src/emu/core.cpp
  src/gpu/gpu.cpp
  src/log/filelog.cpp
  src/log/logger.cpp
  src/gte/gte.cpp
  src/loader/loader.cpp
  src/r3000/bus.cpp
  src/r3000/cpu.cpp
)

target_include_directories(r3000_core PUBLIC src)
target_include_directories(r3000_emu PRIVATE src)
target_link_libraries(r3000_emu PRIVATE r3000_core)

