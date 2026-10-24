cmake_minimum_required(VERSION 3.14)
project(lsm_db LANGUAGES CXX)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 14)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# -DLSM_SANITIZE=address   -> AddressSanitizer + UndefinedBehaviorSanitizer
# -DLSM_SANITIZE=thread    -> ThreadSanitizer
set(LSM_SANITIZE "" CACHE STRING "Sanitizer to enable: address | thread")

find_package(Threads REQUIRED)

add_library(lsm INTERFACE)
target_include_directories(lsm INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(lsm INTERFACE Threads::Threads)

if(MSVC)
  target_compile_options(lsm INTERFACE /W4)
else()
  target_compile_options(lsm INTERFACE -Wall -Wextra -Wpedantic)
  if(LSM_SANITIZE STREQUAL "address")
    target_compile_options(lsm INTERFACE -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    target_link_options(lsm INTERFACE -fsanitize=address,undefined)
  elseif(LSM_SANITIZE STREQUAL "thread")
    target_compile_options(lsm INTERFACE -fsanitize=thread)
    target_link_options(lsm INTERFACE -fsanitize=thread)
  endif()
endif()

add_executable(lsm_db src/main.cpp)
target_link_libraries(lsm_db PRIVATE lsm)

enable_testing()
add_executable(test_lsm tests/test_lsm.cpp)
target_link_libraries(test_lsm PRIVATE lsm)
add_test(NAME test_lsm COMMAND test_lsm WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
