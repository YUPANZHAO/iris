cmake_minimum_required(VERSION 3.21)

project(
  iris-media
  VERSION 0.1.0
  DESCRIPTION "Iris media execution and memory primitives"
  LANGUAGES CXX
)

option(IRIS_MEDIA_BUILD_TESTS "Build iris-media tests" ${PROJECT_IS_TOP_LEVEL})
option(IRIS_MEDIA_BUILD_CODECS "Build FFmpeg codec implementations" ON)

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

if(IRIS_MEDIA_BUILD_CODECS)
  set(_iris_media_module_path "${CMAKE_MODULE_PATH}")
  list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
  find_package(FFMPEG REQUIRED)
  set(CMAKE_MODULE_PATH "${_iris_media_module_path}")
  unset(_iris_media_module_path)

  add_library(iris-media-codec STATIC
    codec/ffmpeg_codec_factory.cpp
    codec/ffmpeg_h264_decoder.cpp
    codec/ffmpeg_h264_encoder.cpp
    codec/ffmpeg_opus_decoder.cpp
    codec/ffmpeg_opus_encoder.cpp
  )
  add_library(iris::media::codec ALIAS iris-media-codec)

  target_compile_features(iris-media-codec PUBLIC cxx_std_17)
  target_include_directories(iris-media-codec
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
      $<INSTALL_INTERFACE:include>
  )
  target_include_directories(
    iris-media-codec SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIRS})
  target_link_libraries(
    iris-media-codec
    PUBLIC iris::media
    PRIVATE ${FFMPEG_LIBRARIES}
  )

  if(MSVC)
    target_compile_options(
      iris-media-codec PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(
      iris-media-codec PRIVATE -Wall -Wextra -Wpedantic)
  endif()
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

  if(IRIS_MEDIA_BUILD_CODECS)
    add_executable(iris-media-codec-test tests/codec_test.cpp)
    target_compile_features(iris-media-codec-test PRIVATE cxx_std_17)
    target_link_libraries(
      iris-media-codec-test PRIVATE iris::media::codec)

    if(WIN32)
      add_custom_command(TARGET iris-media-codec-test POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "$<TARGET_FILE:Boost::container>"
          "$<TARGET_FILE_DIR:iris-media-codec-test>"
        VERBATIM
      )

      if(DEFINED VCPKG_INSTALLED_DIR)
        set(_iris_media_runtime_dir
          "${VCPKG_INSTALLED_DIR}/x64-windows/bin")
      else()
        get_filename_component(
          _iris_media_ffmpeg_prefix "${FFMPEG_INCLUDE_DIR}" DIRECTORY)
        set(_iris_media_runtime_dir
          "${_iris_media_ffmpeg_prefix}/bin")
      endif()
      file(GLOB _iris_media_runtime_dlls
        "${_iris_media_runtime_dir}/avcodec-*.dll"
        "${_iris_media_runtime_dir}/avutil-*.dll"
        "${_iris_media_runtime_dir}/swresample-*.dll"
        "${_iris_media_runtime_dir}/swscale-*.dll"
        "${_iris_media_runtime_dir}/openh264-*.dll"
        "${_iris_media_runtime_dir}/opus.dll"
      )
      if(_iris_media_runtime_dlls)
        add_custom_command(TARGET iris-media-codec-test POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${_iris_media_runtime_dlls}
            $<TARGET_FILE_DIR:iris-media-codec-test>
          VERBATIM
        )
      endif()
      unset(_iris_media_runtime_dir)
      unset(_iris_media_ffmpeg_prefix)
    endif()

    add_test(NAME iris-media-codec-test COMMAND iris-media-codec-test)
  endif()
endif()
