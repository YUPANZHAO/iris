#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  if defined(IRIS_BRIDGE_BUILD)
#    define IRIS_BRIDGE_API __declspec(dllexport)
#  else
#    define IRIS_BRIDGE_API __declspec(dllimport)
#  endif
#else
#  define IRIS_BRIDGE_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct iris_bridge_runtime iris_bridge_runtime;

typedef enum iris_bridge_video_frame_kind {
    IRIS_BRIDGE_VIDEO_REMOTE = 0,
    IRIS_BRIDGE_VIDEO_LOCAL_PREVIEW = 1,
} iris_bridge_video_frame_kind;

typedef struct iris_bridge_video_frame {
    uint32_t struct_size;
    iris_bridge_video_frame_kind kind;
    const char* call_id;
    const uint8_t* data;
    size_t size;
    int width;
    int height;
    int stride;
    uint64_t timestamp_ms;
} iris_bridge_video_frame;

// Runs on a media capture/decode thread. Copy data before returning.
typedef void (*iris_bridge_video_frame_callback)(
    const iris_bridge_video_frame* frame,
    void* user_data);

// Return 0 on success. Error strings are UTF-8 and must be released with
// iris_bridge_string_free().
IRIS_BRIDGE_API int iris_bridge_runtime_create(
    const char* config_json,
    iris_bridge_runtime** out_runtime,
    char** out_error);

IRIS_BRIDGE_API void iris_bridge_runtime_destroy(
    iris_bridge_runtime* runtime);

// Commands and their results are UTF-8 JSON. See docs/ui-bridge.md.
IRIS_BRIDGE_API int iris_bridge_runtime_command(
    iris_bridge_runtime* runtime,
    const char* command_json,
    char** out_result_json,
    char** out_error);

// Returns a JSON array and consumes all currently queued events.
IRIS_BRIDGE_API int iris_bridge_runtime_poll_events(
    iris_bridge_runtime* runtime,
    char** out_events_json,
    char** out_error);

// Returns accounts, calls and media statistics as one coherent JSON object.
IRIS_BRIDGE_API int iris_bridge_runtime_snapshot(
    iris_bridge_runtime* runtime,
    char** out_snapshot_json,
    char** out_error);

// Pass NULL to remove the sink. Frames are BGRA with top-down rows.
IRIS_BRIDGE_API int iris_bridge_runtime_set_video_frame_sink(
    iris_bridge_runtime* runtime,
    iris_bridge_video_frame_callback callback,
    void* user_data,
    char** out_error);

IRIS_BRIDGE_API void iris_bridge_string_free(char* value);

IRIS_BRIDGE_API const char* iris_bridge_version(void);

#ifdef __cplusplus
}
#endif
