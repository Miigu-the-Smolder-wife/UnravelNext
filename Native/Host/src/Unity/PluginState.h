#pragma once
// State shared by the plugin's translation units (UnravelNext.dll): error reporting of the C ABI and Unity's D3D12
// interface. Unity's interface is null outside Unity (standalone renderers, tests).
#include "unx/host/UnravelNextHost.h"

#include <cstdint>
#include <string>

struct IUnityGraphicsD3D12v8;

namespace unx::host::plugin
{
// Records the message for UnxLastError (per calling thread), writes it to stderr and the debugger, returns 'code'.
int32_t failWith(const char* message, int32_t code = UNX_ERROR);
const std::string& lastError();
IUnityGraphicsD3D12v8* unityD3D12();
// UNX_EVENT_RENDER on Unity's submission thread (RendererAbi.cpp); the ticket names the renderer and its frame.
void renderEvent(uint64_t ticket);
// Unity's device is going away (shutdown or reset): every renderer bound to it is destroyed now, while the device and
// queue still exist (their destructors wait for the GPU). Managed handles then report "unknown renderer".
void destroyDeviceRenderers();
} // namespace unx::host::plugin
