cmake_minimum_required(VERSION 3.21)

project(
  iris-media
  VERSION 0.1.0
  DESCRIPTION "Iris media execution and memory primitives"
  LANGUAGES CXX
)

option(IRIS_MEDIA_BUILD_TESTS "Build iris-media tests" ${PROJECT_IS_TOP_LEVEL})
option(IRIS_MEDIA_BUILD_CODECS "Build FFmpeg codec implementations" ON)
option(IRIS_MEDIA_BUILD_TRANSPORT "Build RTP transport implementations" ON)
option(
  IRIS_MEDIA_BUILD_CAPTURE_SOURCES
  "Build FFmpeg capture source implementations"
  ON
)

find_package(Threads REQUIRED)
find_package(Boost CONFIG REQUIRED COMPONENTS container)

set(_iris_media_sources
  engine/simple_task_queue.cpp
  engine/memory_pool.cpp
  engine/pmr_memory_pool.cpp
)

if(IRIS_MEDIA_BUILD_CAPTURE_SOURCES)
  list(APPEND _iris_media_sources
    capture_source/ffmpeg_audio_capture_source.cpp
    capture_source/ffmpeg_video_capture_source.cpp
  )
endif()

add_library(iris-media STATIC ${_iris_media_sources})
unset(_iris_media_sources)
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

if(IRIS_MEDIA_BUILD_CODECS OR IRIS_MEDIA_BUILD_CAPTURE_SOURCES)
  set(_iris_media_module_path "${CMAKE_MODULE_PATH}")
  list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
  find_package(FFMPEG REQUIRED)
  set(CMAKE_MODULE_PATH "${_iris_media_module_path}")
  unset(_iris_media_module_path)

  if(IRIS_MEDIA_BUILD_CAPTURE_SOURCES AND
      (NOT FFMPEG_AVFORMAT_LIBRARY OR NOT FFMPEG_AVDEVICE_LIBRARY))
    message(FATAL_ERROR
      "FFmpeg avformat and avdevice libraries are required for capture")
  endif()
endif()

if(IRIS_MEDIA_BUILD_CAPTURE_SOURCES)
  target_include_directories(
    iris-media SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIRS})
  target_link_libraries(iris-media PRIVATE ${FFMPEG_LIBRARIES})
endif()

if(IRIS_MEDIA_BUILD_CODECS)
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

