cmake_minimum_required(VERSION 3.20)

project(R3000_Emu LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Output lib + DLL to lib/<Config>/ for UE5 linking
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_DEBUG ${CMAKE_SOURCE_DIR}/lib/Debug)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR}/lib/Release)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_SOURCE_DIR}/lib/RelWithDebInfo)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_SOURCE_DIR}/lib/Debug)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR}/lib/Release)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_SOURCE_DIR}/lib/RelWithDebInfo)

add_executable(r3000_emu
  src/main.cpp
)

# Shared library (DLL) — auto-export all symbols on Windows
set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)

add_library(r3000_core SHARED
  src/audio/spu.cpp
  src/audio/spu_voice.cpp
  src/audio/xa_decoder.cpp
  src/audio/wav_writer.cpp
  src/cdrom/cdrom.cpp
  src/emu/core.cpp
  src/gpu/gpu.cpp
  src/gpu/gte_correlation.cpp
  src/log/emu_log.cpp
  src/log/filelog.cpp
  src/log/logger.cpp
  src/gte/gte.cpp
  src/loader/loader.cpp
  src/r3000/bus.cpp
  src/r3000/cpu.cpp
)

target_compile_definitions(r3000_core PUBLIC R3000_DBG_LOOP_DETECTORS=1)
target_include_directories(r3000_core PUBLIC src)
target_include_directories(r3000_emu PRIVATE src)
target_link_libraries(r3000_emu PRIVATE r3000_core)

# Match UE5 compilation flags
if(MSVC)
    target_compile_options(r3000_core PRIVATE /EHs-c- /GR-)
    set_property(TARGET r3000_core PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    set_property(TARGET r3000_emu PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()
