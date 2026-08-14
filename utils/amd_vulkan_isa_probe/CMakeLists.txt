cmake_minimum_required(VERSION 3.21)
project(amd_vulkan_isa_probe LANGUAGES CXX)

find_package(Vulkan REQUIRED)

if(WIN32 AND NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "amd_vulkan_isa_probe requires a 64-bit build")
endif()

add_executable(amd_vulkan_isa_probe main.cpp)
target_compile_features(amd_vulkan_isa_probe PRIVATE cxx_std_23)
target_link_libraries(amd_vulkan_isa_probe PRIVATE Vulkan::Vulkan)

if(MSVC)
  target_compile_options(amd_vulkan_isa_probe PRIVATE /W4 /permissive-)
else()
  target_compile_options(amd_vulkan_isa_probe PRIVATE -Wall -Wextra -Wpedantic)
endif()
