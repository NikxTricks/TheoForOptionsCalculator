cmake_minimum_required(VERSION 3.20)

project(theo_pricer)
set(CMAKE_VERBOSE_MAKEFILE ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED)
set(CMAKE_EXPORT_COMPILE_COMMANDS true)
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

add_compile_options(-ggdb)
if (${cmake_build_type} MATCHES Release)
  add_compile_options(-O3)
endif()

add_executable(theo_pricer theo_pricer.cpp)
target_link_libraries(theo_pricer pthread)
