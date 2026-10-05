cmake_minimum_required(VERSION 3.16)
project(cpp_concurrency_lab CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)

add_compile_options(-Wall -Wextra -Wpedantic)

# Optional: -DSANITIZE=thread (or address) for TSan/ASan builds
if(SANITIZE)
  add_compile_options(-fsanitize=${SANITIZE} -g)
  add_link_options(-fsanitize=${SANITIZE})
endif()

add_executable(counter 01_counter/main.cpp)
target_link_libraries(counter PRIVATE Threads::Threads)

add_executable(concurrent_queue 02_concurrent_queue/main.cpp)
target_link_libraries(concurrent_queue PRIVATE Threads::Threads)

add_executable(spsc_queue 03_spsc_queue/main.cpp)
target_link_libraries(spsc_queue PRIVATE Threads::Threads)

add_executable(lockfree_stack 04_lockfree_stack/main.cpp)
target_link_libraries(lockfree_stack PRIVATE Threads::Threads)

add_executable(hazard_stack 04b_hazard_pointers/main.cpp)
target_link_libraries(hazard_stack PRIVATE Threads::Threads)
