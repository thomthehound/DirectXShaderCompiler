cmake_minimum_required(VERSION 3.21)
project(amd_vulkan_isa_probe LANGUAGES CXX)

find_package(Vulkan REQUIRED)

if(WIN32 AND NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "amd_vulkan_isa_probe requires a 64-bit build")
endif()

add_executable(amd_vulkan_isa_probe main.cpp)
add_executable(amd_vulkan_matrix_probe matrix_probe_main.cpp)
add_executable(amd_vulkan_mixed_dot_probe mixed_dot_probe_main.cpp)
add_executable(amd_vulkan_math_verify math_verify.cpp)
add_executable(amd_vulkan_candidate_verify candidate_verify.cpp)

foreach(target
    amd_vulkan_isa_probe
    amd_vulkan_matrix_probe
    amd_vulkan_mixed_dot_probe
    amd_vulkan_math_verify
    amd_vulkan_candidate_verify)
  target_compile_features(${target} PRIVATE cxx_std_23)
  target_link_libraries(${target} PRIVATE Vulkan::Vulkan)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive-)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endforeach()
