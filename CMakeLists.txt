cmake_minimum_required(VERSION 3.16)
project(optimized_limit_order_book CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(LOB_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)

add_library(lob INTERFACE)
target_include_directories(lob INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

function(lob_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endfunction()

# ---- tests ------------------------------------------------------------------
enable_testing()
add_executable(test_lob tests/test_lob.cpp)
target_link_libraries(test_lob PRIVATE lob)
lob_warnings(test_lob)
add_test(NAME test_lob COMMAND test_lob)

if(LOB_SANITIZE)
  if(MSVC)
    message(FATAL_ERROR "LOB_SANITIZE needs GCC or Clang")
  endif()
  target_compile_options(test_lob PRIVATE -fsanitize=address,undefined
                         -fno-sanitize-recover=undefined -fno-omit-frame-pointer -g -O1)
  target_link_options(test_lob PRIVATE -fsanitize=address,undefined)
endif()

# ---- benchmark (always optimized, even in a Debug configuration) ------------
add_executable(bench_lob bench/bench_lob.cpp)
target_link_libraries(bench_lob PRIVATE lob)
lob_warnings(bench_lob)
if(MSVC)
  target_compile_options(bench_lob PRIVATE /O2)
else()
  target_compile_options(bench_lob PRIVATE -O3 -DNDEBUG)
endif()
