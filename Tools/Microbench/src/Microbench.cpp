// UnravelNext hardware-floor microbench (D3D12, RTX 4080).
// Measures the numbers every cost model in Docs/Design/ARCHITECTURE_KO.md rests on:
// compute FMA rate, DRAM/L2 bandwidth, random-access rate and latency, texture sampling rate, atomics,
// dispatch+barrier overhead, mesh-shader triangle throughput at several triangle sizes, fill/early-z rate,
// ray throughput (inline RayQuery and DispatchRays) for opaque / alpha-tested / opacity-micromap foliage
// on a 100k-tree (+1M grass) scene, acceleration-structure build/refit times, PSO compile time versus
// DXIL size, and async-compute overlap. Every number printed is a GPU timestamp measurement (median of
// repeated runs) unless the row says "cpu".
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <dxcapi.h>
#include <nvapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

static_assert(D3D12_SDK_VERSION == UNX_AGILITY_SDK_VERSION, "d3d12.h must come from the pinned Agility SDK package");

using Microsoft::WRL::ComPtr;
extern "C" {
__declspec(dllexport) extern const UINT D3D12SDKVersion = UNX_AGILITY_SDK_VERSION;
__declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}

// ------------------------------------------------------------------------------------------ utilities
static std::string g_log;
static void logf(const char* fmt, ...)
{
    char buf[4096];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    fputs(buf, stdout); fflush(stdout);
    g_log += buf;
}
struct Failure { std::string what; };
static void check(HRESULT hr, const char* what)
{
    if (FAILED(hr)) { char b[256]; snprintf(b, sizeof b, "%s failed: 0x%08X", what, (unsigned)hr); throw Failure{ b }; }
}
static std::string readFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) throw Failure{ "cannot read " + path };
    std::stringstream ss; ss << f.rdbuf(); return ss.str();
}
static double now()
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}
static float asf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t asu(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

static bool g_nvapi = false, g_nvOmm = false, g_nvSer = false, g_useNvOmm = false;
static UINT g_nvSlotUav = 0;
static bool g_thinFoliage = false;

struct Result { std::string section, name; double value; std::string unit; std::string note; };
static std::vector<Result> g_results;
static void record(const std::string& section, const std::string& name, double value, const std::string& unit, const std::string& note = "")
{
    g_results.push_back({ section, name, value, unit, note });
    logf("  [%s] %-58s %12.4f %-10s %s\n", section.c_str(), name.c_str(), value, unit.c_str(), note.c_str());
}

// ------------------------------------------------------------------------------------------ DXC
struct Dxc
{
    HMODULE lib = nullptr;
    ComPtr<IDxcUtils> utils;
    ComPtr<IDxcCompiler3> compiler;
    ComPtr<IDxcIncludeHandler> includeHandler;
    std::string dir; std::wstring includeDir;
    void load(const std::string& d)
    {
        dir = d;
        SetDllDirectoryA(d.c_str());
        lib = LoadLibraryA((d + "\\dxcompiler.dll").c_str());
        if (!lib) throw Failure{ "cannot load dxcompiler.dll from " + d };
        auto create = (DxcCreateInstanceProc)GetProcAddress(lib, "DxcCreateInstance");
        check(create(CLSID_DxcUtils, IID_PPV_ARGS(&utils)), "DxcUtils");
        check(create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)), "DxcCompiler");
        check(utils->CreateDefaultIncludeHandler(&includeHandler), "include handler");
    }
    ComPtr<IDxcBlob> compile(const std::string& source, const wchar_t* entry, const wchar_t* target,
                             const std::vector<std::wstring>& defines, double* msOut = nullptr, bool quiet = false)
    {
        DxcBuffer src{ source.data(), source.size(), DXC_CP_UTF8 };
        std::vector<std::wstring> owned;
        std::vector<LPCWSTR> args;
        args.push_back(L"-T"); args.push_back(target);
        if (entry) { args.push_back(L"-E"); args.push_back(entry); }
        args.push_back(L"-O3"); args.push_back(L"-HV"); args.push_back(L"2021");
        args.push_back(L"-Qstrip_reflect"); args.push_back(L"-Qstrip_debug");
        if (!includeDir.empty()) { args.push_back(L"-I"); args.push_back(includeDir.c_str()); }
        for (auto& d : defines) { if (d == L"__ENABLE16=1") { args.push_back(L"-enable-16bit-types"); continue; } owned.push_back(L"-D" + d); }
        for (auto& d : owned) args.push_back(d.c_str());
        double t0 = now();
        ComPtr<IDxcResult> result;
        check(compiler->Compile(&src, args.data(), (UINT)args.size(), includeHandler.Get(), IID_PPV_ARGS(&result)), "Compile");
        HRESULT status; result->GetStatus(&status);
        ComPtr<IDxcBlobUtf8> errors; result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
        if (FAILED(status))
        {
            std::string msg = errors && errors->GetStringLength() ? errors->GetStringPointer() : "no diagnostics";
            throw Failure{ std::string("shader compile failed: ") + msg };
        }
        if (!quiet && errors && errors->GetStringLength() > 0) logf("dxc: %s\n", errors->GetStringPointer());
        ComPtr<IDxcBlob> blob; result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&blob), nullptr);
        if (msOut) *msOut = now() - t0;
        return blob;
    }
};

// ------------------------------------------------------------------------------------------ GPU context
struct Stat { double median = 0, min = 0, max = 0; int reps = 0; };

struct Gpu
{
    ComPtr<IDXGIFactory6> factory;
    ComPtr<IDXGIAdapter4> adapter;
    ComPtr<ID3D12Device5> dev;
    ComPtr<ID3D12CommandQueue> direct, compute;
    ComPtr<ID3D12CommandAllocator> allocDirect, allocCompute;
    ComPtr<ID3D12GraphicsCommandList6> listDirect, listCompute;
    ComPtr<ID3D12Fence> fenceDirect, fenceCompute;
    UINT64 fenceValueDirect = 0, fenceValueCompute = 0;
    HANDLE event = nullptr;
    ComPtr<ID3D12DescriptorHeap> heapCbv, heapSampler, heapRtv, heapDsv;
    UINT incCbv = 0, incRtv = 0, incDsv = 0;
    UINT nextCbv = 0, nextRtv = 0, nextDsv = 0;
    ComPtr<ID3D12QueryHeap> queryHeap;
    ComPtr<ID3D12Resource> queryReadback;
    UINT64 timestampFrequency = 0;
    ComPtr<ID3D12RootSignature> rootSig;
    std::string adapterName; UINT64 vram = 0; std::string driverVersion, runtimePath;
    D3D12_RAYTRACING_TIER rtTier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
    D3D12_MESH_SHADER_TIER msTier = D3D12_MESH_SHADER_TIER_NOT_SUPPORTED;
    D3D_SHADER_MODEL shaderModel = D3D_SHADER_MODEL_5_1;
    int reps = 9, warm = 3;

