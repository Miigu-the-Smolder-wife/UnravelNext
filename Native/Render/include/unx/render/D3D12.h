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
// not look like an ordinary failure. The policy (v1.26, I request):
//   Exit (default; tests, gates, tools): "UNX_DEVICE_REMOVED <what> hr 0x... reason 0x..." as the last line of stdout
//        and exit code kDeviceRemovedExitCode at once (no static destructors touch the dead device); GpuLock.ps1 logs
//        that exit as DEVICE_REMOVED.
//   Throw (a host process: the Unity editor or player must survive): the same line to the log and a DeviceRemovedError
//        from check(). Queue::signal / waitCpu and Device::waitIdle (also reached from destructors) never throw: they
//        note the removal and return (a removed device has nothing left to wait for); deviceWasRemoved() tells.
// The host sets Throw before creating its device; a first device created on a host's device (externalDevice) also
// selects Throw unless a policy was set explicitly.
constexpr int kDeviceRemovedExitCode = 87;
enum class DeviceRemovedPolicy
{
    Exit,
    Throw,
};
void setDeviceRemovedPolicy(DeviceRemovedPolicy policy);
DeviceRemovedPolicy deviceRemovedPolicy();
bool deviceWasRemoved();

struct DeviceRemovedError : Error
{
    DeviceRemovedError(const std::string& message, const char* where, HRESULT hr, HRESULT reason) : Error(message), where(where), hr(hr), reason(reason) {}
    std::string where;
    HRESULT hr, reason;
};

[[noreturn]] void deviceRemoved(const char* what, HRESULT hr);  // Exit: exits; Throw: throws DeviceRemovedError
// Exit: exits; Throw: notes the removal once and returns (paths that must not throw: signal, waits, destructors).
void noteDeviceRemoved(const char* what, HRESULT hr);
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
