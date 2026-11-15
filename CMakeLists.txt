cmake_minimum_required(VERSION 3.28)

project(
  iris
  VERSION 0.1.0
  DESCRIPTION "Cross-platform SIP video conference core"
  LANGUAGES C CXX
)

option(IRIS_BUILD_CLI "Build the iris command line client" ON)
option(IRIS_BUILD_TESTS "Build iris tests" ON)

if(MSVC)
  add_compile_options("$<$<COMPILE_LANGUAGE:C>:/utf-8>")
  add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/utf-8>")
endif()

# pjproject has first-class CMake targets in the pinned fork. Keep its sample
# applications and tests out of the application build.
set(PJ_WITH_CXX ON CACHE BOOL "" FORCE)
set(PJ_BUILD_APPS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(PJ_SKIP_EXPERIMENTAL_NOTICE ON CACHE BOOL "" FORCE)

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/pjproject/CMakeLists.txt")
  message(FATAL_ERROR
    "third_party/pjproject is missing. Run:\n"
    "  git submodule update --init --recursive"
  )
endif()

add_subdirectory(third_party/pjproject EXCLUDE_FROM_ALL)

add_library(iris STATIC
  src/api/runtime.cpp
  src/core/account_manager.cpp
  src/core/call_manager.cpp
  src/core/runtime_impl.cpp
  src/core/task_dispatcher.cpp
  src/sip/pj_account.cpp
  src/sip/pj_call.cpp
  src/sip/pj_endpoint.cpp
  src/sip/state_mapper.cpp
)

add_library(iris::iris ALIAS iris)

target_compile_features(iris PUBLIC cxx_std_17)
target_include_directories(iris
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_definitions(iris
  PRIVATE
    IRIS_VERSION="${PROJECT_VERSION}"
)
# pjsua2's implementation links the PJMEDIA/PJSIP libraries privately, so a
# static consumer also needs their generated configuration include directories
# while compiling libiris itself.
target_link_libraries(iris PRIVATE
  pjsua2
  pjlib
  pjlib-util
  pjnath
  pjmedia
  pjmedia-codec
  pjmedia-audiodev
  pjmedia-videodev
  pjsip
  pjsip-simple
  pjsip-ua
  pjsua-lib
)

if(IRIS_BUILD_CLI)
  add_executable(iris-cli apps/iris-cli/main.cpp)
  target_compile_features(iris-cli PRIVATE cxx_std_17)
  target_link_libraries(iris-cli PRIVATE iris::iris)
endif()

if(IRIS_BUILD_TESTS)
  enable_testing()
  add_executable(iris-runtime-smoke tests/runtime_smoke.cpp)
  target_compile_features(iris-runtime-smoke PRIVATE cxx_std_17)
  target_link_libraries(iris-runtime-smoke PRIVATE iris::iris)
  add_test(NAME iris-runtime-smoke COMMAND iris-runtime-smoke)
endif()