    void init()
    {
        check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
        for (UINT i = 0;; ++i)
        {
            ComPtr<IDXGIAdapter4> a;
            if (FAILED(factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&a)))) break;
            DXGI_ADAPTER_DESC3 d; a->GetDesc3(&d);
            if (d.Flags & DXGI_ADAPTER_FLAG3_SOFTWARE) continue;
            if (SUCCEEDED(D3D12CreateDevice(a.Get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&dev))))
            {
                adapter = a; char name[256]; wcstombs(name, d.Description, 255); adapterName = name; vram = d.DedicatedVideoMemory;
                LARGE_INTEGER umd{};
                if (SUCCEEDED(a->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd)))
                {
                    char v[64]; snprintf(v, sizeof v, "%u.%u.%u.%u", (unsigned)HIWORD(umd.HighPart), (unsigned)LOWORD(umd.HighPart), (unsigned)HIWORD(umd.LowPart), (unsigned)LOWORD(umd.LowPart));
                    driverVersion = v;
                }
                break;
            }
        }
        if (!dev) throw Failure{ "no D3D12 12_1 adapter" };
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5{}; dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof o5); rtTier = o5.RaytracingTier;
        D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7{}; dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &o7, sizeof o7); msTier = o7.MeshShaderTier;
        D3D12_FEATURE_DATA_SHADER_MODEL sm{ D3D_SHADER_MODEL_6_9 };
        if (FAILED(dev->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof sm))) { sm.HighestShaderModel = D3D_SHADER_MODEL_6_8; dev->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof sm); }
        shaderModel = sm.HighestShaderModel;
        D3D12_FEATURE_DATA_D3D12_OPTIONS21 o21{}; dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS21, &o21, sizeof o21);
        logf("Adapter: %s, VRAM %.1f GB, driver %s\n", adapterName.c_str(), vram / 1073741824.0, driverVersion.c_str());
        { HMODULE core = GetModuleHandleA("D3D12Core.dll"); char path[MAX_PATH] = "(not loaded)"; if (core) GetModuleFileNameA(core, path, MAX_PATH); logf("D3D12Core.dll: %s\n", path); runtimePath = path; }
        logf("Raytracing tier %d (12 = DXR 1.2/OMM), mesh shader tier %d, shader model 0x%x, work graphs tier %d\n", (int)rtTier, (int)msTier, (int)shaderModel, (int)o21.WorkGraphsTier);
        if (NvAPI_Initialize() == NVAPI_OK)
        {
            g_nvapi = true;
            NvAPI_ShortString iface = "?", branch = "?"; NvU32 drv = 0;
            NvAPI_GetInterfaceVersionString(iface); NvAPI_SYS_GetDriverAndBranchVersion(&drv, branch);
            logf("NVAPI: SDK interface %s, driver %u.%02u branch %s\n", iface, drv / 100, drv % 100, branch);
            NVAPI_D3D12_RAYTRACING_OPACITY_MICROMAP_CAPS oc = NVAPI_D3D12_RAYTRACING_OPACITY_MICROMAP_CAP_NONE;
            NVAPI_D3D12_RAYTRACING_THREAD_REORDERING_CAPS tc = NVAPI_D3D12_RAYTRACING_THREAD_REORDERING_CAP_NONE;
            NvAPI_Status s1 = NvAPI_D3D12_GetRaytracingCaps(dev.Get(), NVAPI_D3D12_RAYTRACING_CAPS_TYPE_OPACITY_MICROMAP, &oc, sizeof oc);
            NvAPI_Status s2 = NvAPI_D3D12_GetRaytracingCaps(dev.Get(), NVAPI_D3D12_RAYTRACING_CAPS_TYPE_THREAD_REORDERING, &tc, sizeof tc);
            g_nvOmm = s1 == NVAPI_OK && (oc & NVAPI_D3D12_RAYTRACING_OPACITY_MICROMAP_CAP_STANDARD) != 0;
            g_nvSer = s2 == NVAPI_OK && (tc & NVAPI_D3D12_RAYTRACING_THREAD_REORDERING_CAP_STANDARD) != 0;
            logf("NVAPI: initialized; opacity micromap cap %d (status %d), thread reordering cap %d (status %d)\n", (int)oc, (int)s1, (int)tc, (int)s2);
        }
        else logf("NVAPI: not available\n");

        D3D12_COMMAND_QUEUE_DESC q{}; q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        check(dev->CreateCommandQueue(&q, IID_PPV_ARGS(&direct)), "direct queue");
        q.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        check(dev->CreateCommandQueue(&q, IID_PPV_ARGS(&compute)), "compute queue");
        check(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocDirect)), "alloc");
        check(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&allocCompute)), "alloc");
        check(dev->CreateCommandList1(0, D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_FLAG_NONE, IID_PPV_ARGS(&listDirect)), "list");
        check(dev->CreateCommandList1(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, D3D12_COMMAND_LIST_FLAG_NONE, IID_PPV_ARGS(&listCompute)), "list");
        check(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fenceDirect)), "fence");
        check(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fenceCompute)), "fence");
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        check(direct->GetTimestampFrequency(&timestampFrequency), "timestamp frequency");

        D3D12_DESCRIPTOR_HEAP_DESC h{}; h.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; h.NumDescriptors = 4096; h.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        check(dev->CreateDescriptorHeap(&h, IID_PPV_ARGS(&heapCbv)), "cbv heap");
        h.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER; h.NumDescriptors = 8;
        check(dev->CreateDescriptorHeap(&h, IID_PPV_ARGS(&heapSampler)), "sampler heap");
        h.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; h.NumDescriptors = 8; h.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        check(dev->CreateDescriptorHeap(&h, IID_PPV_ARGS(&heapRtv)), "rtv heap");
        h.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        check(dev->CreateDescriptorHeap(&h, IID_PPV_ARGS(&heapDsv)), "dsv heap");
        incCbv = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        incRtv = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        incDsv = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        D3D12_SAMPLER_DESC s{}; s.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR; s.AddressU = s.AddressV = s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP; s.MaxLOD = D3D12_FLOAT32_MAX; s.MaxAnisotropy = 1; s.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        dev->CreateSampler(&s, heapSampler->GetCPUDescriptorHandleForHeapStart());
        s.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        D3D12_CPU_DESCRIPTOR_HANDLE sh = heapSampler->GetCPUDescriptorHandleForHeapStart(); sh.ptr += dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        dev->CreateSampler(&s, sh);

        D3D12_QUERY_HEAP_DESC qh{}; qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; qh.Count = 64;
        check(dev->CreateQueryHeap(&qh, IID_PPV_ARGS(&queryHeap)), "query heap");
        queryReadback = buffer(64 * 8, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);

        D3D12_DESCRIPTOR_RANGE1 nvRange{}; nvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; nvRange.NumDescriptors = 1; nvRange.BaseShaderRegister = 999; nvRange.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
        D3D12_ROOT_PARAMETER1 params[4]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; params[0].Constants.Num32BitValues = 32; params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; params[1].Descriptor.ShaderRegister = 0; params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; params[2].Descriptor.ShaderRegister = 0; params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; params[3].DescriptorTable.NumDescriptorRanges = 1; params[3].DescriptorTable.pDescriptorRanges = &nvRange; params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC rs{}; rs.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        rs.Desc_1_1.NumParameters = 4; rs.Desc_1_1.pParameters = params;
        rs.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;
        ComPtr<ID3DBlob> blob, err;
        if (FAILED(D3D12SerializeVersionedRootSignature(&rs, &blob, &err))) throw Failure{ std::string("root signature: ") + (err ? (char*)err->GetBufferPointer() : "") };
        check(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rootSig)), "CreateRootSignature");
    }

    // ---- resources
    ComPtr<ID3D12Resource> buffer(UINT64 bytes, D3D12_HEAP_TYPE type = D3D12_HEAP_TYPE_DEFAULT,
                                  D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON,
                                  D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
    {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = type;
        D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = std::max<UINT64>(bytes, 256); d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
        d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; d.Flags = flags;
        if (type == D3D12_HEAP_TYPE_UPLOAD) state = D3D12_RESOURCE_STATE_GENERIC_READ;
        ComPtr<ID3D12Resource> r;
        check(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r)), "CreateCommittedResource(buffer)");
        return r;
    }
    ComPtr<ID3D12Resource> texture2D(UINT w, UINT h, DXGI_FORMAT fmt, UINT mips, D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags, const D3D12_CLEAR_VALUE* clear = nullptr)
    {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = (UINT16)mips; d.Format = fmt;
        d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN; d.Flags = flags;
        ComPtr<ID3D12Resource> r;
        check(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, clear, IID_PPV_ARGS(&r)), "CreateCommittedResource(texture)");
        return r;
    }
    void execute(ID3D12GraphicsCommandList6* list, ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64& value)
    {
        check(list->Close(), "Close");
        ID3D12CommandList* lists[] = { list };
        queue->ExecuteCommandLists(1, lists);
        check(queue->Signal(fence, ++value), "Signal");
    }
    void wait(ID3D12Fence* fence, UINT64 value)
    {
        if (fence->GetCompletedValue() < value) { check(fence->SetEventOnCompletion(value, event), "SetEventOnCompletion"); WaitForSingleObject(event, INFINITE); }
    }
    ID3D12GraphicsCommandList6* begin()
    {
        check(allocDirect->Reset(), "alloc reset");
        check(listDirect->Reset(allocDirect.Get(), nullptr), "list reset");
        ID3D12DescriptorHeap* heaps[] = { heapCbv.Get(), heapSampler.Get() };
        listDirect->SetDescriptorHeaps(2, heaps);
        return listDirect.Get();
    }
    void submitAndWait()
    {
        execute(listDirect.Get(), direct.Get(), fenceDirect.Get(), fenceValueDirect);
        wait(fenceDirect.Get(), fenceValueDirect);
    }
    void upload(ID3D12Resource* dst, const void* data, UINT64 bytes)
    {
        auto up = buffer(bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE);
        void* p; check(up->Map(0, nullptr, &p), "Map"); memcpy(p, data, bytes); up->Unmap(0, nullptr);
        auto l = begin(); l->CopyBufferRegion(dst, 0, up.Get(), 0, bytes); submitAndWait();
    }
    std::vector<uint8_t> readback(ID3D12Resource* src, UINT64 bytes)
    {
        auto rb = buffer(bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
        auto l = begin(); l->CopyBufferRegion(rb.Get(), 0, src, 0, bytes); submitAndWait();
        std::vector<uint8_t> out(bytes); void* p; D3D12_RANGE r{ 0, bytes }; check(rb->Map(0, &r, &p), "Map"); memcpy(out.data(), p, bytes); rb->Unmap(0, nullptr);
        return out;
    }
    void uploadTexture(ID3D12Resource* tex, const std::vector<std::vector<uint8_t>>& mips, UINT w, UINT h, UINT bpp)
    {
        UINT n = (UINT)mips.size();
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fp(n); std::vector<UINT> rows(n); std::vector<UINT64> rowBytes(n); UINT64 total;
        D3D12_RESOURCE_DESC d = tex->GetDesc();
        dev->GetCopyableFootprints(&d, 0, n, 0, fp.data(), rows.data(), rowBytes.data(), &total);
        auto up = buffer(total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE);
        uint8_t* p; check(up->Map(0, nullptr, (void**)&p), "Map");
        for (UINT m = 0; m < n; ++m)
        {
            UINT mw = std::max(1u, w >> m), mh = std::max(1u, h >> m);
            for (UINT y = 0; y < mh; ++y) memcpy(p + fp[m].Offset + y * fp[m].Footprint.RowPitch, mips[m].data() + (size_t)y * mw * bpp, (size_t)mw * bpp);
        }
        up->Unmap(0, nullptr);
        auto l = begin();
        for (UINT m = 0; m < n; ++m)
        {
            D3D12_TEXTURE_COPY_LOCATION dst{ tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {} }; dst.SubresourceIndex = m;
            D3D12_TEXTURE_COPY_LOCATION src{ up.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {} }; src.PlacedFootprint = fp[m];
            l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = tex; b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        l->ResourceBarrier(1, &b);
        submitAndWait();
    }

    // ---- descriptors
    D3D12_CPU_DESCRIPTOR_HANDLE cbvHandle(UINT i) { auto h = heapCbv->GetCPUDescriptorHandleForHeapStart(); h.ptr += (SIZE_T)i * incCbv; return h; }
    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle(UINT i) { auto h = heapCbv->GetGPUDescriptorHandleForHeapStart(); h.ptr += (UINT64)i * incCbv; return h; }
    UINT srvStructured(ID3D12Resource* r, UINT count, UINT stride)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC d{}; d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER; d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Buffer.NumElements = count; d.Buffer.StructureByteStride = stride;
        UINT i = nextCbv++; dev->CreateShaderResourceView(r, &d, cbvHandle(i)); return i;
    }
    UINT srvRaw(ID3D12Resource* r, UINT64 bytes)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC d{}; d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER; d.Format = DXGI_FORMAT_R32_TYPELESS; d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Buffer.NumElements = (UINT)(bytes / 4); d.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        UINT i = nextCbv++; dev->CreateShaderResourceView(r, &d, cbvHandle(i)); return i;
    }
    UINT uavStructured(ID3D12Resource* r, UINT count, UINT stride)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC d{}; d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER; d.Buffer.NumElements = count; d.Buffer.StructureByteStride = stride;
        UINT i = nextCbv++; dev->CreateUnorderedAccessView(r, nullptr, &d, cbvHandle(i)); return i;
    }
    UINT srvTexture(ID3D12Resource* r, DXGI_FORMAT fmt, UINT mips)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC d{}; d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; d.Format = fmt; d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; d.Texture2D.MipLevels = mips;
        UINT i = nextCbv++; dev->CreateShaderResourceView(r, &d, cbvHandle(i)); return i;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv(ID3D12Resource* r) { auto h = heapRtv->GetCPUDescriptorHandleForHeapStart(); h.ptr += (SIZE_T)(nextRtv++) * incRtv; dev->CreateRenderTargetView(r, nullptr, h); return h; }
    D3D12_CPU_DESCRIPTOR_HANDLE dsv(ID3D12Resource* r) { auto h = heapDsv->GetCPUDescriptorHandleForHeapStart(); h.ptr += (SIZE_T)(nextDsv++) * incDsv; dev->CreateDepthStencilView(r, nullptr, h); return h; }

    // ---- pipelines
    ComPtr<ID3D12PipelineState> computePso(IDxcBlob* cs, double* msOut = nullptr)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC d{}; d.pRootSignature = rootSig.Get(); d.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
        double t0 = now(); ComPtr<ID3D12PipelineState> p; check(dev->CreateComputePipelineState(&d, IID_PPV_ARGS(&p)), "CreateComputePipelineState");
        if (msOut) *msOut = now() - t0; return p;
    }
    template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE T, typename V> struct alignas(void*) Sub { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = T; V value{}; };
    struct MeshStream
    {
        Sub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature*> rs;
        Sub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, D3D12_SHADER_BYTECODE> ms;
        Sub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> ps;
        Sub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> rast;
        Sub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC> ds;
        Sub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY> rtv;
        Sub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, DXGI_FORMAT> dsv;
        Sub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE> topo;
        Sub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC> sd;
        Sub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT> mask;
        Sub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, D3D12_BLEND_DESC> blend;
    };
    ComPtr<ID3D12PipelineState> meshPso(IDxcBlob* ms, IDxcBlob* ps, bool target, bool depthWrite, double* msOut = nullptr, bool conservative = false, bool depthEnable = true)
    {
        MeshStream s{};
        s.rs.value = rootSig.Get();
        s.ms.value = { ms->GetBufferPointer(), ms->GetBufferSize() };
        if (ps) s.ps.value = { ps->GetBufferPointer(), ps->GetBufferSize() };
        s.rast.value.FillMode = D3D12_FILL_MODE_SOLID; s.rast.value.CullMode = D3D12_CULL_MODE_NONE; s.rast.value.DepthClipEnable = TRUE;
        s.rast.value.ConservativeRaster = conservative ? D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON : D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
        s.ds.value.DepthEnable = depthEnable ? TRUE : FALSE; s.ds.value.DepthWriteMask = depthWrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO; s.ds.value.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        s.rtv.value.NumRenderTargets = target ? 1 : 0; s.rtv.value.RTFormats[0] = target ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_UNKNOWN;
        s.dsv.value = depthEnable ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_UNKNOWN;
        s.topo.value = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        s.sd.value = { 1, 0 }; s.mask.value = UINT_MAX;
        s.blend.value.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        D3D12_PIPELINE_STATE_STREAM_DESC d{ sizeof s, &s };
        double t0 = now(); ComPtr<ID3D12PipelineState> p; check(dev->CreatePipelineState(&d, IID_PPV_ARGS(&p)), "CreatePipelineState(mesh)");
        if (msOut) *msOut = now() - t0; return p;
    }
    struct RtPipeline { ComPtr<ID3D12StateObject> so; ComPtr<ID3D12Resource> table; D3D12_GPU_VIRTUAL_ADDRESS va = 0; };
    RtPipeline rtPso(IDxcBlob* lib, bool allowOmm, bool ser = false, double* msOut = nullptr)
    {
        bool nvOmm = allowOmm && g_useNvOmm;
        NVAPI_D3D12_SET_CREATE_PIPELINE_STATE_OPTIONS_PARAMS nvOpt{}; nvOpt.version = NVAPI_D3D12_SET_CREATE_PIPELINE_STATE_OPTIONS_PARAMS_VER;
        if (nvOmm) { nvOpt.flags = NVAPI_D3D12_PIPELINE_CREATION_STATE_FLAGS_ENABLE_OMM_SUPPORT; NvAPI_D3D12_SetCreatePipelineStateOptions(dev.Get(), &nvOpt); }
        if (ser) NvAPI_D3D12_SetNvShaderExtnSlotSpace(dev.Get(), 999, 0);
        D3D12_DXIL_LIBRARY_DESC ld{}; ld.DXILLibrary = { lib->GetBufferPointer(), lib->GetBufferSize() };
        D3D12_HIT_GROUP_DESC hg0{}; hg0.HitGroupExport = L"HG"; hg0.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES; hg0.AnyHitShaderImport = L"AnyHit"; hg0.ClosestHitShaderImport = L"ClosestHit";
        D3D12_HIT_GROUP_DESC hg1{}; hg1.HitGroupExport = L"HGAccept"; hg1.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES; hg1.AnyHitShaderImport = L"AnyHitAccept"; hg1.ClosestHitShaderImport = L"ClosestHit";
        D3D12_RAYTRACING_SHADER_CONFIG sc{ 4, 8 };
        D3D12_RAYTRACING_PIPELINE_CONFIG1 pc{ 1, (allowOmm && !nvOmm) ? D3D12_RAYTRACING_PIPELINE_FLAG_ALLOW_OPACITY_MICROMAPS : D3D12_RAYTRACING_PIPELINE_FLAG_NONE };
        D3D12_GLOBAL_ROOT_SIGNATURE grs{ rootSig.Get() };
        D3D12_STATE_SUBOBJECT subs[6] = {
            { D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &ld }, { D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hg0 }, { D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hg1 },
            { D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &sc }, { D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG1, &pc }, { D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &grs } };
        D3D12_STATE_OBJECT_DESC sd{ D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, 6, subs };
        RtPipeline p; double t0 = now();
        HRESULT hr = dev->CreateStateObject(&sd, IID_PPV_ARGS(&p.so));
        if (nvOmm) { nvOpt.flags = NVAPI_D3D12_PIPELINE_CREATION_STATE_FLAGS_NONE; NvAPI_D3D12_SetCreatePipelineStateOptions(dev.Get(), &nvOpt); }
        if (ser) NvAPI_D3D12_SetNvShaderExtnSlotSpace(dev.Get(), 0xFFFFFFFFu, 0);
        check(hr, "CreateStateObject");
        if (msOut) *msOut = now() - t0;
        ComPtr<ID3D12StateObjectProperties> props; check(p.so.As(&props), "StateObjectProperties");
        p.table = buffer(256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE);
        uint8_t* t; check(p.table->Map(0, nullptr, (void**)&t), "Map table");
        memset(t, 0, 256);
        memcpy(t + 0, props->GetShaderIdentifier(L"RayGen"), 32);
        memcpy(t + 64, props->GetShaderIdentifier(L"Miss"), 32);
        memcpy(t + 128, props->GetShaderIdentifier(L"HG"), 32);
        memcpy(t + 160, props->GetShaderIdentifier(L"HGAccept"), 32);
        p.table->Unmap(0, nullptr);
        p.va = p.table->GetGPUVirtualAddress();
        return p;
    }
    void dispatchRays(ID3D12GraphicsCommandList6* l, const RtPipeline& p, UINT w, UINT h, UINT d)
    {
        D3D12_DISPATCH_RAYS_DESC r{};
        r.RayGenerationShaderRecord = { p.va, 32 };
        r.MissShaderTable = { p.va + 64, 32, 32 };
        r.HitGroupTable = { p.va + 128, 64, 32 };
        r.Width = w; r.Height = h; r.Depth = d;
        l->SetComputeRootDescriptorTable(3, gpuHandle(g_nvSlotUav));
        l->SetPipelineState1(p.so.Get());
        l->DispatchRays(&r);
    }

    // ---- timing: GPU timestamps around a recorded region, median of reps
    Stat time(const std::function<void(ID3D12GraphicsCommandList6*)>& rec, int repsOverride = -1)
    {
        int n = repsOverride > 0 ? repsOverride : reps;
        std::vector<double> v;
        for (int i = 0; i < warm + n; ++i)
        {
            auto l = begin();
            l->EndQuery(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
            rec(l);
            l->EndQuery(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
            l->ResolveQueryData(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, queryReadback.Get(), 0);
            submitAndWait();
            UINT64* q; D3D12_RANGE r{ 0, 16 }; check(queryReadback->Map(0, &r, (void**)&q), "Map query");
            double ms = (double)(q[1] - q[0]) * 1000.0 / (double)timestampFrequency;
            queryReadback->Unmap(0, nullptr);
            if (i >= warm) v.push_back(ms);
        }
        std::sort(v.begin(), v.end());
        Stat s; s.reps = (int)v.size(); s.min = v.front(); s.max = v.back(); s.median = v[v.size() / 2];
        return s;
    }
    void setConstants(ID3D12GraphicsCommandList6* l, const uint32_t* c, bool graphics = false)
    {
        if (graphics) { l->SetGraphicsRootSignature(rootSig.Get()); l->SetGraphicsRoot32BitConstants(0, 32, c, 0); }
        else { l->SetComputeRootSignature(rootSig.Get()); l->SetComputeRoot32BitConstants(0, 32, c, 0); }
    }
    static void uavBarrier(ID3D12GraphicsCommandList6* l, ID3D12Resource* r = nullptr)
    {
        D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; b.UAV.pResource = r; l->ResourceBarrier(1, &b);
    }
};

static Gpu g;
static Dxc dxc;
static std::string g_shaderDir = MB_SHADER_DIR;
static std::string g_common;
static ComPtr<ID3D12Resource> g_scratchOut; static UINT g_outUav = 0; // generic 64 MB output for sentinel writes
static const uint32_t SENTINEL = 0x7fc00123u;

struct Consts { uint32_t v[32]{}; uint32_t& operator()(int i, int c) { return v[i * 4 + c]; } };


static ComPtr<ID3D12PipelineState> g_warmPso;
static void warmUp(const char* section)
{
    if (!g_warmPso) g_warmPso = g.computePso(dxc.compile(g_common + readFile(g_shaderDir + "/compute.hlsl"), L"FmaCS", L"cs_6_6", {}).Get());
    Consts c; c(0, 0) = 4096; c(0, 1) = g_outUav; c(0, 2) = SENTINEL;
    double t0 = now();
    while (now() - t0 < 1500) { auto l = g.begin(); g.setConstants(l, c.v); l->SetPipelineState(g_warmPso.Get()); l->Dispatch(8192, 1, 1); g.submitAndWait(); }
    logf("\n== %s (after 1.5 s warm-up)\n", section);
}

// ------------------------------------------------------------------------------------------ compute floor
static void testCompute()
{
    warmUp("Compute");
    std::string src = g_common + readFile(g_shaderDir + "/compute.hlsl");
    auto pso = g.computePso(dxc.compile(src, L"FmaCS", L"cs_6_6", {}).Get());
    const uint32_t groups = 16384, threads = groups * 256; // 4.2M threads
    for (uint32_t iters : { 512u, 2048u })
    {
        Consts c; c(0, 0) = iters; c(0, 1) = g_outUav; c(0, 2) = SENTINEL;
        Stat s = g.time([&](ID3D12GraphicsCommandList6* l) { g.setConstants(l, c.v); l->SetPipelineState(pso.Get()); l->Dispatch(groups, 1, 1); });
        double flops = (double)threads * iters * 64.0;
        record("compute", "fp32 fma throughput (" + std::to_string(iters) + " iters, 8 float4 chains, unroll 8)", flops / (s.median * 1e-3) / 1e12, "TFLOPS", "median of " + std::to_string(s.reps) + ", best " + std::to_string(flops / (s.min * 1e-3) / 1e12) + ", peak 48.7 at 2.5 GHz");
    }
}

// ------------------------------------------------------------------------------------------ memory floor
static void testMemory()
{
    warmUp("Memory");
    std::string src = g_common + readFile(g_shaderDir + "/compute.hlsl");
    auto psoRead = g.computePso(dxc.compile(src, L"ReadCS", L"cs_6_6", {}).Get());
    auto psoWrite = g.computePso(dxc.compile(src, L"WriteCS", L"cs_6_6", {}).Get());
    auto psoCopy = g.computePso(dxc.compile(src, L"CopyCS", L"cs_6_6", {}).Get());
    auto psoGather = g.computePso(dxc.compile(src, L"GatherCS", L"cs_6_6", {}).Get());
    auto psoChase = g.computePso(dxc.compile(src, L"ChaseCS", L"cs_6_6", {}).Get());
    const UINT64 bytes = 1ull << 30; // 1 GiB
    auto src1 = g.buffer(bytes), dst1 = g.buffer(bytes);
    UINT srcSrv = g.srvStructured(src1.Get(), (UINT)(bytes / 16), 16), srcRaw = g.srvRaw(src1.Get(), bytes);
    UINT dstUav = g.uavStructured(dst1.Get(), (UINT)(bytes / 16), 16), srcUav = g.uavStructured(src1.Get(), (UINT)(bytes / 16), 16);
    const uint32_t per = 64, groups = (uint32_t)(bytes / 16 / 256 / per); // 4096 groups -> 1 GiB per dispatch
    // Fresh allocations read as zero pages and structured data compresses (Ampere/Ada generic memory
    // compression), so bandwidth is measured on hashed (incompressible) content and compared with the
    // structured pattern to show the compression effect explicitly.
    for (int content : { 1, 0 })
    {
        Consts c; c(0, 0) = srcUav; c(0, 2) = per; c(0, 3) = (uint32_t)(bytes / 16 - 1); c(1, 0) = 7; c(1, 1) = content; c(1, 2) = groups;
        Stat s = g.time([&](ID3D12GraphicsCommandList6* l) { g.setConstants(l, c.v); l->SetPipelineState(psoWrite.Get()); l->Dispatch(groups, 1, 1); });
        record("memory", std::string("sequential write 1 GiB, ") + (content ? "hashed (incompressible) data" : "structured (compressible) data"), bytes / (s.median * 1e-3) / 1e9, "GB/s");
        Consts r; r(0, 0) = srcSrv; r(0, 1) = g_outUav; r(0, 2) = per; r(0, 3) = (uint32_t)(bytes / 16 - 1); r(1, 0) = SENTINEL; r(1, 1) = groups;
        Stat t = g.time([&](ID3D12GraphicsCommandList6* l) { g.setConstants(l, r.v); l->SetPipelineState(psoRead.Get()); l->Dispatch(groups, 1, 1); });
        record("memory", std::string("sequential read 1 GiB of ") + (content ? "hashed data" : "structured data"), bytes / (t.median * 1e-3) / 1e9, "GB/s", content ? "true DRAM read floor" : "compression inflated");
    }
    { // leave incompressible content in src for the remaining tests
        Consts c; c(0, 0) = srcUav; c(0, 2) = per; c(0, 3) = (uint32_t)(bytes / 16 - 1); c(1, 0) = 11; c(1, 1) = 1; c(1, 2) = groups;
        auto l = g.begin(); g.setConstants(l, c.v); l->SetPipelineState(psoWrite.Get()); l->Dispatch(groups, 1, 1); g.submitAndWait();
    }
    {
        Consts c; c(0, 0) = srcSrv; c(0, 1) = dstUav; c(0, 2) = per / 2; c(0, 3) = (uint32_t)(bytes / 16 - 1); c(1, 2) = groups;
        Stat s = g.time([&](ID3D12GraphicsCommandList6* l) { g.setConstants(l, c.v); l->SetPipelineState(psoCopy.Get()); l->Dispatch(groups, 1, 1); });
        record("memory", "copy 512 MiB read + 512 MiB write", bytes / (s.median * 1e-3) / 1e9, "GB/s", "read+write bytes summed");
    }
    {
        Consts c; c(0, 0) = srcSrv; c(0, 1) = g_outUav; c(0, 2) = per; c(0, 3) = (uint32_t)((4ull << 20) / 16 - 1); c(1, 0) = SENTINEL; c(1, 1) = groups;
        Stat s = g.time([&](ID3D12GraphicsCommandList6* l) { g.setConstants(l, c.v); l->SetPipelineState(psoRead.Get()); l->Dispatch(groups, 1, 1); });
        record("memory", "cached loads, each group re-reads its own 4 KB (L1-resident)", bytes / (s.median * 1e-3) / 1e9, "GB/s", "SM load-path ceiling");
    }
    auto psoChunk = g.computePso(dxc.compile(src, L"ReadChunkCS", L"cs_6_6", {}).Get());
    for (UINT64 window : { 8ull << 20, 32ull << 20, 64ull << 20, 128ull << 20, 512ull << 20 })
    {
        Consts c; c(0, 0) = srcSrv; c(0, 1) = g_outUav; c(0, 2) = per; c(0, 3) = (uint32_t)(window / 16 - 1); c(1, 0) = SENTINEL; c(1, 1) = 3;
        Stat s = g.time([&](ID3D12GraphicsCommandList6* l) { g.setConstants(l, c.v); l->SetPipelineState(psoChunk.Get()); l->Dispatch(groups, 1, 1); });
        record("memory", "random 4 KB chunk reads, 1 GiB traffic, " + std::to_string(window >> 20) + " MiB window", bytes / (s.median * 1e-3) / 1e9, "GB/s", window <= (48ull << 20) ? "L2 resident (64 MB L2)" : "beyond L2");
    }
    // random gathers
    for (int gran : { 0, 1 })
    {
        const uint32_t threads = 2u << 20, loads = 32;
        for (UINT64 window : { 1ull << 30, 32ull << 20, 2ull << 20 })
        {
            Consts c; c(0, 0) = srcRaw; c(0, 1) = g_outUav; c(0, 2) = loads; c(0, 3) = (uint32_t)(window - 1); c(1, 0) = SENTINEL; c(1, 1) = 17; c(1, 2) = gran;
            Stat s = g.time([&](ID3D12GraphicsCommandList6* l) { g.setConstants(l, c.v); l->SetPipelineState(psoGather.Get()); l->Dispatch(threads / 256, 1, 1); });
            double n = (double)threads * loads;
            std::string name = std::string("random ") + (gran ? "64 B" : "4 B") + " loads, " + std::to_string(window >> 20) + " MiB window";
            record("memory", name, n / (s.median * 1e-3) / 1e9, "Gloads/s", gran ? std::to_string(n * 64 / (s.median * 1e-3) / 1e9) + " GB/s useful" : std::to_string(n * 32 / (s.median * 1e-3) / 1e9) + " GB/s at 32 B sectors");
        }
    }
    // pointer chase latency
    for (UINT64 entries : { 64ull << 20, 1ull << 20, 16ull << 10 })
    {
        std::vector<uint32_t> perm(entries);
        for (uint32_t i = 0; i < entries; ++i) perm[i] = i;
        std::mt19937_64 rng(42);
        for (uint64_t i = entries - 1; i > 0; --i) { uint64_t j = rng() % i; std::swap(perm[i], perm[j]); } // Sattolo: single cycle
        auto buf = g.buffer(entries * 4); g.upload(buf.Get(), perm.data(), entries * 4);
        UINT srv = g.srvStructured(buf.Get(), (UINT)entries, 4);
        const uint32_t chain = 65536; uint32_t rep = 0;
        Consts c; c(0, 0) = srv; c(0, 1) = g_outUav; c(0, 2) = chain; c(1, 1) = 0;
        Stat s = g.time([&](ID3D12GraphicsCommandList6* l) { c(1, 1) = (uint32_t)(((uint64_t)(++rep) * 2654435761ull) & (entries - 1)); g.setConstants(l, c.v); l->SetPipelineState(psoChase.Get()); l->Dispatch(1, 1, 1); }, 3);
        record("memory", "dependent load latency, " + std::to_string(entries * 4 >> 10) + " KiB working set", s.median * 1e6 / chain, "ns/load", "single thread pointer chase");
    }
}

// ------------------------------------------------------------------------------------------ texture floor
static void testTexture()
{
    warmUp("Texture");
    std::string src = g_common + readFile(g_shaderDir + "/compute.hlsl");
    auto pso = g.computePso(dxc.compile(src, L"TexCS", L"cs_6_6", {}).Get());
    const UINT size = 4096; UINT mips = 13;
    std::vector<std::vector<uint8_t>> levels(mips);
    levels[0].resize((size_t)size * size * 4);
    uint32_t s = 1; for (auto& b : levels[0]) { s = s * 1664525u + 1013904223u; b = (uint8_t)(s >> 24); }
    for (UINT m = 1; m < mips; ++m)
    {
        UINT w = size >> m, pw = size >> (m - 1); levels[m].resize((size_t)w * w * 4);
        for (UINT y = 0; y < w; ++y) for (UINT x = 0; x < w; ++x) for (UINT k = 0; k < 4; ++k)
            levels[m][((size_t)y * w + x) * 4 + k] = (uint8_t)((levels[m - 1][((size_t)(2 * y) * pw + 2 * x) * 4 + k] + levels[m - 1][((size_t)(2 * y) * pw + 2 * x + 1) * 4 + k] + levels[m - 1][((size_t)(2 * y + 1) * pw + 2 * x) * 4 + k] + levels[m - 1][((size_t)(2 * y + 1) * pw + 2 * x + 1) * 4 + k]) / 4);
    }
    auto tex = g.texture2D(size, size, DXGI_FORMAT_R8G8B8A8_UNORM, mips, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    g.uploadTexture(tex.Get(), levels, size, size, 4);
    UINT srv = g.srvTexture(tex.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, mips);
    const uint32_t groupsX = 1024, groupsY = 128, samples = 32; // 8.4M threads
    const char* names[] = { "coherent bilinear lod0", "incoherent bilinear lod0", "coherent trilinear SampleGrad", "coherent point lod0" };
    for (uint32_t mode = 0; mode < 4; ++mode)
    {
        Consts c; c(0, 0) = srv; c(0, 1) = g_outUav; c(0, 2) = samples; c(0, 3) = mode; c(1, 0) = SENTINEL; c(1, 1) = 3; c(1, 2) = size;
        Stat st = g.time([&](ID3D12GraphicsCommandList6* l) { g.setConstants(l, c.v); l->SetPipelineState(pso.Get()); l->Dispatch(groupsX, groupsY, 1); });
        double n = (double)groupsX * groupsY * 64 * samples;
        record("texture", std::string("RGBA8 4096^2 ") + names[mode], n / (st.median * 1e-3) / 1e9, "Gsamples/s");
    }
}

// ------------------------------------------------------------------------------------------ atomics + dispatch overhead
static void testAtomicsAndDispatch()
{
    warmUp("Atomics / dispatch overhead");
    std::string src = g_common + readFile(g_shaderDir + "/compute.hlsl");
    auto psoAtomic = g.computePso(dxc.compile(src, L"AtomicCS", L"cs_6_6", {}).Get());
    auto psoTiny = g.computePso(dxc.compile(src, L"TinyCS", L"cs_6_6", {}).Get());
    auto psoTiny2 = g.computePso(dxc.compile(src, L"TinyCS", L"cs_6_6", { L"VARIANT=2" }).Get());
    auto buf = g.buffer(64 << 20); UINT uav = g.uavStructured(buf.Get(), 16 << 20, 4);
    const uint32_t threads = 4u << 20, per = 16;
    for (uint32_t mask : { 0u, 255u, (1u << 20) - 1 })
    {
        Consts c; c(0, 0) = uav; c(0, 2) = per; c(0, 3) = mask; c(1, 1) = 5;
        Stat s = g.time([&](ID3D12GraphicsCommandList6* l) { g.setConstants(l, c.v); l->SetPipelineState(psoAtomic.Get()); l->Dispatch(threads / 256, 1, 1); });
        record("atomics", "InterlockedAdd, " + (mask ? std::to_string(mask + 1) + " addresses" : std::string("single address")), (double)threads * per / (s.median * 1e-3) / 1e9, "Gatomics/s");
    }
    const int N = 1000;
    Consts c; c(0, 0) = uav;
    {
        Stat s = g.time([&](ID3D12GraphicsCommandList6* l) { g.setConstants(l, c.v); l->SetPipelineState(psoTiny.Get()); for (int i = 0; i < N; ++i) l->Dispatch(1, 1, 1); });
        record("dispatch", "1000 x Dispatch(1) no barrier", s.median * 1e3 / N, "us/dispatch");
    }
    {
        Stat s = g.time([&](ID3D12GraphicsCommandList6* l) { g.setConstants(l, c.v); l->SetPipelineState(psoTiny.Get()); for (int i = 0; i < N; ++i) { l->Dispatch(1, 1, 1); Gpu::uavBarrier(l, buf.Get()); } });
        record("dispatch", "1000 x Dispatch(1) + UAV barrier", s.median * 1e3 / N, "us/dispatch", "fixed cost floor of one dependent pass");
    }
    {
        Stat s = g.time([&](ID3D12GraphicsCommandList6* l) { g.setConstants(l, c.v); for (int i = 0; i < N; ++i) { l->SetPipelineState((i & 1) ? psoTiny2.Get() : psoTiny.Get()); l->SetComputeRoot32BitConstants(0, 1, &i, 1); l->Dispatch(1, 1, 1); Gpu::uavBarrier(l, buf.Get()); } });
        record("dispatch", "1000 x (PSO switch + constants + Dispatch(1) + UAV barrier)", s.median * 1e3 / N, "us/dispatch");
    }
    {
        Stat s = g.time([&](ID3D12GraphicsCommandList6* l) { g.setConstants(l, c.v); l->SetPipelineState(psoTiny.Get()); for (int i = 0; i < N; ++i) { l->Dispatch(1024, 1, 1); Gpu::uavBarrier(l, buf.Get()); } });
        record("dispatch", "1000 x Dispatch(1024 groups of 64) + UAV barrier", s.median * 1e3 / N, "us/dispatch", "65k-thread pass with real work");
    }
}

// ------------------------------------------------------------------------------------------ raster floor
struct RasterAssets { ComPtr<ID3D12Resource> vis, depth; D3D12_CPU_DESCRIPTOR_HANDLE rtv{}, dsv{}; ComPtr<ID3D12PipelineState> full[5], depthOnly[5]; };
static RasterAssets g_raster;
static void setupRaster()
{
    if (g_raster.vis) return;
    std::string src = g_common + readFile(g_shaderDir + "/raster.hlsl");
    auto ps = dxc.compile(src, L"PS", L"ps_6_6", {});
    for (int mode = 0; mode < 5; ++mode)
    {
        auto ms = dxc.compile(src, L"MS", L"ms_6_6", { L"MODE=" + std::to_wstring(mode) });
        double msFull, msDepth;
        g_raster.full[mode] = g.meshPso(ms.Get(), ps.Get(), true, true, &msFull);
        g_raster.depthOnly[mode] = g.meshPso(ms.Get(), nullptr, false, true, &msDepth);
        if (mode == 0) { record("pso", "mesh+pixel graphics PSO create (cpu)", msFull, "ms"); record("pso", "mesh depth-only graphics PSO create (cpu)", msDepth, "ms"); }
    }
    g_raster.vis = g.texture2D(3840, 2160, DXGI_FORMAT_R32_UINT, 1, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, nullptr);
    D3D12_CLEAR_VALUE dv{ DXGI_FORMAT_D32_FLOAT, {} }; dv.DepthStencil.Depth = 1.0f;
    g_raster.depth = g.texture2D(3840, 2160, DXGI_FORMAT_D32_FLOAT, 1, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, &dv);
    g_raster.rtv = g.rtv(g_raster.vis.Get()); g_raster.dsv = g.dsv(g_raster.depth.Get());
}
static void recordRaster(ID3D12GraphicsCommandList6* l, ID3D12PipelineState* pso, bool target, const Consts& c, UINT gx, UINT gy)
{
    D3D12_VIEWPORT vp{ 0, 0, 3840, 2160, 0, 1 }; D3D12_RECT sc{ 0, 0, 3840, 2160 };
    l->RSSetViewports(1, &vp); l->RSSetScissorRects(1, &sc);
    l->OMSetRenderTargets(target ? 1 : 0, target ? &g_raster.rtv : nullptr, FALSE, &g_raster.dsv);
    l->ClearDepthStencilView(g_raster.dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    g.setConstants(l, c.v, true);
    l->SetPipelineState(pso);
    l->DispatchMesh(gx, gy, 1);
}
static void testRaster()
{
    warmUp("Raster (mesh shader, 3840x2160 R32_UINT visibility + D32)");
    setupRaster();
    const UINT gx = 1024, gy = 170; const double meshlets = (double)gx * gy, tris = meshlets * 98; // 16.7M triangles
    Consts base; base(0, 2) = asu(3840.f); base(0, 3) = asu(2160.f); base(1, 2) = 11; base(1, 3) = gx;
    {
        Consts c = base; c(0, 0) = 1; c(0, 1) = asu(2.f);
        Stat s = g.time([&](ID3D12GraphicsCommandList6* l) { recordRaster(l, g_raster.full[1].Get(), true, c, gx, gy); });
        record("raster", "meshlet launch only (174k meshlets, zero output)", meshlets / (s.median * 1e-3) / 1e6, "Mmeshlets/s", std::to_string(s.median) + " ms incl. depth clear");
    }
    {
        Stat s = g.time([&](ID3D12GraphicsCommandList6* l) {
            D3D12_VIEWPORT vp{ 0, 0, 3840, 2160, 0, 1 }; D3D12_RECT sc{ 0, 0, 3840, 2160 }; l->RSSetViewports(1, &vp); l->RSSetScissorRects(1, &sc);
            l->OMSetRenderTargets(1, &g_raster.rtv, FALSE, &g_raster.dsv); l->ClearDepthStencilView(g_raster.dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr); });
        record("raster", "4K depth clear alone", s.median, "ms");
    }
    for (float edge : { 0.5f, 1.f, 2.f, 4.f, 8.f, 16.f, 32.f })
    {
        Consts c = base; c(0, 0) = 0; c(0, 1) = asu(edge);
        Stat s = g.time([&](ID3D12GraphicsCommandList6* l) { recordRaster(l, g_raster.full[0].Get(), true, c, gx, gy); });
        char name[128]; snprintf(name, sizeof name, "16.7M tris, edge %.1f px, vis id + depth", edge);
        record("raster", name, tris / (s.median * 1e-3) / 1e9, "Gtris/s", std::to_string(s.median) + " ms");
        Stat d = g.time([&](ID3D12GraphicsCommandList6* l) { recordRaster(l, g_raster.depthOnly[0].Get(), false, c, gx, gy); });
        snprintf(name, sizeof name, "16.7M tris, edge %.1f px, depth only", edge);
        record("raster", name, tris / (d.median * 1e-3) / 1e9, "Gtris/s", std::to_string(d.median) + " ms");
    }
    {
        Consts c = base; c(0, 0) = 2; c(0, 1) = asu(2.f);
        Stat s = g.time([&](ID3D12GraphicsCommandList6* l) { recordRaster(l, g_raster.full[2].Get(), true, c, gx, gy); });
        record("raster", "16.7M tris, edge 2 px, 50% culled by SV_CullPrimitive", tris / (s.median * 1e-3) / 1e9, "Gtris/s (emitted)", std::to_string(s.median) + " ms");
    }
    {
        Consts c = base; c(0, 0) = 4; c(0, 1) = asu(2.f);
        Stat s = g.time([&](ID3D12GraphicsCommandList6* l) { recordRaster(l, g_raster.full[4].Get(), true, c, gx, gy); });
        record("raster", "174k meshlets x 1 triangle (edge 2 px)", meshlets / (s.median * 1e-3) / 1e6, "Mtris/s", "per-meshlet overhead bound");
    }
    for (int order : { 1, 0 })
    {
        const UINT layers = 32;
        Consts c = base; c(0, 0) = 3; c(1, 0) = layers; c(1, 1) = order; c(1, 3) = layers;
        Stat s = g.time([&](ID3D12GraphicsCommandList6* l) { recordRaster(l, g_raster.full[3].Get(), true, c, layers, 1); });
        double pix = 3840.0 * 2160 * layers;
        record("raster", std::string("32 full-screen layers ") + (order ? "back-to-front (all pass, vis id + depth write)" : "front-to-back (early-z reject)"), pix / (s.median * 1e-3) / 1e9, "Gpix/s", std::to_string(s.median) + " ms");
        Stat d = g.time([&](ID3D12GraphicsCommandList6* l) { recordRaster(l, g_raster.depthOnly[3].Get(), false, c, layers, 1); });
        record("raster", std::string("32 full-screen layers ") + (order ? "back-to-front, depth only" : "front-to-back, depth only (early-z reject)"), pix / (d.median * 1e-3) / 1e9, "Gpix/s", std::to_string(d.median) + " ms");
    }
}

// ------------------------------------------------------------------------------------------ ray tracing
struct Blas { ComPtr<ID3D12Resource> as; UINT64 size = 0; };
struct RtScene
{
    ComPtr<ID3D12Resource> terrainPos, terrainIdx, cityPos, treePos[4], grassPos, uvs, alphaTex, charPos, charIdx;
    Blas terrain, city, tree[4], treeOmm[4], grass, chars[64];
    ComPtr<ID3D12Resource> ommInput, ommDescs, ommArray;
    UINT alphaSrv = 0, uvSrv = 0;
    float sceneMin = -1000.f, sceneSize = 2000.f;
    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instancesTrees, instancesGrass;
    bool ommOk = false; std::string ommNote;
    int leafTrisPerTree = 0, trunkTris = 0;
    double unknownFraction = 0;
};
static RtScene S;

static float terrainHeight(float x, float z)
{
    return 6.f * sinf(x / 70.f) * cosf(z / 90.f) + 2.f * sinf(x / 13.f + z / 17.f) + 0.7f * sinf(x / 3.1f - z / 2.7f);
}
static D3D12_RAYTRACING_GEOMETRY_DESC triGeom(D3D12_GPU_VIRTUAL_ADDRESS pos, UINT vertexCount, D3D12_GPU_VIRTUAL_ADDRESS idx, UINT indexCount, bool opaque)
{
    D3D12_RAYTRACING_GEOMETRY_DESC d{}; d.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES; d.Flags = opaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
    d.Triangles.VertexBuffer = { pos, 12 }; d.Triangles.VertexCount = vertexCount; d.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    if (idx) { d.Triangles.IndexBuffer = idx; d.Triangles.IndexCount = indexCount; d.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT; }
    return d;
}
static Blas buildBlas(const std::vector<D3D12_RAYTRACING_GEOMETRY_DESC>& geoms, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS flags, double* gpuMs = nullptr, ComPtr<ID3D12Resource>* scratchOut = nullptr)
{
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{}; in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL; in.Flags = flags; in.NumDescs = (UINT)geoms.size(); in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY; in.pGeometryDescs = geoms.data();
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{}; g.dev->GetRaytracingAccelerationStructurePrebuildInfo(&in, &info);
    Blas b; b.size = info.ResultDataMaxSizeInBytes;
    b.as = g.buffer(info.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    auto scratch = g.buffer(std::max(info.ScratchDataSizeInBytes, info.UpdateScratchDataSizeInBytes));
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC d{}; d.DestAccelerationStructureData = b.as->GetGPUVirtualAddress(); d.Inputs = in; d.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
    Stat s = g.time([&](ID3D12GraphicsCommandList6* l) { l->BuildRaytracingAccelerationStructure(&d, 0, nullptr); Gpu::uavBarrier(l); }, gpuMs ? 3 : 1);
    if (gpuMs) *gpuMs = s.median;
    if (scratchOut) *scratchOut = scratch;
    return b;
}
struct Tlas { ComPtr<ID3D12Resource> as, scratch, instances; UINT count = 0; UINT64 size = 0; };
static Tlas buildTlas(const std::vector<D3D12_RAYTRACING_INSTANCE_DESC>& inst, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS flags, double* gpuMs = nullptr, bool nv = false)
{
    Tlas t; t.count = (UINT)inst.size();
    t.instances = g.buffer(inst.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_NONE);
    g.upload(t.instances.Get(), inst.data(), inst.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{}; in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL; in.Flags = flags; in.NumDescs = t.count; in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY; in.InstanceDescs = t.instances->GetGPUVirtualAddress();
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{}; g.dev->GetRaytracingAccelerationStructurePrebuildInfo(&in, &info);
    t.size = info.ResultDataMaxSizeInBytes;
    t.as = g.buffer(info.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    t.scratch = g.buffer(std::max(info.ScratchDataSizeInBytes, info.UpdateScratchDataSizeInBytes));
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC d{}; d.DestAccelerationStructureData = t.as->GetGPUVirtualAddress(); d.Inputs = in; d.ScratchAccelerationStructureData = t.scratch->GetGPUVirtualAddress();
    if (nv)
    {
        NVAPI_D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS_EX inx{}; inx.type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL; inx.flags = (NVAPI_D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS_EX)flags;
        inx.numDescs = t.count; inx.descsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY; inx.instanceDescs = t.instances->GetGPUVirtualAddress();
        NVAPI_D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC_EX bd{}; bd.destAccelerationStructureData = t.as->GetGPUVirtualAddress(); bd.inputs = inx; bd.scratchAccelerationStructureData = t.scratch->GetGPUVirtualAddress();
        NVAPI_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_EX_PARAMS bp{}; bp.version = NVAPI_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_EX_PARAMS_VER; bp.pDesc = &bd;
        Stat s = g.time([&](ID3D12GraphicsCommandList6* l) { NvAPI_Status r = NvAPI_D3D12_BuildRaytracingAccelerationStructureEx(l, &bp); if (r != NVAPI_OK) throw Failure{ "NVAPI TLAS build failed, status " + std::to_string((int)r) }; Gpu::uavBarrier(l); }, gpuMs ? 5 : 1);
        if (gpuMs) *gpuMs = s.median;
        return t;
    }
    Stat s = g.time([&](ID3D12GraphicsCommandList6* l) { l->BuildRaytracingAccelerationStructure(&d, 0, nullptr); Gpu::uavBarrier(l); }, gpuMs ? 5 : 1);
    if (gpuMs) *gpuMs = s.median;
    return t;
}
static double refitTlas(Tlas& t, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS flags)
{
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{}; in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL; in.Flags = flags | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE; in.NumDescs = t.count; in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY; in.InstanceDescs = t.instances->GetGPUVirtualAddress();
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC d{}; d.DestAccelerationStructureData = t.as->GetGPUVirtualAddress(); d.SourceAccelerationStructureData = t.as->GetGPUVirtualAddress(); d.Inputs = in; d.ScratchAccelerationStructureData = t.scratch->GetGPUVirtualAddress();
    return g.time([&](ID3D12GraphicsCommandList6* l) { l->BuildRaytracingAccelerationStructure(&d, 0, nullptr); Gpu::uavBarrier(l); }, 5).median;
}
static D3D12_RAYTRACING_INSTANCE_DESC instance(float x, float y, float z, float yaw, float scale, D3D12_GPU_VIRTUAL_ADDRESS blas, UINT flags)
{
    D3D12_RAYTRACING_INSTANCE_DESC d{}; float c = cosf(yaw) * scale, s = sinf(yaw) * scale;
    d.Transform[0][0] = c; d.Transform[0][2] = s; d.Transform[0][3] = x;
    d.Transform[1][1] = scale; d.Transform[1][3] = y;
    d.Transform[2][0] = -s; d.Transform[2][2] = c; d.Transform[2][3] = z;
    d.InstanceMask = 0xff; d.Flags = flags; d.AccelerationStructure = blas; return d;
}


// Opacity micromap array + OMM-linked BLAS through DXR 1.2 or, when the runtime/driver pair does not expose
// tier 1.2, through NVAPI (same data formats: OC1 2/4-state, per-OMM desc {byteOffset, level, format}).
static ComPtr<ID3D12Resource> buildOmmArray(ID3D12Resource* input, ID3D12Resource* descs, UINT count, UINT level, bool fourState)
{
    ComPtr<ID3D12Resource> result;
    if (g_useNvOmm)
    {
        NVAPI_D3D12_RAYTRACING_OPACITY_MICROMAP_USAGE_COUNT usage{ count, level, fourState ? NVAPI_D3D12_RAYTRACING_OPACITY_MICROMAP_FORMAT_OC1_4_STATE : NVAPI_D3D12_RAYTRACING_OPACITY_MICROMAP_FORMAT_OC1_2_STATE };
        NVAPI_D3D12_BUILD_RAYTRACING_OPACITY_MICROMAP_ARRAY_INPUTS in{}; in.flags = NVAPI_D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_BUILD_FLAG_PREFER_FAST_TRACE; in.numOMMUsageCounts = 1; in.pOMMUsageCounts = &usage;
        in.inputBuffer = input->GetGPUVirtualAddress(); in.perOMMDescs = { descs->GetGPUVirtualAddress(), 8 };
        NVAPI_D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_PREBUILD_INFO info{};
        NVAPI_GET_RAYTRACING_OPACITY_MICROMAP_ARRAY_PREBUILD_INFO_PARAMS pp{}; pp.version = NVAPI_GET_RAYTRACING_OPACITY_MICROMAP_ARRAY_PREBUILD_INFO_PARAMS_VER; pp.pDesc = &in; pp.pInfo = &info;
        NvAPI_Status st = NvAPI_D3D12_GetRaytracingOpacityMicromapArrayPrebuildInfo(g.dev.Get(), &pp);
        if (st != NVAPI_OK || info.resultDataMaxSizeInBytes == 0) throw Failure{ "NVAPI OMM prebuild failed, status " + std::to_string((int)st) };
        result = g.buffer(info.resultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
        auto scratch = g.buffer(std::max<UINT64>(info.scratchDataSizeInBytes, 256));
        NVAPI_D3D12_BUILD_RAYTRACING_OPACITY_MICROMAP_ARRAY_DESC bd{}; bd.destOpacityMicromapArrayData = result->GetGPUVirtualAddress(); bd.inputs = in; bd.scratchOpacityMicromapArrayData = scratch->GetGPUVirtualAddress();
        NVAPI_BUILD_RAYTRACING_OPACITY_MICROMAP_ARRAY_PARAMS bp{}; bp.version = NVAPI_BUILD_RAYTRACING_OPACITY_MICROMAP_ARRAY_PARAMS_VER; bp.pDesc = &bd;
        auto l = g.begin(); st = NvAPI_D3D12_BuildRaytracingOpacityMicromapArray(l, &bp);
        if (st != NVAPI_OK) throw Failure{ "NVAPI OMM build failed, status " + std::to_string((int)st) };
        Gpu::uavBarrier(l); g.submitAndWait();
        return result;
    }
    D3D12_RAYTRACING_OPACITY_MICROMAP_HISTOGRAM_ENTRY hist{ count, level, fourState ? D3D12_RAYTRACING_OPACITY_MICROMAP_FORMAT_OC1_4_STATE : D3D12_RAYTRACING_OPACITY_MICROMAP_FORMAT_OC1_2_STATE };
    D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_DESC ad{ 1, &hist, input->GetGPUVirtualAddress(), { descs->GetGPUVirtualAddress(), 8 } };
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{}; in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_OPACITY_MICROMAP_ARRAY; in.NumDescs = 1; in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY; in.pOpacityMicromapArrayDesc = &ad;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{}; g.dev->GetRaytracingAccelerationStructurePrebuildInfo(&in, &info);
    if (info.ResultDataMaxSizeInBytes == 0) throw Failure{ "OMM array prebuild returned 0 bytes" };
    result = g.buffer(info.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    auto scratch = g.buffer(std::max<UINT64>(info.ScratchDataSizeInBytes, 256));
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd{}; bd.DestAccelerationStructureData = result->GetGPUVirtualAddress(); bd.Inputs = in; bd.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
    { auto l = g.begin(); l->BuildRaytracingAccelerationStructure(&bd, 0, nullptr); Gpu::uavBarrier(l); g.submitAndWait(); }
    return result;
}
static Blas buildBlasOmm(const D3D12_RAYTRACING_GEOMETRY_DESC* opaquePart, const D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC& leaves,
                         D3D12_GPU_VIRTUAL_ADDRESS ommIndex, D3D12_GPU_VIRTUAL_ADDRESS ommArray, UINT ommCount, UINT level, bool fourState, double* gpuMs)
{
    if (g_useNvOmm)
    {
        NVAPI_D3D12_RAYTRACING_OPACITY_MICROMAP_USAGE_COUNT usage{ ommCount, level, fourState ? NVAPI_D3D12_RAYTRACING_OPACITY_MICROMAP_FORMAT_OC1_4_STATE : NVAPI_D3D12_RAYTRACING_OPACITY_MICROMAP_FORMAT_OC1_2_STATE };
        std::vector<NVAPI_D3D12_RAYTRACING_GEOMETRY_DESC_EX> gs;
        if (opaquePart) { NVAPI_D3D12_RAYTRACING_GEOMETRY_DESC_EX t{}; t.type = NVAPI_D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES_EX; t.flags = opaquePart->Flags; t.triangles = opaquePart->Triangles; gs.push_back(t); }
        NVAPI_D3D12_RAYTRACING_GEOMETRY_DESC_EX lf{}; lf.type = NVAPI_D3D12_RAYTRACING_GEOMETRY_TYPE_OMM_TRIANGLES_EX; lf.flags = D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
        lf.ommTriangles.triangles = leaves;
        lf.ommTriangles.ommAttachment.opacityMicromapIndexBuffer = { ommIndex, 4 }; lf.ommTriangles.ommAttachment.opacityMicromapIndexFormat = DXGI_FORMAT_R32_UINT;
        lf.ommTriangles.ommAttachment.opacityMicromapBaseLocation = 0; lf.ommTriangles.ommAttachment.opacityMicromapArray = ommArray;
        lf.ommTriangles.ommAttachment.numOMMUsageCounts = 1; lf.ommTriangles.ommAttachment.pOMMUsageCounts = &usage;
        gs.push_back(lf);
        NVAPI_D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS_EX in{}; in.type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL; in.flags = NVAPI_D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE_EX;
        in.numDescs = (NvU32)gs.size(); in.descsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY; in.geometryDescStrideInBytes = sizeof(NVAPI_D3D12_RAYTRACING_GEOMETRY_DESC_EX); in.pGeometryDescs = gs.data();
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
        NVAPI_GET_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO_EX_PARAMS pp{}; pp.version = NVAPI_GET_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO_EX_PARAMS_VER; pp.pDesc = &in; pp.pInfo = &info;
        NvAPI_Status st = NvAPI_D3D12_GetRaytracingAccelerationStructurePrebuildInfoEx(g.dev.Get(), &pp);
        if (st != NVAPI_OK) throw Failure{ "NVAPI BLAS(OMM) prebuild failed, status " + std::to_string((int)st) };
        Blas b; b.size = info.ResultDataMaxSizeInBytes;
        b.as = g.buffer(info.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
        auto scratch = g.buffer(std::max(info.ScratchDataSizeInBytes, info.UpdateScratchDataSizeInBytes));
        NVAPI_D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC_EX bd{}; bd.destAccelerationStructureData = b.as->GetGPUVirtualAddress(); bd.inputs = in; bd.scratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
        NVAPI_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_EX_PARAMS bp{}; bp.version = NVAPI_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_EX_PARAMS_VER; bp.pDesc = &bd;
        Stat s = g.time([&](ID3D12GraphicsCommandList6* l) { NvAPI_Status r = NvAPI_D3D12_BuildRaytracingAccelerationStructureEx(l, &bp); if (r != NVAPI_OK) throw Failure{ "NVAPI BLAS(OMM) build failed, status " + std::to_string((int)r) }; Gpu::uavBarrier(l); }, gpuMs ? 3 : 1);
        if (gpuMs) *gpuMs = s.median;
        return b;
    }
    D3D12_RAYTRACING_GEOMETRY_OMM_LINKAGE_DESC link{}; link.OpacityMicromapIndexBuffer = { ommIndex, 4 }; link.OpacityMicromapIndexFormat = DXGI_FORMAT_R32_UINT; link.OpacityMicromapArray = ommArray;
    D3D12_RAYTRACING_GEOMETRY_DESC gd{}; gd.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_OMM_TRIANGLES; gd.OmmTriangles.pTriangles = &leaves; gd.OmmTriangles.pOmmLinkage = &link;
    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> gs; if (opaquePart) gs.push_back(*opaquePart); gs.push_back(gd);
    return buildBlas(gs, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE, gpuMs);
}

// Opacity micromap: the microtriangle index -> barycentric mapping (bird curve) is decoded empirically
// with 8 single-bit calibration micromaps so the classification below cannot be wrong by construction.
struct MicroTri { float u[3], v[3]; };
static bool decodeBirdCurve(Gpu::RtPipeline& pipe, std::vector<MicroTri>& out, std::string& note)
{
    const UINT level = 4, count = 1u << (2 * level), bits = 8;
    // 8 OMMs, 2-state, bit k of the index -> opaque
    std::vector<uint8_t> input(bits * 128, 0);
    for (UINT k = 0; k < bits; ++k) for (UINT i = 0; i < count; ++i) if ((i >> k) & 1) input[k * 128 + i / 8] |= (uint8_t)(1u << (i % 8));
    std::vector<D3D12_RAYTRACING_OPACITY_MICROMAP_DESC> descs(bits);
    for (UINT k = 0; k < bits; ++k) { descs[k].ByteOffset = k * 128; descs[k].SubdivisionLevel = level; descs[k].Format = D3D12_RAYTRACING_OPACITY_MICROMAP_FORMAT_OC1_2_STATE; }
    auto inBuf = g.buffer(input.size(), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_NONE); g.upload(inBuf.Get(), input.data(), input.size());
    auto descBuf = g.buffer(descs.size() * 8, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_NONE); g.upload(descBuf.Get(), descs.data(), descs.size() * 8);
    auto omm = buildOmmArray(inBuf.Get(), descBuf.Get(), bits, level, false);
    // one triangle, 8 BLAS each linked to OMM k
    float tri[9] = { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
    auto triBuf = g.buffer(sizeof tri, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_NONE); g.upload(triBuf.Get(), tri, sizeof tri);
    std::vector<uint32_t> idx(bits); for (UINT k = 0; k < bits; ++k) idx[k] = k;
    auto idxBuf = g.buffer(bits * 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_NONE); g.upload(idxBuf.Get(), idx.data(), bits * 4);
    std::vector<Blas> blas(bits); std::vector<D3D12_RAYTRACING_INSTANCE_DESC> inst;
    for (UINT k = 0; k < bits; ++k)
    {
        D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC t{}; t.VertexBuffer = { triBuf->GetGPUVirtualAddress(), 12 }; t.VertexCount = 3; t.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        blas[k] = buildBlasOmm(nullptr, t, idxBuf->GetGPUVirtualAddress() + k * 4, omm->GetGPUVirtualAddress(), bits, level, false, nullptr);
        inst.push_back(instance(4.f * k, 0, 0, 0, 1, blas[k].as->GetGPUVirtualAddress(), 0));
    }
    Tlas tlas = buildTlas(inst, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE, nullptr, g_useNvOmm);
    const UINT grid = 256;
    auto outBuf = g.buffer((UINT64)grid * grid * bits * 4); UINT outUav = g.uavStructured(outBuf.Get(), grid * grid * bits, 4);
    Consts c; c(0, 1) = outUav; c(1, 0) = grid; c(1, 1) = 4; c(1, 2) = asu(4.f); c(3, 3) = 0; c(4, 3) = 1; // hit group 1 (accept)
    { auto l = g.begin(); g.setConstants(l, c.v); l->SetComputeRootShaderResourceView(1, tlas.as->GetGPUVirtualAddress()); g.dispatchRays(l, pipe, grid, grid, bits); g.submitAndWait(); }
    auto data = g.readback(outBuf.Get(), (UINT64)grid * grid * bits * 4);
    const float* t = (const float*)data.data();
    std::vector<double> cu(count, 0), cv(count, 0); std::vector<int> n(count, 0);
    for (UINT y = 0; y < grid; ++y) for (UINT x = 0; x < grid; ++x)
    {
        float u = (x + 0.5f) / grid, v = (y + 0.5f) / grid;
        if (u + v > 0.995f) continue;
        UINT index = 0; for (UINT k = 0; k < bits; ++k) if (t[((size_t)k * grid + y) * grid + x] > 0) index |= 1u << k;
        cu[index] += u; cv[index] += v; n[index]++;
    }
    out.assign(count, {});
    const float N = (float)(1u << level);
    int missing = 0;
    for (UINT i = 0; i < count; ++i)
    {
        if (n[i] == 0) { missing++; continue; }
        float mu = (float)(cu[i] / n[i]) * N, mv = (float)(cv[i] / n[i]) * N;
        int a = (int)floorf(mu), b = (int)floorf(mv); float fu = mu - a, fv = mv - b;
        MicroTri m;
        if (fu + fv < 1.f) { m.u[0] = a / N; m.v[0] = b / N; m.u[1] = (a + 1) / N; m.v[1] = b / N; m.u[2] = a / N; m.v[2] = (b + 1) / N; }
        else { m.u[0] = (a + 1) / N; m.v[0] = b / N; m.u[1] = a / N; m.v[1] = (b + 1) / N; m.u[2] = (a + 1) / N; m.v[2] = (b + 1) / N; }
        out[i] = m;
    }
    // uniqueness: every microtriangle must decode once
    std::map<std::pair<int, int>, int> seen;
    for (UINT i = 0; i < count; ++i) if (n[i]) seen[{ (int)lroundf((out[i].u[0] + out[i].u[1] + out[i].u[2]) * N * 3), (int)lroundf((out[i].v[0] + out[i].v[1] + out[i].v[2]) * N * 3) }]++;
    char b[256]; snprintf(b, sizeof b, "bird-curve decode: %u indices, %d missing, %zu distinct cells", count, missing, seen.size());
    note = b;
    return missing == 0 && seen.size() == count;
}

static void buildRtScene(Gpu::RtPipeline* calibrationPipe)
{
    logf("\n== Building ray-tracing scene (2 km terrain, 100k trees, 3k buildings, 1M grass; %s)\n", g_thinFoliage ? "THIN foliage: 6 cm leaves x 40k, 4 mm grass blades" : "card foliage: 35 cm leaves x 1.5k, 30 cm grass cards");
    std::mt19937 rng(7); std::uniform_real_distribution<float> U(0.f, 1.f);
    // terrain 1024x1024 quads
    {
        const UINT n = 1024; std::vector<float> pos; pos.reserve((size_t)(n + 1) * (n + 1) * 3); std::vector<uint32_t> idx; idx.reserve((size_t)n * n * 6);
        for (UINT z = 0; z <= n; ++z) for (UINT x = 0; x <= n; ++x) { float fx = S.sceneMin + x * (S.sceneSize / n), fz = S.sceneMin + z * (S.sceneSize / n); pos.push_back(fx); pos.push_back(terrainHeight(fx, fz)); pos.push_back(fz); }
        for (UINT z = 0; z < n; ++z) for (UINT x = 0; x < n; ++x) { uint32_t a = z * (n + 1) + x; idx.insert(idx.end(), { a, a + 1, a + n + 1, a + 1, a + n + 2, a + n + 1 }); }
        S.terrainPos = g.buffer(pos.size() * 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_NONE); g.upload(S.terrainPos.Get(), pos.data(), pos.size() * 4);
        S.terrainIdx = g.buffer(idx.size() * 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_NONE); g.upload(S.terrainIdx.Get(), idx.data(), idx.size() * 4);
        double ms; S.terrain = buildBlas({ triGeom(S.terrainPos->GetGPUVirtualAddress(), (UINT)(pos.size() / 3), S.terrainIdx->GetGPUVirtualAddress(), (UINT)idx.size(), true) }, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE, &ms);
        record("rtas", "BLAS build terrain 2.1M tris (fast trace)", ms, "ms", std::to_string(S.terrain.size >> 20) + " MB");
    }
    // city: 3000 boxes
    {
        std::vector<float> pos;
        auto box = [&](float x, float y, float z, float sx, float sy, float sz) {
            float v[8][3] = { {x,y,z},{x + sx,y,z},{x + sx,y + sy,z},{x,y + sy,z},{x,y,z + sz},{x + sx,y,z + sz},{x + sx,y + sy,z + sz},{x,y + sy,z + sz} };
            int f[12][3] = { {0,1,2},{0,2,3},{4,6,5},{4,7,6},{0,4,5},{0,5,1},{1,5,6},{1,6,2},{2,6,7},{2,7,3},{3,7,4},{3,4,0} };
            for (auto& t : f) for (int k : t) pos.insert(pos.end(), v[k], v[k] + 3); };
        for (int i = 0; i < 3000; ++i) { float x = -300 + U(rng) * 600, z = -300 + U(rng) * 600; box(x, terrainHeight(x, z) - 1, z, 8 + U(rng) * 20, 8 + U(rng) * 40, 8 + U(rng) * 20); }
        S.cityPos = g.buffer(pos.size() * 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_NONE); g.upload(S.cityPos.Get(), pos.data(), pos.size() * 4);
        double ms; S.city = buildBlas({ triGeom(S.cityPos->GetGPUVirtualAddress(), (UINT)(pos.size() / 3), 0, 0, true) }, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE, &ms);
        record("rtas", "BLAS build city 36k tris", ms, "ms");
    }
    // leaf alpha texture 256^2 R8 with mips
    const UINT ts = 256; std::vector<std::vector<uint8_t>> lv(9);
    auto leafAlpha = [](float u, float v) {
        float du = u - 0.5f, dv = v - 0.5f; float ca = cosf(0.5f), sa = sinf(0.5f);
        float a = (du * ca + dv * sa) / 0.42f, b = (-du * sa + dv * ca) / 0.28f;
        float r = sqrtf(a * a + b * b), th = atan2f(b, a);
        return r < 1.f + 0.12f * sinf(9.f * th) ? 1.f : 0.f; };
    lv[0].resize((size_t)ts * ts); for (UINT y = 0; y < ts; ++y) for (UINT x = 0; x < ts; ++x) lv[0][(size_t)y * ts + x] = (uint8_t)(255 * leafAlpha((x + 0.5f) / ts, (y + 0.5f) / ts));
    for (UINT m = 1; m < 9; ++m) { UINT w = ts >> m, pw = ts >> (m - 1); lv[m].resize((size_t)w * w); for (UINT y = 0; y < w; ++y) for (UINT x = 0; x < w; ++x) lv[m][(size_t)y * w + x] = (uint8_t)((lv[m - 1][(size_t)(2 * y) * pw + 2 * x] + lv[m - 1][(size_t)(2 * y) * pw + 2 * x + 1] + lv[m - 1][(size_t)(2 * y + 1) * pw + 2 * x] + lv[m - 1][(size_t)(2 * y + 1) * pw + 2 * x + 1]) / 4); }
    S.alphaTex = g.texture2D(ts, ts, DXGI_FORMAT_R8_UNORM, 9, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE); g.uploadTexture(S.alphaTex.Get(), lv, ts, ts, 1);
    S.alphaSrv = g.srvTexture(S.alphaTex.Get(), DXGI_FORMAT_R8_UNORM, 9);
    double coverage = 0; for (auto b : lv[0]) coverage += b / 255.0; coverage /= lv[0].size();
    // canonical per-primitive UVs (quad tri 0: (0,0)(1,0)(1,1), tri 1: (0,0)(1,1)(0,1))
    const int leafQuads = g_thinFoliage ? 40000 : 1500; S.leafTrisPerTree = leafQuads * 2;
    const float leafHalf = g_thinFoliage ? 0.03f : 0.35f;
    std::vector<float> uv; for (int i = 0; i < leafQuads; ++i) { uv.insert(uv.end(), { 0,0, 1,0, 1,1 }); uv.insert(uv.end(), { 0,0, 1,1, 0,1 }); }
    S.uvs = g.buffer(uv.size() * 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_NONE); g.upload(S.uvs.Get(), uv.data(), uv.size() * 4);
    S.uvSrv = g.srvStructured(S.uvs.Get(), (UINT)(uv.size() / 2), 8);
    // OMM for the two canonical leaf triangles (level 4, 4-state), classified from the alpha texture
    std::vector<MicroTri> curve;
    if ((g.rtTier >= D3D12_RAYTRACING_TIER_1_2 || g_useNvOmm) && calibrationPipe)
    {
        try
        {
            S.ommOk = decodeBirdCurve(*calibrationPipe, curve, S.ommNote);
            logf("  %s\n", S.ommNote.c_str());
        }
        catch (Failure& f) { S.ommOk = false; S.ommNote = f.what; logf("  OMM calibration failed: %s\n", f.what.c_str()); }
    }
    else S.ommNote = "raytracing tier < 1.2 and no NVAPI OMM cap, OMM unavailable";
    if (S.ommOk)
    {
        const UINT level = 4, count = 256;
        std::vector<uint8_t> input(2 * 128, 0); int unknown = 0;
        for (int tri = 0; tri < 2; ++tri) for (UINT i = 0; i < count; ++i)
        {
            const MicroTri& m = curve[i]; int opaque = 0, transparent = 0;
            for (int a = 0; a < 8; ++a) for (int b = 0; b < 8 - a; ++b)
            {
                float bu = (a + 0.33f) / 8.f, bv = (b + 0.33f) / 8.f; // barycentric sample inside the microtriangle
                float u = m.u[0] * (1 - bu - bv) + m.u[1] * bu + m.u[2] * bv, v = m.v[0] * (1 - bu - bv) + m.v[1] * bu + m.v[2] * bv;
                float tu = tri == 0 ? u + v : u, tv = tri == 0 ? v : u + v;
                (leafAlpha(tu, tv) > 0.5f ? opaque : transparent)++;
            }
            uint8_t state = (transparent == 0) ? 1 : (opaque == 0) ? 0 : 3; // opaque / transparent / unknown-opaque
            if (state == 3) unknown++;
            input[tri * 128 + i / 4] |= (uint8_t)(state << (2 * (i % 4)));
        }
        S.unknownFraction = unknown / 512.0;
        std::vector<D3D12_RAYTRACING_OPACITY_MICROMAP_DESC> descs(2);
        for (int k = 0; k < 2; ++k) { descs[k].ByteOffset = k * 128; descs[k].SubdivisionLevel = level; descs[k].Format = D3D12_RAYTRACING_OPACITY_MICROMAP_FORMAT_OC1_4_STATE; }
        S.ommInput = g.buffer(input.size(), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_NONE); g.upload(S.ommInput.Get(), input.data(), input.size());
        S.ommDescs = g.buffer(16, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_NONE); g.upload(S.ommDescs.Get(), descs.data(), 16);
        S.ommArray = buildOmmArray(S.ommInput.Get(), S.ommDescs.Get(), 2, level, true);
        logf("  leaf alpha coverage %.2f, OMM level 4: %.1f%% microtriangles unknown (need any-hit), %.1f%% resolved\n", coverage, 100 * S.unknownFraction, 100 * (1 - S.unknownFraction));
    }
    // trees: trunk (opaque, indexed) + leaves (non-opaque, soup). 4 variants; OMM-linked twins.
    std::vector<uint32_t> ommIdx(S.leafTrisPerTree); for (int i = 0; i < S.leafTrisPerTree; ++i) ommIdx[i] = i & 1;
    auto ommIdxBuf = g.buffer(ommIdx.size() * 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_NONE); g.upload(ommIdxBuf.Get(), ommIdx.data(), ommIdx.size() * 4);
    static ComPtr<ID3D12Resource> keepOmmIdx = ommIdxBuf;
    for (int t = 0; t < 4; ++t)
    {
        std::vector<float> pos; std::vector<uint32_t> idx;
        const int seg = 16, rings = 8; float H = 8.f;
        for (int r = 0; r <= rings; ++r) for (int s = 0; s < seg; ++s) { float a = s * 6.2831853f / seg, rad = 0.35f * (1.f - 0.6f * r / rings); pos.insert(pos.end(), { rad * cosf(a), H * r / rings, rad * sinf(a) }); }
        for (int r = 0; r < rings; ++r) for (int s = 0; s < seg; ++s) { uint32_t a = r * seg + s, b = r * seg + (s + 1) % seg, c = a + seg, d = b + seg; idx.insert(idx.end(), { a, c, b, b, c, d }); }
        S.trunkTris = (int)idx.size() / 3;
        size_t trunkVerts = pos.size() / 3;
        for (int q = 0; q < leafQuads; ++q)
        {
            float cx, cy, cz; do { cx = U(rng) * 2 - 1; cy = U(rng) * 2 - 1; cz = U(rng) * 2 - 1; } while (cx * cx + cy * cy + cz * cz > 1);
            cx *= 3.5f; cy = 7.f + cy * 3.0f; cz *= 3.5f;
            float yaw = U(rng) * 6.2831853f, pitch = U(rng) * 3.1415926f; float e = leafHalf;
            float ax = cosf(yaw) * cosf(pitch), ay = sinf(pitch), az = sinf(yaw) * cosf(pitch); // quad axis 1
            float bx = -sinf(yaw), by = 0, bz = cosf(yaw);                                     // quad axis 2
            float p[4][3] = { { cx - (ax + bx) * e, cy - (ay + by) * e, cz - (az + bz) * e }, { cx + (ax - bx) * e, cy + (ay - by) * e, cz + (az - bz) * e },
                              { cx + (ax + bx) * e, cy + (ay + by) * e, cz + (az + bz) * e }, { cx - (ax - bx) * e, cy - (ay - by) * e, cz - (az - bz) * e } };
            pos.insert(pos.end(), p[0], p[0] + 3); pos.insert(pos.end(), p[1], p[1] + 3); pos.insert(pos.end(), p[2], p[2] + 3);
            pos.insert(pos.end(), p[0], p[0] + 3); pos.insert(pos.end(), p[2], p[2] + 3); pos.insert(pos.end(), p[3], p[3] + 3);
        }
        S.treePos[t] = g.buffer(pos.size() * 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_NONE); g.upload(S.treePos[t].Get(), pos.data(), pos.size() * 4);
        static ComPtr<ID3D12Resource> trunkIdx[4]; trunkIdx[t] = g.buffer(idx.size() * 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_NONE); g.upload(trunkIdx[t].Get(), idx.data(), idx.size() * 4);
        D3D12_GPU_VIRTUAL_ADDRESS pva = S.treePos[t]->GetGPUVirtualAddress();
        auto trunk = triGeom(pva, (UINT)trunkVerts, trunkIdx[t]->GetGPUVirtualAddress(), (UINT)idx.size(), true);
        auto leaves = triGeom(pva + trunkVerts * 12, (UINT)(S.leafTrisPerTree * 3), 0, 0, false);
        double ms; S.tree[t] = buildBlas({ trunk, leaves }, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE, t == 0 ? &ms : nullptr);
        if (t == 0) record("rtas", std::string("BLAS build tree ") + std::to_string((S.trunkTris + S.leafTrisPerTree) / 1000) + "k tris (fast trace)", ms, "ms", std::to_string(S.tree[0].size >> 10) + " KB");
        if (S.ommOk)
        {
            S.treeOmm[t] = buildBlasOmm(&trunk, leaves.Triangles, ommIdxBuf->GetGPUVirtualAddress(), S.ommArray->GetGPUVirtualAddress(), 2, 4, true, t == 0 ? &ms : nullptr);
            if (t == 0) record("rtas", "BLAS build tree with OMM leaves (fast trace)", ms, "ms", std::to_string(S.treeOmm[0].size >> 10) + " KB");
        }
    }
    // grass clump: 8 alpha quads
    {
        std::vector<float> pos;
        const int blades = g_thinFoliage ? 100 : 8; const float bw = g_thinFoliage ? 0.002f : 0.15f, bh = g_thinFoliage ? 0.5f : 0.5f;
        for (int q = 0; q < blades; ++q)
        {
            float cx = U(rng) - 0.5f, cz = U(rng) - 0.5f, yaw = U(rng) * 6.28f, w = bw, h = bh * (0.6f + 0.8f * U(rng)); float bx = cosf(yaw) * w, bz = sinf(yaw) * w;
            float lean = g_thinFoliage ? 0.1f * (U(rng) - 0.5f) : 0.f;
            float p[4][3] = { { cx - bx, 0, cz - bz }, { cx + bx, 0, cz + bz }, { cx + bx * 0.3f + lean, h, cz + bz * 0.3f }, { cx - bx * 0.3f + lean, h, cz - bz * 0.3f } };
            pos.insert(pos.end(), p[0], p[0] + 3); pos.insert(pos.end(), p[1], p[1] + 3); pos.insert(pos.end(), p[2], p[2] + 3);
            pos.insert(pos.end(), p[0], p[0] + 3); pos.insert(pos.end(), p[2], p[2] + 3); pos.insert(pos.end(), p[3], p[3] + 3);
        }
        S.grassPos = g.buffer(pos.size() * 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_NONE); g.upload(S.grassPos.Get(), pos.data(), pos.size() * 4);
        // thin blades are true geometry (opaque); cards are alpha tested
        S.grass = buildBlas({ triGeom(S.grassPos->GetGPUVirtualAddress(), (UINT)(pos.size() / 3), 0, 0, g_thinFoliage) }, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE);
    }
    // instances
    for (int i = 0; i < 100000; ++i) { float x = S.sceneMin + U(rng) * S.sceneSize, z = S.sceneMin + U(rng) * S.sceneSize; S.instancesTrees.push_back(instance(x, terrainHeight(x, z) - 0.2f, z, U(rng) * 6.28f, 0.8f + U(rng) * 0.5f, S.tree[i & 3].as->GetGPUVirtualAddress(), 0)); S.instancesTrees.back().InstanceID = i & 3; }
    for (int i = 0; i < 1000000; ++i) { float x = S.sceneMin + U(rng) * S.sceneSize, z = S.sceneMin + U(rng) * S.sceneSize; S.instancesGrass.push_back(instance(x, terrainHeight(x, z), z, U(rng) * 6.28f, 0.7f + U(rng) * 0.5f, S.grass.as->GetGPUVirtualAddress(), 0)); }
    // character-like BLAS x64 for refit timing (60k tris)
    {
        const int R = 200, C = 150; std::vector<float> pos; std::vector<uint32_t> idx;
        for (int r = 0; r <= R; ++r) for (int c = 0; c < C; ++c) { float th = 3.14159f * r / R, ph = 6.28318f * c / C; pos.insert(pos.end(), { 0.4f * sinf(th) * cosf(ph), 1.f + 0.9f * cosf(th), 0.25f * sinf(th) * sinf(ph) }); }
        for (int r = 0; r < R; ++r) for (int c = 0; c < C; ++c) { uint32_t a = r * C + c, b = r * C + (c + 1) % C, cc = a + C, d = b + C; idx.insert(idx.end(), { a, cc, b, b, cc, d }); }
        S.charPos = g.buffer(pos.size() * 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_NONE); g.upload(S.charPos.Get(), pos.data(), pos.size() * 4);
        S.charIdx = g.buffer(idx.size() * 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_NONE); g.upload(S.charIdx.Get(), idx.data(), idx.size() * 4);
        auto geom = triGeom(S.charPos->GetGPUVirtualAddress(), (UINT)(pos.size() / 3), S.charIdx->GetGPUVirtualAddress(), (UINT)idx.size(), true);
        std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> gs{ geom };
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{}; in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL; in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE; in.NumDescs = 1; in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY; in.pGeometryDescs = gs.data();
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{}; g.dev->GetRaytracingAccelerationStructurePrebuildInfo(&in, &info);
        static ComPtr<ID3D12Resource> scratch = g.buffer(std::max(info.ScratchDataSizeInBytes, info.UpdateScratchDataSizeInBytes) * 64);
        std::vector<D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC> builds(64);
        for (int i = 0; i < 64; ++i)
        {
            S.chars[i].as = g.buffer(info.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE); S.chars[i].size = info.ResultDataMaxSizeInBytes;
            builds[i].DestAccelerationStructureData = S.chars[i].as->GetGPUVirtualAddress(); builds[i].Inputs = in; builds[i].ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress() + i * std::max(info.ScratchDataSizeInBytes, info.UpdateScratchDataSizeInBytes);
        }
        Stat b = g.time([&](ID3D12GraphicsCommandList6* l) { for (int i = 0; i < 64; ++i) l->BuildRaytracingAccelerationStructure(&builds[i], 0, nullptr); Gpu::uavBarrier(l); }, 3);
        record("rtas", "BLAS full build 60k-tri skinned mesh (allow update), per mesh, 64 in flight", b.median * 1000 / 64, "us", std::to_string(info.ResultDataMaxSizeInBytes >> 10) + " KB each");
        for (int i = 0; i < 64; ++i) { builds[i].Inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE; builds[i].SourceAccelerationStructureData = builds[i].DestAccelerationStructureData; }
        Stat u = g.time([&](ID3D12GraphicsCommandList6* l) { for (int i = 0; i < 64; ++i) l->BuildRaytracingAccelerationStructure(&builds[i], 0, nullptr); Gpu::uavBarrier(l); }, 5);
        record("rtas", "BLAS refit 60k-tri skinned mesh, per mesh, 64 in flight", u.median * 1000 / 64, "us", "256 characters = " + std::to_string(u.median * 4) + " ms (expected, linear)");
    }
}

static bool g_onlySer = false, g_onlyEdges = false;
static void testRays(bool trySer)
{
    warmUp("Ray tracing");
    if (g.rtTier < D3D12_RAYTRACING_TIER_1_1) { logf("  raytracing tier < 1.1, skipped\n"); return; }
    std::string lib = g_common + readFile(g_shaderDir + "/raylib.hlsl");
    double msLib; auto libBlob = dxc.compile(lib, nullptr, L"lib_6_6", {}, &msLib);
    double msSo; Gpu::RtPipeline pipe = g.rtPso(libBlob.Get(), false, false, &msSo);
    record("pso", "raytracing state object create, 5 shaders (cpu)", msSo, "ms", "dxc " + std::to_string(msLib) + " ms");
    g_useNvOmm = (g.rtTier < D3D12_RAYTRACING_TIER_1_2) && g_nvOmm;
    logf("  OMM path: %s\n", g.rtTier >= D3D12_RAYTRACING_TIER_1_2 ? "DXR 1.2" : g_useNvOmm ? "NVAPI (driver exposes OMM without DXR 1.2)" : "none");
    Gpu::RtPipeline pipeOmm; bool haveOmmPipe = false;
    if (g.rtTier >= D3D12_RAYTRACING_TIER_1_2 || g_useNvOmm)
    {
        try { pipeOmm = g.rtPso(libBlob.Get(), true); haveOmmPipe = true; }
        catch (Failure& f) { logf("  OMM pipeline creation failed: %s\n", f.what.c_str()); }
    }
    buildRtScene(haveOmmPipe ? &pipeOmm : nullptr);
    std::string rq = g_common + readFile(g_shaderDir + "/rays.hlsl");
    auto psoOpaque = g.computePso(dxc.compile(rq, L"RayCS", L"cs_6_6", { L"RQ_FLAGS=RAY_FLAG_FORCE_OPAQUE" }).Get());
    auto psoOpaqueFirst = g.computePso(dxc.compile(rq, L"RayCS", L"cs_6_6", { L"RQ_FLAGS=RAY_FLAG_FORCE_OPAQUE|RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH" }).Get());
    auto psoAlpha = g.computePso(dxc.compile(rq, L"RayCS", L"cs_6_6", { L"RQ_FLAGS=RAY_FLAG_NONE", L"ALPHA=1" }).Get());
    auto psoAlphaFirst = g.computePso(dxc.compile(rq, L"RayCS", L"cs_6_6", { L"RQ_FLAGS=RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH", L"ALPHA=1" }).Get());
    auto psoNone = g.computePso(dxc.compile(rq, L"RayCS", L"cs_6_6", { L"RQ_FLAGS=RAY_FLAG_NONE" }).Get());

    // TLAS variants
    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> base; base.push_back(instance(0, 0, 0, 0, 1, S.terrain.as->GetGPUVirtualAddress(), 0)); base.push_back(instance(0, 0, 0, 0, 1, S.city.as->GetGPUVirtualAddress(), 0));
    auto withTrees = [&](bool omm, UINT flags) { auto v = base; for (auto d : S.instancesTrees) { d.AccelerationStructure = (omm ? S.treeOmm[d.InstanceID] : S.tree[d.InstanceID]).as->GetGPUVirtualAddress(); d.Flags = flags; v.push_back(d); } return v; };
    double ms;
    Tlas tAlpha = buildTlas(withTrees(false, 0), D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE, &ms);
    record("rtas", "TLAS build 100k instances (fast trace)", ms, "ms", std::to_string(tAlpha.size >> 20) + " MB");
    Tlas tOpaque = buildTlas(withTrees(false, D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE), D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE);
    {
        Tlas tb = buildTlas(withTrees(false, 0), D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD, &ms);
        record("rtas", "TLAS build 100k instances (fast build)", ms, "ms");
        Tlas tu = buildTlas(withTrees(false, 0), D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE, &ms);
        record("rtas", "TLAS build 100k instances (fast trace + allow update)", ms, "ms");
        record("rtas", "TLAS refit 100k instances", refitTlas(tu, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE), "ms");
    }
    Tlas tOmm4, tOmm2;
    if (S.ommOk) { tOmm4 = buildTlas(withTrees(true, 0), D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE, nullptr, g_useNvOmm); tOmm2 = buildTlas(withTrees(true, D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OMM_2_STATE), D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE, nullptr, g_useNvOmm); }
    auto grassInst = [&](UINT flags) { auto v = withTrees(false, flags); for (auto d : S.instancesGrass) { d.Flags = flags; v.push_back(d); } return v; };
    Tlas tGrassAlpha, tGrassOpaque;
    if (!g_onlySer || g_onlyEdges)
    {
        tGrassAlpha = buildTlas(grassInst(0), D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE, &ms);
        record("rtas", "TLAS build 1.1M instances (fast trace)", ms, "ms", std::to_string(tGrassAlpha.size >> 20) + " MB");
        tGrassOpaque = buildTlas(grassInst(D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE), D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE);
        Tlas tu = buildTlas(grassInst(0), D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE, &ms);
        record("rtas", "TLAS build 1.1M instances (fast trace + allow update)", ms, "ms");
        record("rtas", "TLAS refit 1.1M instances", refitTlas(tu, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE), "ms");
    }

    const UINT W = 2048, H = 2048; const UINT W4 = 3840, H4 = 2160;
    auto outBuf = g.buffer((UINT64)W4 * H4 * 4); UINT outUav = g.uavStructured(outBuf.Get(), W4 * H4, 4);
    struct Mode { const char* name; uint32_t mode; float tmax; UINT w, h; };
    Mode modes[] = { { "incoherent long (canopy origins, sphere dirs, 2 km)", 0, 2000.f, W, H }, { "incoherent short (same, tMax 2 m)", 1, 2.f, W, H },
                     { "primary 3840x2160 camera", 2, 5000.f, W4, H4 }, { "sun shadow from terrain (0.25 deg jitter)", 3, 2000.f, W, H } };
    float sun[3] = { 0.5f, 0.64f, 0.58f }; float sl = sqrtf(sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2]); for (float& v : sun) v /= sl;
    // 16 material textures (2048^2 RGBA8 noise, 256 MB total) for the divergent hit-shading tests
    UINT matBase = 0;
    {
        std::vector<ComPtr<ID3D12Resource>>* keep = new std::vector<ComPtr<ID3D12Resource>>();
        for (int i = 0; i < 16; ++i)
        {
            std::vector<std::vector<uint8_t>> lv(1); lv[0].resize((size_t)2048 * 2048 * 4); uint32_t r = 77 + i; for (auto& b : lv[0]) { r = r * 1664525u + 1013904223u; b = (uint8_t)(r >> 24); }
            auto tex = g.texture2D(2048, 2048, DXGI_FORMAT_R8G8B8A8_UNORM, 1, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE); g.uploadTexture(tex.Get(), lv, 2048, 2048, 4);
            UINT srv = g.srvTexture(tex.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 1); if (i == 0) matBase = srv; keep->push_back(tex);
        }
    }
    auto consts = [&](const Mode& m, uint32_t rayFlags, uint32_t hitGroup) {
        Consts c; c(0, 1) = outUav; c(0, 2) = S.alphaSrv; c(0, 3) = S.uvSrv; c(1, 0) = m.w; c(1, 1) = m.mode; c(1, 2) = asu(m.tmax); c(1, 3) = 3; c(5, 3) = matBase;
        c(2, 0) = asu(S.sceneMin); c(2, 1) = asu(S.sceneMin); c(2, 2) = asu(S.sceneSize); c(2, 3) = m.h;
        c(3, 0) = asu(sun[0]); c(3, 1) = asu(sun[1]); c(3, 2) = asu(sun[2]); c(3, 3) = rayFlags;
        float cx = 0, cz = 0; c(4, 0) = asu(cx); c(4, 1) = asu(terrainHeight(cx, cz) + 1.7f); c(4, 2) = asu(cz); c(4, 3) = hitGroup;
        c(5, 0) = asu(0.9848f); c(5, 1) = asu(-0.1736f); c(5, 2) = asu(0.f); // forward, 10 deg down
        c(6, 0) = asu(0.f); c(6, 1) = asu(0.f); c(6, 2) = asu(1.f);           // right
        c(7, 0) = asu(0.1736f); c(7, 1) = asu(0.9848f); c(7, 2) = asu(0.f); c(7, 3) = asu(tanf(30.f * 3.14159f / 180.f)); // up, tan(fov/2)
        return c; };
    auto runRq = [&](const char* label, ID3D12PipelineState* pso, Tlas& t, const Mode& m) {
        Consts c = consts(m, 0, 0);
        Stat s = g.time([&](ID3D12GraphicsCommandList6* l) { g.setConstants(l, c.v); l->SetComputeRootShaderResourceView(1, t.as->GetGPUVirtualAddress()); l->SetPipelineState(pso); l->Dispatch(m.w / 8, m.h / 8, 1); });
        double rays = (double)m.w * m.h;
        record("rays", std::string(label) + " | " + m.name + (g_thinFoliage ? " [thin foliage]" : ""), rays / (s.median * 1e-3) / 1e9, "Grays/s", std::to_string(s.median) + " ms for " + std::to_string((int)(rays / 1e6)) + " M rays"); };
    auto runDr = [&](const char* label, Gpu::RtPipeline& p, Tlas& t, const Mode& m, uint32_t rayFlags) {
        Consts c = consts(m, rayFlags, 0);
        Stat s = g.time([&](ID3D12GraphicsCommandList6* l) { g.setConstants(l, c.v); l->SetComputeRootShaderResourceView(1, t.as->GetGPUVirtualAddress()); g.dispatchRays(l, p, m.w, m.h, 1); });
        double rays = (double)m.w * m.h;
        record("rays", std::string(label) + " | " + m.name + (g_thinFoliage ? " [thin foliage]" : ""), rays / (s.median * 1e-3) / 1e9, "Grays/s", std::to_string(s.median) + " ms"); };
    // hit-rate sanity: fraction of primary rays that hit (readback once)
    {
        Consts c = consts(modes[2], 0, 0);
        auto l = g.begin(); g.setConstants(l, c.v); l->SetComputeRootShaderResourceView(1, tAlpha.as->GetGPUVirtualAddress()); l->SetPipelineState(psoAlpha.Get()); l->Dispatch(W4 / 8, H4 / 8, 1); g.submitAndWait();
        auto d = g.readback(outBuf.Get(), (UINT64)W4 * H4 * 4); const float* t = (const float*)d.data(); size_t hits = 0; for (size_t i = 0; i < (size_t)W4 * H4; ++i) hits += t[i] > 0;
        record("rays", "sanity: primary alpha-tested hit fraction", hits / (double)((size_t)W4 * H4), "fraction");
    }
    if (g_onlyEdges)
    {
        // 16 stratified primary sub-samples per 4K pixel: how many pixels hold more than one surface identity, and how
        // much of that is invisible to a 1-sample visibility buffer plus its 3x3 identity neighbourhood.
        auto psoEdge = g.computePso(dxc.compile(rq, L"EdgeCountCS", L"cs_6_6", { L"ALPHA=1" }).Get());
        auto centres = g.buffer((UINT64)W4 * H4 * 4); UINT centresUav = g.uavStructured(centres.Get(), W4 * H4, 4), centresSrv = g.srvStructured(centres.Get(), W4 * H4, 4);
        auto counts = g.buffer((UINT64)W4 * H4 * 4); UINT countsUav = g.uavStructured(counts.Get(), W4 * H4, 4);
        struct View { const char* name; float cx, cz; };
        View views[] = { { "forest view (camera 600,600 looking +x)", 600.f, 600.f }, { "city view (camera 0,0)", 0.f, 0.f } };
        for (const View& v : views) for (int withGrass = 1; withGrass >= 0; --withGrass)
        {
            Tlas& t = withGrass ? tGrassAlpha : tAlpha;
            if (!t.as) continue;
            Consts c = consts(modes[2], 0, 0);
            c(4, 0) = asu(v.cx); c(4, 1) = asu(terrainHeight(v.cx, v.cz) + 1.7f); c(4, 2) = asu(v.cz);
            c(0, 1) = centresUav; c(1, 3) = 0;
            { auto l = g.begin(); g.setConstants(l, c.v); l->SetComputeRootShaderResourceView(1, t.as->GetGPUVirtualAddress()); l->SetPipelineState(psoEdge.Get()); l->Dispatch(W4 / 8, H4 / 8, 1); g.submitAndWait(); }
            c(0, 1) = countsUav; c(1, 3) = 1; c(3, 3) = centresSrv;
            Stat st = g.time([&](ID3D12GraphicsCommandList6* l) { g.setConstants(l, c.v); l->SetComputeRootShaderResourceView(1, t.as->GetGPUVirtualAddress()); l->SetPipelineState(psoEdge.Get()); l->Dispatch(W4 / 8, H4 / 8, 1); }, 3);
            auto data = g.readback(counts.Get(), (UINT64)W4 * H4 * 4); const uint32_t* d = (const uint32_t*)data.data();
            size_t n = (size_t)W4 * H4, ge2 = 0, ge3 = 0, ge5 = 0, ge9 = 0, miss1 = 0, miss4 = 0, miss8 = 0;
            for (size_t i = 0; i < n; ++i) { uint32_t k = d[i] & 255, m = d[i] >> 8; ge2 += k >= 2; ge3 += k >= 3; ge5 += k >= 5; ge9 += k >= 9; miss1 += m >= 1; miss4 += m >= 4; miss8 += m >= 8; }
            std::string tag = std::string(v.name) + (withGrass ? ", 100k trees + 1M grass" : ", 100k trees") + (g_thinFoliage ? " [thin foliage]" : " [card foliage]");
            record("edges", tag + ": pixels with >=2 identities in 16 sub-samples", ge2 / (double)n, "fraction", std::to_string(st.median) + " ms for 133M rays");
            record("edges", tag + ": >=3 identities", ge3 / (double)n, "fraction");
            record("edges", tag + ": >=5 identities", ge5 / (double)n, "fraction");
            record("edges", tag + ": >=9 identities", ge9 / (double)n, "fraction");
            record("edges", tag + ": pixels with sub-samples missed by the 3x3 centre set (>=1 of 16)", miss1 / (double)n, "fraction", "invisible to a 1-spp visibility buffer + 3x3 identity test");
            record("edges", tag + ": missed >=4 of 16 (>=25% pixel area)", miss4 / (double)n, "fraction");
            record("edges", tag + ": missed >=8 of 16 (>=50% pixel area)", miss8 / (double)n, "fraction");
        }
        return;
    }
    const uint32_t FIRST = 0x04 | 0x08; // RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER
    for (const Mode& m : modes)
    {
        if (g_onlySer) break;
        bool shadow = m.mode == 3;
        runRq("RayQuery opaque closest", psoOpaque.Get(), tOpaque, m);
        if (shadow) runRq("RayQuery opaque first-hit", psoOpaqueFirst.Get(), tOpaque, m);
        runRq("RayQuery alpha-test any-hit loop", psoAlpha.Get(), tAlpha, m);
        if (shadow) runRq("RayQuery alpha-test first-hit", psoAlphaFirst.Get(), tAlpha, m);
        runDr("DispatchRays opaque", pipe, tOpaque, m, shadow ? FIRST : 0);
        runDr("DispatchRays alpha any-hit shader", pipe, tAlpha, m, shadow ? FIRST : 0);
        if (S.ommOk)
        {
            runDr("DispatchRays OMM 4-state (unknown -> any-hit)", pipeOmm, tOmm4, m, shadow ? FIRST : 0);
            runDr("DispatchRays OMM forced 2-state (no any-hit)", pipeOmm, tOmm2, m, shadow ? FIRST : 0);
        }
    }
    if (!g_onlySer)
    {
        if (S.ommOk) runDr("DispatchRays alpha any-hit shader, OMM-enabled pipeline (flag cost)", pipeOmm, tAlpha, modes[0], 0);
        runRq("RayQuery opaque closest, +1M grass", psoOpaque.Get(), tGrassOpaque, modes[0]);
        runRq("RayQuery alpha any-hit loop, +1M grass", psoAlpha.Get(), tGrassAlpha, modes[0]);
        runRq("RayQuery opaque first-hit, +1M grass", psoOpaqueFirst.Get(), tGrassOpaque, modes[3]);
        runRq("RayQuery alpha first-hit, +1M grass", psoAlphaFirst.Get(), tGrassAlpha, modes[3]);
        runRq("RayQuery opaque closest, +1M grass", psoOpaque.Get(), tGrassOpaque, modes[2]);
        // no-flag opaque geometry (leaves flagged non-opaque but no candidate handling) shows pure traversal without commits
        runRq("RayQuery no-flags, non-opaque leaves ignored (traversal only)", psoNone.Get(), tAlpha, modes[0]);
    }
    if (g_onlySer)
    {
        runDr("DispatchRays opaque trivial hit (reference)", pipe, tOpaque, modes[0], 0);
        runDr("DispatchRays opaque trivial hit (reference)", pipe, tOpaque, modes[2], 0);
        runDr("DispatchRays opaque trivial hit (reference)", pipe, tOpaque, modes[1], 0);
    }

    // divergent hit shading: DispatchRays with and without NVAPI shader execution reordering
    {
        Gpu::RtPipeline pipeHeavy = g.rtPso(dxc.compile(lib, nullptr, L"lib_6_6", { L"HEAVY_HIT=1" }).Get(), false, false);
        runDr("DispatchRays divergent hit shading (16 paths, 256 MB textures), no SER", pipeHeavy, tOpaque, modes[0], 0);
        runDr("DispatchRays divergent hit shading (16 paths, 256 MB textures), no SER", pipeHeavy, tOpaque, modes[2], 0);
        runDr("DispatchRays divergent hit shading (16 paths, 256 MB textures), no SER", pipeHeavy, tOpaque, modes[1], 0);
        if (g_nvSer)
        {
            try
            {
                Gpu::RtPipeline pipeSer = g.rtPso(dxc.compile(lib, nullptr, L"lib_6_6", { L"HEAVY_HIT=1", L"NV_SER=1" }).Get(), false, true);
                runDr("DispatchRays divergent hit shading, NVAPI SER (reorder by material)", pipeSer, tOpaque, modes[0], 0);
                runDr("DispatchRays divergent hit shading, NVAPI SER (reorder by material)", pipeSer, tOpaque, modes[2], 0);
                runDr("DispatchRays divergent hit shading, NVAPI SER (reorder by material)", pipeSer, tOpaque, modes[1], 0);
                Gpu::RtPipeline pipeSerLight = g.rtPso(dxc.compile(lib, nullptr, L"lib_6_6", { L"NV_SER=1" }).Get(), false, true);
                runDr("DispatchRays trivial hit, NVAPI SER (reorder overhead)", pipeSerLight, tOpaque, modes[0], 0);
            }
            catch (Failure& f) { logf("  SER unavailable: %s\n", f.what.c_str()); record("rays", "NVAPI SER measurement", 0, "n/a", f.what.substr(0, 160)); }
        }
        else record("rays", "NVAPI SER measurement", 0, "n/a", "thread reordering cap not reported");
    }
    (void)trySer;
}

// ------------------------------------------------------------------------------------------ PSO compile time vs DXIL size
static std::string generateKernel(int stages, uint32_t seed)
{
    std::mt19937 rng(seed); std::uniform_real_distribution<float> U(0.5f, 1.5f);
    std::ostringstream s;
    s << g_common;
    s << "[numthreads(64,1,1)] void CS(uint tid : SV_DispatchThreadID) {\n";
    s << " RWStructuredBuffer<float4> Out = ResourceDescriptorHeap[P[0].y]; StructuredBuffer<float4> In = ResourceDescriptorHeap[P[0].x];\n";
    s << " float4 r0 = float4(tid,1,2,3)*1e-3, r1 = r0+1, r2 = r0+2, r3 = r0+3, r4 = r0+4, r5 = r0+5, r6 = r0+6, r7 = r0+7;\n";
    for (int k = 0; k < stages; ++k)
    {
        int a = k % 8, b = (k + 3) % 8, c = (k + 5) % 8;
        s << " r" << a << " = mad(r" << a << ", r" << b << ", float4(" << U(rng) << "," << U(rng) << "," << U(rng) << "," << U(rng) << "));\n";
        s << " r" << b << " = min(r" << b << " * " << U(rng) << ", r" << c << ") + sqrt(abs(r" << a << ".wzyx) + " << U(rng) << ");\n";
        s << " r" << c << " = (r" << c << ".x > " << U(rng) << ") ? r" << c << ".yzwx * r" << a << " : r" << c << " - " << U(rng) << ";\n";
        if (k % 8 == 7) s << " r" << a << " += In[(tid * " << (k + 1) << "u + " << (k * 13) << "u) & 4095u];\n";
        if (k % 32 == 31) s << " if (r" << b << ".x == " << U(rng) << ") Out[tid & 1023u] = r" << b << ";\n";
    }
    s << " Out[tid & 1023u] = r0 + r1 + r2 + r3 + r4 + r5 + r6 + r7;\n}\n";
    return s.str();
}
static void testPsoCompile()
{
    // The constants must differ between runs as well as between sizes: the driver keeps a disk cache keyed by
    // the DXIL, so a fixed seed makes every run after the first a disk-cache hit (measured 20-75x faster).
    const uint32_t nonce = std::random_device{}() ^ (uint32_t)GetTickCount64();
    logf("\n== PSO compile time vs DXIL size (per-run nonce %08x in the constants defeats the driver's memory and disk caches; 'warm' recreates the same blob)\n", nonce);
    for (int stages : { 32, 128, 512, 2048, 8192, 32768 })
    {
        double dxcMs = 0, cold = 0, warm = 0; size_t bytes = 0;
        try
        {
            auto blob = dxc.compile(generateKernel(stages, nonce * 2654435761u + (uint32_t)stages), L"CS", L"cs_6_6", {}, &dxcMs, true);
            bytes = blob->GetBufferSize();
            g.computePso(blob.Get(), &cold);
            g.computePso(blob.Get(), &warm);
        }
        catch (Failure& f) { logf("  stages %d: %s\n", stages, f.what.c_str()); continue; }
        char name[128];
        snprintf(name, sizeof name, "compute PSO, %d stages, DXIL %.0f KB: cold create", stages, bytes / 1024.0);
        record("pso", name, cold, "ms", "dxc " + std::to_string(dxcMs) + " ms, warm recreate " + std::to_string(warm) + " ms, " + std::to_string(cold / (bytes / 1024.0)) + " ms/KB");
        if (dxcMs + cold > 15000) { logf("  stopping: dxc + cold create exceeded 15 s (the next size would take minutes)\n"); break; }
    }
}

// ------------------------------------------------------------------------------------------ async compute overlap
static void testAsync()
{
    warmUp("Async compute overlap (wall clock; raster on direct queue, compute on compute queue)");
    setupRaster();
    std::string src = g_common + readFile(g_shaderDir + "/compute.hlsl");
    auto psoFma = g.computePso(dxc.compile(src, L"FmaCS", L"cs_6_6", {}).Get());
    auto psoRead = g.computePso(dxc.compile(src, L"ReadCS", L"cs_6_6", {}).Get());
    const UINT64 bytes = 1ull << 30; auto buf = g.buffer(bytes); UINT srv = g.srvStructured(buf.Get(), (UINT)(bytes / 16), 16);
    const UINT gx = 1024, gy = 170;
    Consts cr; cr(0, 0) = 0; cr(0, 1) = asu(2.f); cr(0, 2) = asu(3840.f); cr(0, 3) = asu(2160.f); cr(1, 2) = 11; cr(1, 3) = gx;
    Consts cf; cf(0, 0) = 1024; cf(0, 1) = g_outUav; cf(0, 2) = SENTINEL;
    Consts cm; cm(0, 0) = srv; cm(0, 1) = g_outUav; cm(0, 2) = 64; cm(0, 3) = (uint32_t)(bytes / 16 - 1); cm(1, 0) = SENTINEL; cm(1, 1) = 4096;
    auto recordCompute = [&](ID3D12GraphicsCommandList6* l, ID3D12PipelineState* pso, const Consts& c, UINT groups) { ID3D12DescriptorHeap* heaps[] = { g.heapCbv.Get(), g.heapSampler.Get() }; l->SetDescriptorHeaps(2, heaps); g.setConstants(l, c.v); l->SetPipelineState(pso); l->Dispatch(groups, 1, 1); };
    auto wall = [&](bool doRaster, bool doCompute, ID3D12PipelineState* cpso, const Consts& cc, UINT groups) {
        std::vector<double> v;
        for (int i = 0; i < 2 + 5; ++i)
        {
            if (doRaster) { auto l = g.begin(); recordRaster(l, g_raster.full[0].Get(), true, cr, gx, gy); check(l->Close(), "close"); }
            if (doCompute) { check(g.allocCompute->Reset(), "reset"); check(g.listCompute->Reset(g.allocCompute.Get(), nullptr), "reset"); recordCompute(g.listCompute.Get(), cpso, cc, groups); check(g.listCompute->Close(), "close"); }
            double t0 = now();
            if (doRaster) { ID3D12CommandList* ls[] = { g.listDirect.Get() }; g.direct->ExecuteCommandLists(1, ls); g.direct->Signal(g.fenceDirect.Get(), ++g.fenceValueDirect); }
            if (doCompute) { ID3D12CommandList* ls[] = { g.listCompute.Get() }; g.compute->ExecuteCommandLists(1, ls); g.compute->Signal(g.fenceCompute.Get(), ++g.fenceValueCompute); }
            if (doRaster) g.wait(g.fenceDirect.Get(), g.fenceValueDirect);
            if (doCompute) g.wait(g.fenceCompute.Get(), g.fenceValueCompute);
            if (i >= 2) v.push_back(now() - t0);
        }
        std::sort(v.begin(), v.end()); return v[v.size() / 2]; };
    double r = wall(true, false, nullptr, cf, 0);
    double f = wall(false, true, psoFma.Get(), cf, 16384);
    double rf = wall(true, true, psoFma.Get(), cf, 16384);
    record("async", "raster 16.7M tris alone (wall)", r, "ms");
    record("async", "fma compute alone (wall)", f, "ms");
    record("async", "raster + fma concurrent (wall)", rf, "ms", "serial sum " + std::to_string(r + f) + " ms, overlap gain " + std::to_string(100 * (1 - rf / (r + f))) + " %");
    double m = wall(false, true, psoRead.Get(), cm, 4096);
    double rm = wall(true, true, psoRead.Get(), cm, 4096);
    record("async", "bandwidth read alone (wall)", m, "ms");
    record("async", "raster + bandwidth read concurrent (wall)", rm, "ms", "serial sum " + std::to_string(r + m) + " ms, overlap gain " + std::to_string(100 * (1 - rm / (r + m))) + " %");
}


// ------------------------------------------------------------------------------------------ kernel-variant experiments
static void testExperiments()
{
    warmUp("Experiments (kernel formulation check)");
    std::string src = g_common + readFile(g_shaderDir + "/experiments.hlsl");
    const uint32_t groups = 16384, threads = groups * 256, iters = 1024;
    for (int v = 0; v < 6; ++v)
    {
        std::vector<std::wstring> defs{ L"FMA_VARIANT=" + std::to_wstring(v) };
        ComPtr<IDxcBlob> blob;
        try { if (v == 4) { std::string s16 = src; blob = dxc.compile(s16, L"FmaX", L"cs_6_6", { defs[0], L"__ENABLE16=1" }); } else blob = dxc.compile(src, L"FmaX", L"cs_6_6", defs); }
        catch (Failure& f) { logf("  fma variant %d: %s\n", v, f.what.c_str()); continue; }
        auto pso = g.computePso(blob.Get());
        Consts c; c(0, 0) = iters; c(0, 1) = g_outUav; c(0, 2) = SENTINEL;
        Stat s = g.time([&](ID3D12GraphicsCommandList6* l) { g.setConstants(l, c.v); l->SetPipelineState(pso.Get()); l->Dispatch(groups, 1, 1); });
        double flops = (double)threads * iters * 64.0;
        record("experiment", "fma variant " + std::to_string(v), flops / (s.median * 1e-3) / 1e12, "TFLOPS", std::to_string(s.median) + " ms, DXIL " + std::to_string(blob->GetBufferSize()) + " B");
    }
    const UINT64 bytes = 1ull << 30; auto buf = g.buffer(bytes);
    UINT srv = g.srvStructured(buf.Get(), (UINT)(bytes / 16), 16), raw = g.srvRaw(buf.Get(), bytes);
    {
        auto psoWrite = g.computePso(dxc.compile(g_common + readFile(g_shaderDir + "/compute.hlsl"), L"WriteCS", L"cs_6_6", {}).Get());
        UINT uav = g.uavStructured(buf.Get(), (UINT)(bytes / 16), 16);
        Consts c; c(0, 0) = uav; c(0, 2) = 64; c(0, 3) = (uint32_t)(bytes / 16 - 1); c(1, 0) = 11; c(1, 1) = 1; c(1, 2) = (uint32_t)(bytes / 16 / 256 / 64);
        auto l = g.begin(); g.setConstants(l, c.v); l->SetPipelineState(psoWrite.Get()); l->Dispatch((UINT)(bytes / 16 / 256 / 64), 1, 1); g.submitAndWait();
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC td{}; td.ViewDimension = D3D12_SRV_DIMENSION_BUFFER; td.Format = DXGI_FORMAT_R32G32B32A32_UINT; td.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; td.Buffer.NumElements = (UINT)(bytes / 16);
    UINT typed = g.nextCbv++; g.dev->CreateShaderResourceView(buf.Get(), &td, g.cbvHandle(typed));
    for (int v = 0; v < 6; ++v)
    {
        auto pso = g.computePso(dxc.compile(src, L"ReadX", L"cs_6_6", { L"READ_VARIANT=" + std::to_wstring(v) }).Get());
        for (uint32_t per : { 16u, 64u })
        {
            uint32_t grp = (uint32_t)(bytes / 16 / 256 / per);
            Consts c; c(0, 0) = per; c(0, 1) = g_outUav; c(0, 2) = SENTINEL; c(0, 3) = (uint32_t)(bytes / 16 - 1); c(1, 0) = (v == 1 || v == 4) ? raw : srv; c(1, 1) = grp; c(1, 2) = typed;
            Stat s = g.time([&](ID3D12GraphicsCommandList6* l) { g.setConstants(l, c.v); l->SetPipelineState(pso.Get()); l->Dispatch(grp, 1, 1); });
            record("experiment", "read variant " + std::to_string(v) + ", per-thread " + std::to_string(per) + " x 16 B, 1 GiB", bytes / (s.median * 1e-3) / 1e9, "GB/s", std::to_string(s.median) + " ms");
        }
    }
}


// ------------------------------------------------------------------------------------------ VSM lookup + fused shading (4K)
static uint32_t pcgHash(uint32_t v) { uint32_t s = v * 747796405u + 2891336453u; uint32_t w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u; return (w >> 22u) ^ w; }
static void testVsmShade()
{
    warmUp("Virtual shadow map lookup and fused shading floors (3840x2160)");
    std::string src = g_common + readFile(g_shaderDir + "/extra.hlsl");
    auto psoVsm = g.computePso(dxc.compile(src, L"VsmLookupCS", L"cs_6_6", {}).Get());
    auto psoShade = g.computePso(dxc.compile(src, L"ShadeCS", L"cs_6_6", {}).Get());
    const UINT W = 3840, H = 2160; const size_t P = (size_t)W * H;
    std::vector<uint32_t> table(12 * 16384);
    for (size_t i = 0; i < table.size(); ++i) table[i] = pcgHash((uint32_t)i * 2654435761u) & 1023;
    auto sunTable = g.buffer(table.size() * 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_NONE); g.upload(sunTable.Get(), table.data(), table.size() * 4);
    for (size_t i = 0; i < table.size(); ++i) table[i] = pcgHash((uint32_t)i * 40503u + 17u) & 1023;
    auto locTable = g.buffer(table.size() * 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_NONE); g.upload(locTable.Get(), table.data(), table.size() * 4);
    UINT sunTableSrv = g.srvStructured(sunTable.Get(), (UINT)table.size(), 4), locTableSrv = g.srvStructured(locTable.Get(), (UINT)table.size(), 4);
    std::vector<float> atlas((size_t)1024 * 16384);
    for (size_t i = 0; i < atlas.size(); ++i) atlas[i] = (pcgHash((uint32_t)i) & 0xffff) / 65535.0f * 300.0f;
    auto sunAtlas = g.buffer(atlas.size() * 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_NONE); g.upload(sunAtlas.Get(), atlas.data(), atlas.size() * 4);
    auto locAtlas = g.buffer(atlas.size() * 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_NONE); g.upload(locAtlas.Get(), atlas.data(), atlas.size() * 4);
    UINT sunAtlasSrv = g.srvStructured(sunAtlas.Get(), (UINT)atlas.size(), 4), locAtlasSrv = g.srvStructured(locAtlas.Get(), (UINT)atlas.size(), 4);
    auto outBuf = g.buffer(P * 8); UINT outUav = g.uavStructured(outBuf.Get(), (UINT)P, 4), outUav2 = g.uavStructured(outBuf.Get(), (UINT)P, 8);
    for (uint32_t scattered = 0; scattered < 2; ++scattered)
        for (uint32_t taps : { 1u, 5u, 9u })
            for (uint32_t locals = 0; locals < 2; ++locals)
            {
                if (scattered && taps != 5) continue;
                Consts c; c(0, 0) = sunTableSrv; c(0, 1) = sunAtlasSrv; c(0, 2) = outUav; c(0, 3) = scattered; c(1, 0) = W; c(1, 1) = H; c(1, 2) = taps; c(1, 3) = 5; c(2, 0) = locTableSrv; c(2, 1) = locAtlasSrv; c(2, 2) = locals ? 3 : 0;
                Stat s = g.time([&](ID3D12GraphicsCommandList6* l) { g.setConstants(l, c.v); l->SetPipelineState(psoVsm.Get()); l->Dispatch(W / 8, H / 8, 1); });
                double lookups = (double)P * (1 + (locals ? 1.5 : 0));
                char name[160]; snprintf(name, sizeof name, "VSM lookup 4K, %s depth, sun%s, %u taps", scattered ? "scattered (worst)" : "coherent scene", locals ? " + 1.5 local lights" : "", taps);
                record("vsm", name, s.median, "ms", std::to_string(s.median * 1e6 / lookups) + " ns per light lookup, " + std::to_string(s.median * 1e6 / (lookups * taps)) + " ns per tap");
            }
    // synthetic G-buffer + probes + froxels
    std::vector<uint32_t> gb(P * 4);
    for (size_t i = 0; i < P; ++i)
    {
        uint32_t h = pcgHash((uint32_t)i * 3u + 1u);
        float nx = ((h & 0xffff) / 65535.0f) * 1.2f - 0.6f, ny = (((h >> 16) & 0xffff) / 65535.0f) * 1.2f - 0.6f; // oct coords, mostly up-facing
        gb[i * 4 + 0] = (uint32_t)((nx + 1) * 32767.5f) | ((uint32_t)((ny + 1) * 32767.5f) << 16);
        gb[i * 4 + 1] = pcgHash(h) | 0x40000000u; // albedo + roughness (roughness byte >= 0x40)
        gb[i * 4 + 2] = pcgHash(h + 7) & 0xffff;
        float dist = 1.0f + (pcgHash(h + 9) & 0xffff) / 65535.0f * 60.0f; uint32_t du; memcpy(&du, &dist, 4); gb[i * 4 + 3] = du;
    }
    auto gbuf = g.buffer(gb.size() * 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_NONE); g.upload(gbuf.Get(), gb.data(), gb.size() * 4);
    UINT gbSrv = g.srvStructured(gbuf.Get(), (UINT)P, 16);
    size_t probeCount = (size_t)(W / 8 + 1) * (H / 8 + 1) * 2; std::vector<float> pr(probeCount * 4); for (auto& f : pr) f = 0.3f;
    auto probes = g.buffer(pr.size() * 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_NONE); g.upload(probes.Get(), pr.data(), pr.size() * 4);
    UINT probeSrv = g.srvStructured(probes.Get(), (UINT)probeCount, 16);
    std::vector<float> fr((size_t)160 * 90 * 64 * 4); for (size_t i = 0; i < fr.size(); ++i) fr[i] = (i % 4 == 3) ? 0.9f : 0.02f;
    auto frox = g.buffer(fr.size() * 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_NONE); g.upload(frox.Get(), fr.data(), fr.size() * 4);
    UINT froxSrv = g.srvStructured(frox.Get(), (UINT)(fr.size() / 4), 16);
    auto visBuf = g.buffer(P * 4); UINT visSrv = g.srvStructured(visBuf.Get(), (UINT)P, 4);
    struct Variant { uint32_t mask; uint32_t lights; const char* name; };
    Variant variants[] = {
        { 15, 4, "fused: sun + 4 locals GGX, 3 shadowed x 5 VSM taps (in-kernel), 4 probes, froxel" },
        { 31, 4, "fused: same, page-table entries prefetched before atlas taps" },
        { 14, 4, "split: visibility from a 4 B/pixel buffer (separate VSM pass), sun + 4 locals, probes, froxel" },
        { 12, 4, "split, no light loop (sun only) + probes + froxel" },
        { 8, 4, "split, sun only + froxel (no probes)" },
        { 0, 4, "G-buffer in / out + sun only (memory floor of the kernel)" },
        { 14, 8, "split: sun + 8 locals" },
        { 14, 16, "split: sun + 16 locals" },
        { 15, 8, "fused: sun + 8 locals (3 shadowed in-kernel)" },
    };
    for (const Variant& v : variants)
    {
        auto pso = g.computePso(dxc.compile(src, L"ShadeCS", L"cs_6_6", { L"SHADE_MASK=" + std::to_wstring(v.mask) }).Get());
        Consts c; c(0, 0) = gbSrv; c(0, 1) = outUav2; c(0, 2) = probeSrv; c(0, 3) = froxSrv; c(1, 0) = W; c(1, 1) = H; c(1, 2) = 5; c(1, 3) = 9;
        c(2, 0) = sunTableSrv; c(2, 1) = sunAtlasSrv; c(2, 2) = locTableSrv; c(2, 3) = locAtlasSrv; c(3, 0) = v.lights; c(3, 1) = visSrv;
        Stat s = g.time([&](ID3D12GraphicsCommandList6* l) { g.setConstants(l, c.v); l->SetPipelineState(pso.Get()); l->Dispatch(W / 8, H / 8, 1); });
        record("shade", std::string("shading 4K, ") + v.name, s.median, "ms", std::to_string(s.median * 1e6 / P) + " ns per pixel");
    }
}

// ------------------------------------------------------------------------------------------ MPM step floors
static void testMpm()
{
    warmUp("MPM step floors (128^3 grid, quadratic B-spline, APIC, 8 particles per cell)");
    std::string src = g_common + readFile(g_shaderDir + "/extra.hlsl");
    auto psoHist = g.computePso(dxc.compile(src, L"MpmHistogramCS", L"cs_6_6", {}).Get());
    auto psoPrefix = g.computePso(dxc.compile(src, L"MpmPrefixCS", L"cs_6_6", {}).Get());
    auto psoScatter = g.computePso(dxc.compile(src, L"MpmScatterCS", L"cs_6_6", {}).Get());
    auto psoClear = g.computePso(dxc.compile(src, L"MpmClearCS", L"cs_6_6", {}).Get());
    auto psoP2GA = g.computePso(dxc.compile(src, L"MpmP2GAtomicCS", L"cs_6_6", {}).Get());
    auto psoP2GB = g.computePso(dxc.compile(src, L"MpmP2GBlockCS", L"cs_6_6", {}).Get());
    auto psoGrid = g.computePso(dxc.compile(src, L"MpmGridCS", L"cs_6_6", {}).Get());
    auto psoG2P = g.computePso(dxc.compile(src, L"MpmG2PCS", L"cs_6_6", {}).Get());
    const UINT G = 128, nodes = G * G * G;
    auto gridInt = g.buffer((UINT64)nodes * 16), gridVel = g.buffer((UINT64)nodes * 16);
    UINT gridInt4Uav = g.uavStructured(gridInt.Get(), nodes * 4, 4), gridInt16Uav = g.uavStructured(gridInt.Get(), nodes, 16), gridInt16Srv = g.srvStructured(gridInt.Get(), nodes, 16);
    UINT gridVelUav = g.uavStructured(gridVel.Get(), nodes, 16), gridVelSrv = g.srvStructured(gridVel.Get(), nodes, 16);
    auto count = g.buffer(4096 * 4), offsets = g.buffer(8192 * 4);
    UINT countUav = g.uavStructured(count.Get(), 4096, 4), offsetsUav = g.uavStructured(offsets.Get(), 8192, 4), offsetsSrv = g.srvStructured(offsets.Get(), 8192, 4);
    { std::vector<uint32_t> z(4096, 0); g.upload(count.Get(), z.data(), z.size() * 4); }
    for (uint32_t N : { 1u << 20, 2u << 20 })
    {
        float side = cbrtf(N / 8.0f);
        std::vector<float> pd((size_t)N * 16);
        std::mt19937 rng(3); std::uniform_real_distribution<float> U(0.f, 1.f);
        for (uint32_t i = 0; i < N; ++i)
        {
            float* q = &pd[(size_t)i * 16];
            q[0] = 4 + U(rng) * side; q[1] = 4 + U(rng) * side; q[2] = 4 + U(rng) * side; q[3] = 1.0f;
            q[4] = U(rng) - 0.5f; q[5] = U(rng) - 0.5f; q[6] = U(rng) - 0.5f; q[7] = 1.0f;
            for (int k = 8; k < 16; ++k) q[k] = 0;
        }
        auto parts = g.buffer((UINT64)N * 64), sorted = g.buffer((UINT64)N * 64), bid = g.buffer((UINT64)N * 4);
        g.upload(parts.Get(), pd.data(), pd.size() * 4);
        UINT partsSrv = g.srvStructured(parts.Get(), N, 64), partsUav = g.uavStructured(parts.Get(), N, 64);
        UINT sortedSrv = g.srvStructured(sorted.Get(), N, 64), sortedUav = g.uavStructured(sorted.Get(), N, 64);
        UINT bidUav = g.uavStructured(bid.Get(), N, 4), bidSrv = g.srvStructured(bid.Get(), N, 4);
        float dt = 1.0f / 240.0f;
        auto C = [&](uint32_t p0x, uint32_t p0y, uint32_t p0z, uint32_t p0w, uint32_t p1x, uint32_t p1y, uint32_t p2x) {
            Consts c; c(0, 0) = p0x; c(0, 1) = p0y; c(0, 2) = p0z; c(0, 3) = p0w; c(1, 0) = p1x; c(1, 1) = p1y; c(1, 2) = N; c(1, 3) = G; c(2, 0) = p2x; c(2, 2) = asu(dt); return c; };
        Consts cHist = C(partsSrv, 0, countUav, 0, 0, 0, bidUav);
        Consts cPrefix = C(0, 0, countUav, offsetsUav, 0, 0, 0);
        Consts cScatter = C(partsSrv, sortedUav, 0, offsetsUav, 0, 0, bidSrv);
        Consts cClear = C(0, 0, 0, 0, gridInt16Uav, 0, 0);
        Consts cP2G = C(0, sortedSrv, 0, offsetsSrv, gridInt4Uav, 0, 0);
        Consts cGrid = C(0, 0, 0, 0, gridInt16Srv, gridVelUav, 0);
        Consts cG2P = C(partsUav, sortedSrv, 0, 0, 0, gridVelSrv, 0);
        auto dispatch = [&](ID3D12GraphicsCommandList6* l, ID3D12PipelineState* pso, const Consts& c, UINT groups) { g.setConstants(l, c.v); l->SetPipelineState(pso); l->Dispatch(groups, 1, 1); Gpu::uavBarrier(l); };
        auto sortSeq = [&](ID3D12GraphicsCommandList6* l) { dispatch(l, psoHist.Get(), cHist, N / 256); dispatch(l, psoPrefix.Get(), cPrefix, 1); dispatch(l, psoScatter.Get(), cScatter, N / 256); };
        // prime: one full step so that the sorted buffer and grid hold a consistent state
        { auto l = g.begin(); dispatch(l, psoClear.Get(), cClear, nodes / 256); sortSeq(l); dispatch(l, psoP2GB.Get(), cP2G, 4096); dispatch(l, psoGrid.Get(), cGrid, nodes / 256); dispatch(l, psoG2P.Get(), cG2P, N / 256); g.submitAndWait(); }
        std::string tag = std::to_string(N >> 20) + "M particles";
        Stat sClear = g.time([&](ID3D12GraphicsCommandList6* l) { dispatch(l, psoClear.Get(), cClear, nodes / 256); });
        record("mpm", tag + ": grid clear (128^3 x 16 B dense)", sClear.median, "ms");
        Stat sSort = g.time([&](ID3D12GraphicsCommandList6* l) { sortSeq(l); });
        record("mpm", tag + ": block sort (histogram + prefix + scatter, 64 B particles)", sSort.median, "ms");
        Stat sA = g.time([&](ID3D12GraphicsCommandList6* l) { dispatch(l, psoClear.Get(), cClear, nodes / 256); dispatch(l, psoP2GA.Get(), cP2G, N / 256); });
        record("mpm", tag + ": P2G via global atomics (27 nodes x 4 channels), incl. clear", sA.median, "ms", std::to_string((sA.median - sClear.median) * 1e6 / N) + " ns per particle");
        Stat sB = g.time([&](ID3D12GraphicsCommandList6* l) { dispatch(l, psoClear.Get(), cClear, nodes / 256); dispatch(l, psoP2GB.Get(), cP2G, 4096); });
        record("mpm", tag + ": P2G via groupshared block accumulation + halo atomics, incl. clear", sB.median, "ms", std::to_string((sB.median - sClear.median) * 1e6 / N) + " ns per particle");
        Stat sG = g.time([&](ID3D12GraphicsCommandList6* l) { dispatch(l, psoGrid.Get(), cGrid, nodes / 256); });
        record("mpm", tag + ": grid update (dense 128^3)", sG.median, "ms", "sparse grids scale with active nodes (" + std::to_string(N / 8) + " here)");
        Stat sP = g.time([&](ID3D12GraphicsCommandList6* l) { dispatch(l, psoG2P.Get(), cG2P, N / 256); });
        record("mpm", tag + ": G2P (27-node gather, APIC, 64 B write)", sP.median, "ms", std::to_string(sP.median * 1e6 / N) + " ns per particle");
        Stat sAll = g.time([&](ID3D12GraphicsCommandList6* l) { dispatch(l, psoClear.Get(), cClear, nodes / 256); sortSeq(l); dispatch(l, psoP2GB.Get(), cP2G, 4096); dispatch(l, psoGrid.Get(), cGrid, nodes / 256); dispatch(l, psoG2P.Get(), cG2P, N / 256); });
        record("mpm", tag + ": full step (clear + sort + block P2G + grid + G2P)", sAll.median, "ms", std::to_string(sAll.median * 1e6 / N) + " ns per particle-step");
    }
}

// ------------------------------------------------------------------------------------------ sub-pixel coverage layer floor
static void testCoverage()
{
    warmUp("Sub-pixel coverage layer (conservative raster + exact pixel area + fragment append, 4K)");
    std::string src = g_common + readFile(g_shaderDir + "/coverage.hlsl");
    auto ms = dxc.compile(src, L"CovMS", L"ms_6_6", {});
    auto ps = dxc.compile(src, L"CovPS", L"ps_6_6", {});
    auto psCount = dxc.compile(src, L"CovCountPS", L"ps_6_6", {});
    auto psoCov = g.meshPso(ms.Get(), ps.Get(), false, false, nullptr, true, false);
    auto psoCovStd = g.meshPso(ms.Get(), ps.Get(), false, false, nullptr, false, false);
    auto psoCountCons = g.meshPso(ms.Get(), psCount.Get(), false, false, nullptr, true, false);
    auto psoCountStd = g.meshPso(ms.Get(), psCount.Get(), false, false, nullptr, false, false);
    const UINT cap = 1u << 24;
    auto frags = g.buffer((UINT64)cap * 16); UINT fragUav = g.uavStructured(frags.Get(), cap, 16);
    auto counter = g.buffer(256); UINT counterUav = g.uavStructured(counter.Get(), 64, 4);
    auto recordCov = [&](ID3D12GraphicsCommandList6* l, ID3D12PipelineState* pso, const Consts& c, UINT gx, UINT gy) {
        D3D12_VIEWPORT vp{ 0, 0, 3840, 2160, 0, 1 }; D3D12_RECT sc{ 0, 0, 3840, 2160 };
        l->RSSetViewports(1, &vp); l->RSSetScissorRects(1, &sc);
        l->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
        g.setConstants(l, c.v, true); l->SetPipelineState(pso); l->DispatchMesh(gx, gy, 1); Gpu::uavBarrier(l); };
    double areaSum = 0;
    auto countOnce = [&](ID3D12PipelineState* pso, const Consts& c0, UINT gx, UINT gy, int slot) {
        std::vector<uint32_t> z(64, 0); g.upload(counter.Get(), z.data(), 256);
        Consts c = c0; c(2, 1) = 1;
        auto l = g.begin(); recordCov(l, pso, c, gx, gy); g.submitAndWait();
        auto d = g.readback(counter.Get(), 256); areaSum = ((const uint32_t*)d.data())[2] / 16.0; return (double)((const uint32_t*)d.data())[slot]; };
    for (float width : { 0.25f, 0.5f, 1.0f })
        for (UINT gy : { 8u, 32u })
        {
            const UINT gx = 1024; double tris = (double)gx * gy * 64;
            Consts c; c(0, 0) = asu(width); c(0, 1) = asu(20.f); c(0, 2) = asu(3840.f); c(0, 3) = asu(2160.f); c(1, 0) = fragUav; c(1, 1) = counterUav; c(1, 2) = 5; c(1, 3) = gx; c(2, 0) = cap - 1;
            double fragsCons = countOnce(psoCov.Get(), c, gx, gy, 0);
            double areaExpected = (tris / 2) * width * 20.0, areaMeasured = areaSum;
            double pixStd = countOnce(psoCountStd.Get(), c, gx, gy, 1);
            Stat s = g.time([&](ID3D12GraphicsCommandList6* l) { recordCov(l, psoCov.Get(), c, gx, gy); });
            Stat sc = g.time([&](ID3D12GraphicsCommandList6* l) { recordCov(l, psoCountCons.Get(), c, gx, gy); });
            Stat ss = g.time([&](ID3D12GraphicsCommandList6* l) { recordCov(l, psoCovStd.Get(), c, gx, gy); });
            char name[200];
            snprintf(name, sizeof name, "%.0fk slivers %.2f px x 20 px (%.1fM tris): conservative raster + exact area + append", tris / 2000, width, tris / 1e6);
            record("coverage", name, s.median, "ms", std::to_string(fragsCons / 1e6) + " M fragments (" + std::to_string(fragsCons / tris) + " per tri), " + std::to_string(fragsCons / (s.median * 1e-3) / 1e9) + " G frags/s, area check " + std::to_string(areaMeasured / areaExpected));
            snprintf(name, sizeof name, "%.1fM tris %.2f px: conservative raster, trivial PS", tris / 1e6, width);
            record("coverage", name, sc.median, "ms");
            snprintf(name, sizeof name, "%.1fM tris %.2f px: standard raster + area + append (centre-sample pixels only)", tris / 1e6, width);
            record("coverage", name, ss.median, "ms", std::to_string(pixStd / 1e6) + " M pixels touched by centre sampling vs " + std::to_string(fragsCons / 1e6) + " M true fragments");
        }
}

// ------------------------------------------------------------------------------------------ main
static std::string jsonEscape(const std::string& s) { std::string o; for (char c : s) { if (c == '"' || c == '\\') o += '\\'; if (c == '\n') { o += "\\n"; continue; } o += c; } return o; }
int main(int argc, char** argv)
{
    bool doCompute = true, doMemory = true, doTexture = true, doAtomics = true, doRaster = true, doRays = true, doPso = true, doAsync = true, trySer = false, doExperiments = false, doVsm = true, doMpm = true, doCoverage = true;
    std::string dxcDir = MB_DXC_DIR, outDir = MB_RESULT_DIR; bool experimental = false;
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "--skip-compute") doCompute = false; else if (a == "--skip-memory") doMemory = false; else if (a == "--skip-texture") doTexture = false;
        else if (a == "--skip-atomics") doAtomics = false; else if (a == "--skip-raster") doRaster = false; else if (a == "--skip-rays") doRays = false;
        else if (a == "--skip-pso") doPso = false; else if (a == "--skip-async") doAsync = false; else if (a == "--ser") trySer = true; else if (a == "--experimental") experimental = true; else if (a == "--thin-foliage") g_thinFoliage = true;
        else if (a == "--only-rays") { doCompute = doMemory = doTexture = doAtomics = doRaster = doPso = doAsync = doVsm = doMpm = doCoverage = false; }
        else if (a == "--only-ser") { doCompute = doMemory = doTexture = doAtomics = doRaster = doPso = doAsync = doVsm = doMpm = doCoverage = false; g_onlySer = true; }
        else if (a == "--only-edges") { doCompute = doMemory = doTexture = doAtomics = doRaster = doPso = doAsync = doVsm = doMpm = doCoverage = false; g_onlySer = true; g_onlyEdges = true; }
        else if (a == "--only-vsm") { doCompute = doMemory = doTexture = doAtomics = doRaster = doPso = doAsync = doRays = doMpm = doCoverage = false; }
        else if (a == "--only-mpm") { doCompute = doMemory = doTexture = doAtomics = doRaster = doPso = doAsync = doRays = doVsm = doCoverage = false; }
        else if (a == "--only-coverage") { doCompute = doMemory = doTexture = doAtomics = doRaster = doPso = doAsync = doRays = doVsm = doMpm = false; }
        else if (a == "--skip-vsm") doVsm = false; else if (a == "--skip-mpm") doMpm = false; else if (a == "--skip-coverage") doCoverage = false;
        else if (a == "--only-raster") { doCompute = doMemory = doTexture = doAtomics = doRays = doPso = doAsync = doVsm = doMpm = doCoverage = false; }
        else if (a == "--only-pso") { doCompute = doMemory = doTexture = doAtomics = doRays = doRaster = doAsync = doVsm = doMpm = doCoverage = false; }
        else if (a == "--experiments") { doCompute = doMemory = doTexture = doAtomics = doRays = doRaster = doAsync = doPso = doVsm = doMpm = doCoverage = false; doExperiments = true; }
        else if (a == "--dxc" && i + 1 < argc) dxcDir = argv[++i];
        else if (a == "--out" && i + 1 < argc) outDir = argv[++i];
        else if (a == "--reps" && i + 1 < argc) g.reps = atoi(argv[++i]);
    }
    try
    {
        if (experimental) { UUID f = D3D12ExperimentalShaderModels; HRESULT hr = D3D12EnableExperimentalFeatures(1, &f, nullptr, nullptr); logf("D3D12EnableExperimentalFeatures(ShaderModels): 0x%08X\n", (unsigned)hr); }
        g.init();
        dxc.load(dxcDir);
        logf("DXC: %s\n", dxcDir.c_str());
        g_common = readFile(g_shaderDir + "/common.hlsli") + "\n";
        g_scratchOut = g.buffer(64 << 20); g_outUav = g.uavStructured(g_scratchOut.Get(), 16 << 20, 4);
        g_nvSlotUav = g.uavStructured(g_scratchOut.Get(), 1024, 4);
        { std::string nv = MB_NVAPI_DIR; dxc.includeDir.assign(nv.begin(), nv.end()); }
        record("env", "timestamp frequency", (double)g.timestampFrequency, "Hz");
        record("env", "raytracing tier", (double)g.rtTier, "tier");
        record("env", "mesh shader tier", (double)g.msTier, "tier");
        record("env", "highest shader model", (double)g.shaderModel, "hex");
        if (doExperiments) testExperiments();
        if (doCompute) testCompute();
        if (doMemory) testMemory();
        if (doTexture) testTexture();
        if (doAtomics) testAtomicsAndDispatch();
        if (doRaster) testRaster();
        if (doCoverage) testCoverage();
        if (doVsm) testVsmShade();
        if (doMpm) testMpm();
        if (doRays) testRays(trySer);
        if (doPso) testPsoCompile();
        if (doAsync) testAsync();
    }
    catch (Failure& f)
    {
        logf("\nFAILED: %s\n", f.what.c_str());
        HRESULT reason = g.dev ? g.dev->GetDeviceRemovedReason() : S_OK;
        if (FAILED(reason)) logf("device removed reason 0x%08X\n", (unsigned)reason);
    }
    // write results
    SYSTEMTIME st; GetLocalTime(&st);
    char stamp[64]; snprintf(stamp, sizeof stamp, "%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    CreateDirectoryA(outDir.c_str(), nullptr);
    std::ofstream js(outDir + "/microbench_" + stamp + ".json");
    js << "{\n \"adapter\": \"" << jsonEscape(g.adapterName) << "\", \"driver\": \"" << g.driverVersion << "\", \"agility_sdk\": " << D3D12SDKVersion << ", \"dxc\": \"" << jsonEscape(dxcDir) << "\", \"timestamp\": \"" << stamp << "\",\n \"results\": [\n";
    for (size_t i = 0; i < g_results.size(); ++i)
    {
        const Result& r = g_results[i];
        js << "  {\"section\": \"" << r.section << "\", \"name\": \"" << jsonEscape(r.name) << "\", \"value\": " << r.value << ", \"unit\": \"" << r.unit << "\", \"note\": \"" << jsonEscape(r.note) << "\"}" << (i + 1 < g_results.size() ? ",\n" : "\n");
    }
    js << " ]\n}\n";
    std::ofstream md(outDir + "/microbench_" + stamp + ".md");
    md << "# Microbench " << stamp << "\n\nAdapter: " << g.adapterName << ", driver " << g.driverVersion << ", Agility SDK " << D3D12SDKVersion << ", DXC " << dxcDir << "\n\n| section | measurement | value | unit | note |\n|---|---|---:|---|---|\n";
    for (const Result& r : g_results) md << "| " << r.section << " | " << r.name << " | " << r.value << " | " << r.unit << " | " << r.note << " |\n";
    std::ofstream lg(outDir + "/microbench_" + stamp + ".log"); lg << g_log;
    logf("\nResults written to %s/microbench_%s.{json,md,log}\n", outDir.c_str(), stamp);
    return 0;
}
