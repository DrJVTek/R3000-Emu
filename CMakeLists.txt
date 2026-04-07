cmake_minimum_required(VERSION 3.20)

project(R3000_Emu LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Output to lib/<Config>/
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_DEBUG ${CMAKE_SOURCE_DIR}/lib/Debug)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR}/lib/Release)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_SOURCE_DIR}/lib/RelWithDebInfo)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_SOURCE_DIR}/lib/Debug)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR}/lib/Release)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_SOURCE_DIR}/lib/RelWithDebInfo)

# Emulator core sources (shared between CLI and UE5 via symlink)
set(CORE_SOURCES
  src/audio/spu.cpp
  src/audio/spu_voice.cpp
  src/audio/xa_decoder.cpp
  src/audio/wav_writer.cpp
  src/cdrom/cdrom.cpp
  src/debug/debug_server.cpp
  src/emu/core.cpp
  src/emu/mcp_server.cpp
  src/emu/provenance_hotspot_profiler.cpp
  src/emu/psx3d_profile_store.cpp
  src/emu/psx3d_mode_manager.cpp
  src/gpu/gpu.cpp
  src/gpu/gpu_3d.cpp
  src/gpu/gte_correlation.cpp
  src/log/async_log.cpp
  src/log/emu_log.cpp
  src/log/filelog.cpp
  src/log/logger.cpp
  src/gte/gte.cpp
  src/gte/gte_3d.cpp
  src/loader/loader.cpp
  src/mdec/mdec.cpp
  src/r3000/bus.cpp
  src/r3000/cpu.cpp
  src/r3000/cpu_provenance_analyzer.cpp
)

# CLI emulator — links core sources directly (no separate library needed)
add_executable(r3000_emu
  cli/main.cpp
  ${CORE_SOURCES}
)

target_compile_definitions(r3000_emu PRIVATE R3000_DBG_LOOP_DETECTORS=1)
target_include_directories(r3000_emu PRIVATE src)

if(MSVC)
    target_compile_options(r3000_emu PRIVATE /EHs-c- /GR-)
    set_property(TARGET r3000_emu PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

# Winsock2 for debug TCP server
if(WIN32)
    target_link_libraries(r3000_emu PRIVATE ws2_32)
endif()

# MCP bridge — standalone executable for Claude Code integration
add_executable(r3000_mcp cli/r3000_mcp.cpp)
if(WIN32)
    target_link_libraries(r3000_mcp PRIVATE ws2_32)
endif()
