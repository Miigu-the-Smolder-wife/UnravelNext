// Tells the D3D12 loader to use the Agility runtime deployed next to the executable (bin/D3D12/D3D12Core.dll).
#include <windows.h>
#include <d3d12.h>

static_assert(D3D12_SDK_VERSION == UNX_AGILITY_SDK_VERSION, "d3d12.h must come from the pinned Agility SDK package");

extern "C" {
__declspec(dllexport) extern const UINT D3D12SDKVersion = UNX_AGILITY_SDK_VERSION;
__declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}
