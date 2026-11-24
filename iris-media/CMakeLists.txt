cmake_minimum_required(VERSION 3.21)

project(
  iris-media
  VERSION 0.1.0
  DESCRIPTION "Iris media execution and memory primitives"
  LANGUAGES CXX
)

option(IRIS_MEDIA_BUILD_TESTS "Build iris-media tests" ${PROJECT_IS_TOP_LEVEL})

find_package(Threads REQUIRED)
find_package(Boost CONFIG REQUIRED COMPONENTS container)

add_library(iris-media STATIC
  engine/simple_task_queue.cpp
  engine/memory_pool.cpp
  engine/pmr_memory_pool.cpp
)
add_library(iris::media ALIAS iris-media)

target_compile_features(iris-media PUBLIC cxx_std_17)
target_include_directories(iris-media
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(iris-media
  PUBLIC Threads::Threads
  PRIVATE Boost::container
)

if(MSVC)
  target_compile_options(iris-media PRIVATE /W4 /permissive-)
else()
  target_compile_options(iris-media PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(IRIS_MEDIA_BUILD_TESTS)
  enable_testing()

  add_executable(iris-media-engine-test tests/engine_test.cpp)
  target_compile_features(iris-media-engine-test PRIVATE cxx_std_17)
  target_link_libraries(iris-media-engine-test PRIVATE iris::media)

  if(WIN32)
    add_custom_command(TARGET iris-media-engine-test POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:Boost::container>"
        "$<TARGET_FILE_DIR:iris-media-engine-test>"
      VERBATIM
    )
  endif()

  add_test(NAME iris-media-engine-test COMMAND iris-media-engine-test)
endif()
