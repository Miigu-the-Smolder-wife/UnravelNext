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

// Device removal (TDR: DEVICE_HUNG, REMOVED, RESET): nothing in the process can continue on the device, and the run must
// not look like an ordinary failure. deviceRemoved prints "UNX_DEVICE_REMOVED <what> hr 0x... reason 0x..." as the last
// line and exits with kDeviceRemovedExitCode at once (no static destructors touch the dead device); GpuLock.ps1 logs
// that exit as DEVICE_REMOVED. check() and Queue::waitCpu (a removed device's fences read UINT64_MAX) route here.
constexpr int kDeviceRemovedExitCode = 87;
[[noreturn]] void deviceRemoved(const char* what, HRESULT hr);  // Device.cpp
inline bool isDeviceRemoved(HRESULT hr)
{
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_HUNG || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

inline void check(HRESULT hr, const char* what)
{
    if (FAILED(hr))
    {
        if (isDeviceRemoved(hr)) deviceRemoved(what, hr);
        fail("%s failed: 0x%08X", what, (unsigned)hr);
    }
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
