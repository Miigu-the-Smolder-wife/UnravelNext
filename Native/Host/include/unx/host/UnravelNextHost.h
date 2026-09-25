#pragma once
// UnravelNext.dll C ABI (I track). Unity loads the DLL as a native plugin; C# calls these functions through P/Invoke
// (Assets/UnravelNextBridge in the old repository). Rules:
//   - Only fixed-size structs with a leading {size, version} pair, plain integers/floats, and opaque 64-bit handles cross
//     the boundary. No C++ objects, no STL, no exceptions: every function returns UNX_OK or an error code, and
//     UnxLastError() returns the message of the calling thread's last failure.
//   - A struct whose size or version does not match is refused (UNX_ERROR_ABI), so a stale managed side fails loudly.
//   - Render work happens on Unity's submission thread through the plugin event returned by UnxRenderEventFunc().
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UNX_API __declspec(dllexport)
#define UNX_CALL __cdecl

enum UnxResult
{
    UNX_OK = 1,
    UNX_ERROR = 0,
    UNX_ERROR_ABI = -1,        // size/version mismatch between managed and native structs
    UNX_ERROR_NO_DEVICE = -2,  // Unity's D3D12 device is not available (other graphics API, device lost)
    UNX_ERROR_BUFFER = -3,     // output buffer too small; *required holds the size
};

#define UNX_ABI_VERSION 1u
UNX_API uint32_t UNX_CALL UnxAbiVersion(void);
// Message of the calling thread's last failure (UTF-8, empty when none). Valid until the next failing call.
UNX_API const char* UNX_CALL UnxLastError(void);
// Unity plugin event callback and the first of its reserved event ids (UnxEvent + base).
UNX_API void* UNX_CALL UnxRenderEventFunc(void);
UNX_API int32_t UNX_CALL UnxEventBase(void);

enum UnxEvent
{
    UNX_EVENT_PROBE_BEFORE = 0,  // probe: timestamp on Unity's queue before the frame
    UNX_EVENT_PROBE_WORK = 1,    // probe: the frame workload through the selected boundary
    UNX_EVENT_PROBE_AFTER = 2,   // probe: timestamp on Unity's queue after Unity consumed the output
    UNX_EVENT_PROBE_FACTS = 3,   // probe: collect device/queue facts on the submission thread
    UNX_EVENT_COUNT = 4,
};

// ---- Host boundary probe (ARCHITECTURE 7.1-5) ----------------------------------------------------------------------

enum UnxProbeMode
{
    UNX_PROBE_UNITY_QUEUE = 1,  // frame list executed on Unity's queue (IUnityGraphicsD3D12v8::ExecuteCommandList)
    UNX_PROBE_OWN_QUEUE = 2,    // frame on the renderer's own DIRECT queue of Unity's device, fences to Unity's queue
};

typedef struct UnxProbeSettings
{
    uint32_t size;       // sizeof(UnxProbeSettings)
    uint32_t version;    // 1
    uint32_t mode;       // UnxProbeMode
    uint32_t width, height;
    uint32_t frames;     // measured frames after warm-up
    float warmupSeconds;
    uint32_t raiseUnityQueuePriority;  // 1: ID3D12CommandQueue1::SetProcessPriority(HIGH) on Unity's queue for the run
    uint64_t outputs[2];  // ID3D12Resource* of two Unity RenderTextures (R10G10B10A2, random write), used alternately
    char shaderDirectory[512];  // UTF-8, folder holding Probe/*.dxil
} UnxProbeSettings;

// Facts about Unity's device and queue (JSON). Valid after one UNX_EVENT_PROBE_FACTS event has run.
UNX_API int32_t UNX_CALL UnxProbeFacts(char* json, uint32_t capacity, uint32_t* required);
UNX_API int32_t UNX_CALL UnxProbeStart(const UnxProbeSettings* settings);
// Frames measured so far in the running probe; *finished = 1 when the requested count is reached.
UNX_API int32_t UNX_CALL UnxProbeProgress(uint32_t* measured, uint32_t* finished);
// Result of the finished probe (JSON), then the probe releases its resources (after the GPU is done with them).
UNX_API int32_t UNX_CALL UnxProbeResults(char* json, uint32_t capacity, uint32_t* required);
UNX_API int32_t UNX_CALL UnxProbeStop(void);

#ifdef __cplusplus
}
#endif