if(IRIS_MEDIA_BUILD_CODECS AND IRIS_MEDIA_BUILD_CAPTURE_SOURCES)
  add_library(iris-media-pipeline STATIC
    pipeline/stream.cpp
    pipeline/capture_stream.cpp
    pipeline/rtp_media_stream.cpp
  )
  add_library(iris::media::pipeline ALIAS iris-media-pipeline)

  target_compile_features(iris-media-pipeline PUBLIC cxx_std_17)
  target_include_directories(iris-media-pipeline
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
      $<INSTALL_INTERFACE:include>
  )
  target_link_libraries(
    iris-media-pipeline
    PUBLIC iris::media::codec
  )

  if(MSVC)
    target_compile_options(
      iris-media-pipeline PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(
      iris-media-pipeline PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endif()

if(IRIS_MEDIA_BUILD_TRANSPORT)
  add_library(iris-media-transport STATIC
    transport/rtp_engine.cpp
    transport/udp_transport.cpp
  )
  add_library(iris::media::transport ALIAS iris-media-transport)

  target_compile_features(iris-media-transport PUBLIC cxx_std_17)
  target_include_directories(iris-media-transport
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
      $<INSTALL_INTERFACE:include>
  )
  target_link_libraries(iris-media-transport PUBLIC iris::media)

  if(WIN32)
    target_link_libraries(iris-media-transport PRIVATE ws2_32)
  endif()

  if(MSVC)
    target_compile_options(
      iris-media-transport PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(
      iris-media-transport PRIVATE -Wall -Wextra -Wpedantic)
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

  if(IRIS_MEDIA_BUILD_CAPTURE_SOURCES)
    add_executable(
      iris-media-capture-source-test
      tests/capture_source_test.cpp
    )
    target_compile_features(
      iris-media-capture-source-test PRIVATE cxx_std_17)
    target_link_libraries(
      iris-media-capture-source-test
      PRIVATE iris::media
    )

    if(WIN32)
      add_custom_command(
        TARGET iris-media-capture-source-test POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "$<TARGET_FILE:Boost::container>"
          "$<TARGET_FILE_DIR:iris-media-capture-source-test>"
        VERBATIM
      )

      if(DEFINED VCPKG_INSTALLED_DIR)
        set(_iris_media_capture_runtime_dir
          "${VCPKG_INSTALLED_DIR}/x64-windows/bin")
      else()
        get_filename_component(
          _iris_media_ffmpeg_prefix "${FFMPEG_INCLUDE_DIR}" DIRECTORY)
        set(_iris_media_capture_runtime_dir
          "${_iris_media_ffmpeg_prefix}/bin")
      endif()
      file(GLOB _iris_media_capture_runtime_dlls
        "${_iris_media_capture_runtime_dir}/avcodec-*.dll"
        "${_iris_media_capture_runtime_dir}/avdevice-*.dll"
        "${_iris_media_capture_runtime_dir}/avfilter-*.dll"
        "${_iris_media_capture_runtime_dir}/avformat-*.dll"
        "${_iris_media_capture_runtime_dir}/avutil-*.dll"
        "${_iris_media_capture_runtime_dir}/swresample-*.dll"
        "${_iris_media_capture_runtime_dir}/swscale-*.dll"
      )
      if(_iris_media_capture_runtime_dlls)
        add_custom_command(
          TARGET iris-media-capture-source-test POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${_iris_media_capture_runtime_dlls}
            $<TARGET_FILE_DIR:iris-media-capture-source-test>
          VERBATIM
        )
      endif()
      unset(_iris_media_capture_runtime_dir)
      unset(_iris_media_ffmpeg_prefix)
    endif()

    add_test(
      NAME iris-media-capture-source-test
      COMMAND iris-media-capture-source-test
    )
    set_tests_properties(
      iris-media-capture-source-test PROPERTIES TIMEOUT 20)
  endif()

  if(IRIS_MEDIA_BUILD_CODECS AND IRIS_MEDIA_BUILD_CAPTURE_SOURCES)
    add_executable(
      iris-media-pipeline-test
      tests/pipeline_test.cpp
    )
    target_compile_features(
      iris-media-pipeline-test PRIVATE cxx_std_17)
    target_link_libraries(
      iris-media-pipeline-test
      PRIVATE iris::media::pipeline
    )

    if(WIN32)
      add_custom_command(
        TARGET iris-media-pipeline-test POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "$<TARGET_FILE:Boost::container>"
          "$<TARGET_FILE_DIR:iris-media-pipeline-test>"
        VERBATIM
      )
    endif()

    add_test(
      NAME iris-media-pipeline-test
      COMMAND iris-media-pipeline-test
    )
  endif()

  if(IRIS_MEDIA_BUILD_TRANSPORT)
    add_executable(
      iris-media-transport-test
      tests/transport_test.cpp
    )
    target_compile_features(
      iris-media-transport-test PRIVATE cxx_std_17)
    target_link_libraries(
      iris-media-transport-test
      PRIVATE iris::media::transport
    )

    if(WIN32)
      add_custom_command(
        TARGET iris-media-transport-test POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "$<TARGET_FILE:Boost::container>"
          "$<TARGET_FILE_DIR:iris-media-transport-test>"
        VERBATIM
      )
    endif()

    add_test(
      NAME iris-media-transport-test
      COMMAND iris-media-transport-test
    )
  endif()
endif()
