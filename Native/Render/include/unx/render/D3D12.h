#pragma once
// Common D3D12 includes. d3d12.h comes from the pinned Agility SDK package (External/Dependencies.cmake).
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include "unx/core/Log.h"

namespace unx::render
{
using Microsoft::WRL::ComPtr;

inline void check(HRESULT hr, const char* what)
{
    if (FAILED(hr)) fail("%s failed: 0x%08X", what, (unsigned)hr);
}

enum class QueueType : uint8_t
{
    Graphics = 0,
    Compute = 1,
    Copy = 2,
};
constexpr uint32_t kQueueTypeCount = 3;
const char* queueName(QueueType q);
} // namespace unx::render
