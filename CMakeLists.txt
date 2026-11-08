cmake_minimum_required(VERSION 3.22)
project(Silt VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
# The sources are UTF-8 (preset names in French).
if(MSVC)
  add_compile_options(/utf-8)
endif()

option(SILT_BUILD_RENDER "Build the offline render / measurement tool" ON)
option(SILT_BUILD_PLUGIN "Build the plugin (fetches JUCE)" ON)

# DSP core: header-only, no framework dependency, shared by the tool and the plugin.
add_library(silt_dsp INTERFACE)
target_include_directories(silt_dsp INTERFACE dsp/include)

if(SILT_BUILD_RENDER)
  add_executable(silt_render tools/render/main.cpp)
  target_link_libraries(silt_render PRIVATE silt_dsp)
  if(MSVC)
    target_compile_options(silt_render PRIVATE /O2 /fp:fast)
  else()
    target_compile_options(silt_render PRIVATE -Wall -Wextra -O3)
  endif()
endif()

if(SILT_BUILD_PLUGIN)
  add_subdirectory(plugin)
endif()
