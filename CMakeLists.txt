cmake_minimum_required(VERSION 3.28)

project(
  iris
  VERSION 0.1.0
  DESCRIPTION "Cross-platform SIP video conference core"
  LANGUAGES C CXX
)

option(IRIS_BUILD_CLI "Build the iris command line client" ON)
option(IRIS_BUILD_TESTS "Build iris tests" ON)
option(IRIS_WITH_MEDIA "Build the independent FFmpeg media plane" ON)
option(IRIS_BUILD_UI_BRIDGE "Build the NSH C ABI bridge" ON)

# pjproject has first-class CMake targets in the pinned fork. Keep its sample
# applications and tests out of the application build.
set(PJ_WITH_CXX ON CACHE BOOL "" FORCE)
set(PJ_BUILD_APPS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(PJ_SKIP_EXPERIMENTAL_NOTICE ON CACHE BOOL "" FORCE)
set(PJMEDIA_WITH_FFMPEG OFF CACHE BOOL "" FORCE)
set(PJMEDIA_WITH_VIDEODEV_FFMPEG OFF CACHE BOOL "" FORCE)

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/pjproject/CMakeLists.txt")
  message(FATAL_ERROR
    "third_party/pjproject is missing. Run:\n"
    "  git submodule update --init --recursive"
  )
endif()

# External endpoints can send UDP INVITEs whose SDP and SIP headers together
# exceed the default 4000-byte buffer. Keep the receive-data layout consistent
# across pjproject and all Iris targets.
add_compile_definitions(PJSIP_MAX_PKT_LEN=8192)
add_subdirectory(third_party/pjproject EXCLUDE_FROM_ALL)

if(MSVC)
  add_compile_options("$<$<COMPILE_LANGUAGE:C>:/utf-8>")
  add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/utf-8>")
endif()

if(IRIS_WITH_MEDIA)
  find_package(FFMPEG REQUIRED)
  find_package(OpenSSL REQUIRED)
endif()
if(IRIS_BUILD_UI_BRIDGE)
  find_package(nlohmann_json CONFIG REQUIRED)
endif()

add_library(iris STATIC
  src/api/runtime.cpp
  src/core/account_manager.cpp
  src/core/call_manager.cpp
  src/core/runtime_impl.cpp
  src/core/task_dispatcher.cpp
  src/media/backend/ffmpeg/capture_sources.cpp
  src/media/backend/ffmpeg/ffmpeg_audio_pipeline.cpp
  src/media/backend/ffmpeg/ffmpeg_media_backend.cpp
  src/media/backend/ffmpeg/ffmpeg_video_pipeline.cpp
  src/media/backend/ffmpeg/null_video_image.cpp
  src/media/engine/media_engine.cpp
  src/media/render/native_renderer.cpp
  src/media/sdp/codec_registry.cpp
  src/media/sdp/sdp_controller.cpp
  src/media/transport/pj_ioqueue_service.cpp
  src/media/transport/pj_ioqueue_udp_transport.cpp
  src/media/transport/rtp.cpp
  src/sip/pj_account.cpp
  src/sip/pj_call.cpp
  src/sip/pj_endpoint.cpp
  src/sip/sip_message.cpp
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

if(IRIS_WITH_MEDIA)
  target_compile_definitions(iris PRIVATE IRIS_HAS_MEDIA=1)
  target_include_directories(iris PRIVATE ${FFMPEG_INCLUDE_DIRS})
  target_link_libraries(iris PRIVATE ${FFMPEG_LIBRARIES} OpenSSL::SSL OpenSSL::Crypto)
endif()

if(WIN32)
  target_link_libraries(iris PRIVATE winmm gdi32 user32 ws2_32)
endif()
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

if(WIN32 AND IRIS_WITH_MEDIA)
  set(_iris_runtime_dll_dir "${VCPKG_INSTALLED_DIR}/x64-windows/bin")
  file(GLOB _iris_runtime_dlls
    "${_iris_runtime_dll_dir}/avcodec-*.dll"
    "${_iris_runtime_dll_dir}/avdevice-*.dll"
    "${_iris_runtime_dll_dir}/avfilter-*.dll"
    "${_iris_runtime_dll_dir}/avformat-*.dll"
    "${_iris_runtime_dll_dir}/avutil-*.dll"
    "${_iris_runtime_dll_dir}/swresample-*.dll"
    "${_iris_runtime_dll_dir}/swscale-*.dll"
    "${_iris_runtime_dll_dir}/libssl-*.dll"
    "${_iris_runtime_dll_dir}/libcrypto-*.dll"
    "${_iris_runtime_dll_dir}/openh264-*.dll"
    "${_iris_runtime_dll_dir}/opus.dll"
  )
  function(iris_copy_vcpkg_runtime target)
    if(_iris_runtime_dlls)
      add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${_iris_runtime_dlls}
          $<TARGET_FILE_DIR:${target}>
      )
    endif()
  endfunction()
endif()

if(IRIS_BUILD_UI_BRIDGE)
  add_library(iris_bridge SHARED src/ffi/c_api.cpp)
  add_library(iris::bridge ALIAS iris_bridge)
  target_compile_features(iris_bridge PRIVATE cxx_std_17)
  target_compile_definitions(iris_bridge PRIVATE IRIS_BRIDGE_BUILD=1)
  target_include_directories(iris_bridge PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(iris_bridge PRIVATE
    iris::iris
    nlohmann_json::nlohmann_json
  )
  if(WIN32 AND IRIS_WITH_MEDIA)
    iris_copy_vcpkg_runtime(iris_bridge)
  endif()
endif()

if(IRIS_BUILD_CLI)
  add_executable(iris-cli apps/iris-cli/main.cpp)
  target_compile_features(iris-cli PRIVATE cxx_std_17)
  target_link_libraries(iris-cli PRIVATE iris::iris)
  if(WIN32 AND CMAKE_CONFIGURATION_TYPES)
    set_target_properties(iris-cli PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}")
  endif()
  if(WIN32 AND IRIS_WITH_MEDIA)
    iris_copy_vcpkg_runtime(iris-cli)
  endif()
endif()
if(IRIS_BUILD_TESTS)
  enable_testing()
  add_executable(iris-runtime-smoke tests/runtime_smoke.cpp)
  target_compile_features(iris-runtime-smoke PRIVATE cxx_std_17)
  target_link_libraries(iris-runtime-smoke PRIVATE iris::iris)
  if(WIN32 AND IRIS_WITH_MEDIA)
    iris_copy_vcpkg_runtime(iris-runtime-smoke)
  endif()
  add_test(NAME iris-runtime-smoke COMMAND iris-runtime-smoke)

  if(IRIS_BUILD_UI_BRIDGE)
    add_executable(iris-c-api-test tests/c_api_test.cpp)
    target_compile_features(iris-c-api-test PRIVATE cxx_std_17)
    target_include_directories(iris-c-api-test PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(iris-c-api-test PRIVATE iris::bridge)
    if(WIN32 AND IRIS_WITH_MEDIA)
      iris_copy_vcpkg_runtime(iris-c-api-test)
    endif()
    add_test(NAME iris-c-api-test COMMAND iris-c-api-test)
  endif()

  add_executable(iris-media-interface-compile
    tests/media_interface_compile.cpp
  )
  target_compile_features(iris-media-interface-compile PRIVATE cxx_std_17)
  target_include_directories(iris-media-interface-compile PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  add_test(NAME iris-media-interface-compile
    COMMAND iris-media-interface-compile)

  add_executable(iris-media-sdp-test
    tests/sdp_controller_test.cpp
  )
  target_compile_features(iris-media-sdp-test PRIVATE cxx_std_17)
  target_include_directories(iris-media-sdp-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(iris-media-sdp-test PRIVATE iris::iris pjlib)
  add_test(NAME iris-media-sdp-test COMMAND iris-media-sdp-test)

  add_executable(iris-media-sources-test tests/media_sources_test.cpp)
  target_compile_features(iris-media-sources-test PRIVATE cxx_std_17)
  target_include_directories(iris-media-sources-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(iris-media-sources-test PRIVATE iris::iris)
  if(WIN32 AND IRIS_WITH_MEDIA)
    iris_copy_vcpkg_runtime(iris-media-sources-test)
  endif()
  add_test(NAME iris-media-sources-test
    COMMAND iris-media-sources-test)

  add_executable(iris-pj-ioqueue-transport-test
    tests/pj_ioqueue_transport_test.cpp
  )
  target_compile_features(
    iris-pj-ioqueue-transport-test PRIVATE cxx_std_17)
  target_include_directories(
    iris-pj-ioqueue-transport-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(
    iris-pj-ioqueue-transport-test PRIVATE iris::iris pjlib)
  if(WIN32 AND IRIS_WITH_MEDIA)
    iris_copy_vcpkg_runtime(iris-pj-ioqueue-transport-test)
  endif()
  add_test(NAME iris-pj-ioqueue-transport-test
    COMMAND iris-pj-ioqueue-transport-test)

  if(IRIS_WITH_MEDIA)
    add_executable(iris-media-loopback tests/media_loopback_test.cpp)
    target_compile_features(iris-media-loopback PRIVATE cxx_std_17)
    target_include_directories(iris-media-loopback PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/include
      ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(iris-media-loopback PRIVATE iris::iris pjlib)
    if(WIN32)
      iris_copy_vcpkg_runtime(iris-media-loopback)
    endif()
    add_test(NAME iris-media-loopback COMMAND iris-media-loopback)
    set_tests_properties(iris-media-loopback PROPERTIES TIMEOUT 20)
    add_test(NAME iris-media-null-loopback
      COMMAND iris-media-loopback --null-input)
    set_tests_properties(iris-media-null-loopback PROPERTIES TIMEOUT 20)
    add_test(NAME iris-media-stap-a-loopback
      COMMAND iris-media-loopback --stap-a)
    set_tests_properties(iris-media-stap-a-loopback PROPERTIES TIMEOUT 10)
  endif()

  if(WIN32 AND IRIS_BUILD_CLI AND IRIS_WITH_MEDIA)
    find_program(IRIS_POWERSHELL_EXECUTABLE NAMES powershell.exe)
    if(IRIS_POWERSHELL_EXECUTABLE)
      add_test(NAME iris-sip-e2e
        COMMAND "${IRIS_POWERSHELL_EXECUTABLE}"
          -NoProfile
          -ExecutionPolicy Bypass
          -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/sip_e2e_test.ps1"
          -CliPath "$<TARGET_FILE:iris-cli>")
      set_tests_properties(iris-sip-e2e PROPERTIES
        TIMEOUT 30
        LABELS integration)
    endif()
  endif()
endif()
