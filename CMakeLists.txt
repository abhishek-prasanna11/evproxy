cmake_minimum_required(VERSION 3.20)
project(evproxy CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

# -DEVP_SANITIZE=address|thread  (benchmarks must be built without any sanitizer)
if(EVP_SANITIZE)
  if(EVP_SANITIZE STREQUAL "address")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
  elseif(EVP_SANITIZE STREQUAL "thread")
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=thread)
  else()
    message(FATAL_ERROR "EVP_SANITIZE must be 'address' or 'thread'")
  endif()
endif()

add_executable(evproxy
  src/main.cpp
  src/config.cpp
  src/socket.cpp
  src/http.cpp
  src/connection.cpp
  src/conn_queue.cpp
  src/backend/factory.cpp
  src/backend/thread_per_conn.cpp
  src/backend/thread_pool.cpp
)

target_include_directories(evproxy PRIVATE src)

find_package(Threads REQUIRED)
target_link_libraries(evproxy PRIVATE Threads::Threads)
