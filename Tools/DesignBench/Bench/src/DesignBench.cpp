// Design-revision microbenchmarks (COVERAGE_REDESIGN_KO.md 7.3; Requests/20260925_D_coverage_redesign.md 9).
// Standalone D3D12 tool in the style of Tools/Microbench: run-time DXC compilation, GPU timestamps around a recorded
// region, 3 warm + N timed repetitions, the median reported. Four sections:
//   --only-coverage   band B fragment pipeline: conservative raster + exact area + mask + 24 B append (global / per-tile
//                     segments with per-fragment or per-wave-tile atomics), tile composite (groupshared sort + shade)
//   --only-bricks     band C brick DDA: 16^3 bricks, 1 B or 8 B voxels, camera and sun (orthographic) marches, entry maps
//   --only-bands      banded pixel passes: resolve (8 B in, 12 B out) + shade (32/48 B in, 4 B out), 1/4/8/16 bands
//   --only-shade      shading kernel with tile-shared probe SH, in-kernel air fetches, K tap, 8 lights, register pressure
// Hardware runs need the GPU lock (UNX_GPU_LOCK, Tools/CI/GpuLock.ps1). --warp runs everything on the WARP adapter at a
// small resolution with 1 repetition as a termination / sanity check (no lock, no clock warm-up).
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3d12sdklayers.h>
#include <dxcapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
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
static double now() { using namespace std::chrono; return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count(); }
static uint32_t asu(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

struct Result { std::string section, name; double value; std::string unit; std::string note; };
static std::vector<Result> g_results;
static void record(const std::string& section, const std::string& name, double value, const std::string& unit, const std::string& note = "")
{
    g_results.push_back({ section, name, value, unit, note });
    logf("  [%s] %-70s %12.4f %-8s %s\n", section.c_str(), name.c_str(), value, unit.c_str(), note.c_str());
}

// ------------------------------------------------------------------------------------------ DXC
struct Dxc
{
    HMODULE lib = nullptr;
    ComPtr<IDxcUtils> utils;
    ComPtr<IDxcCompiler3> compiler;
    ComPtr<IDxcIncludeHandler> includeHandler;
    std::wstring includeDir;
    void load(const std::string& d)
    {
        SetDllDirectoryA(d.c_str());
        lib = LoadLibraryA((d + "\\dxcompiler.dll").c_str());
        if (!lib) throw Failure{ "cannot load dxcompiler.dll from " + d };
        auto create = (DxcCreateInstanceProc)GetProcAddress(lib, "DxcCreateInstance");
        check(create(CLSID_DxcUtils, IID_PPV_ARGS(&utils)), "DxcUtils");
        check(create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)), "DxcCompiler");
        check(utils->CreateDefaultIncludeHandler(&includeHandler), "include handler");
    }
    ComPtr<IDxcBlob> compile(const std::string& source, const wchar_t* entry, const wchar_t* target, const std::vector<std::wstring>& defines)
    {
        DxcBuffer src{ source.data(), source.size(), DXC_CP_UTF8 };
        std::vector<std::wstring> owned;
        std::vector<LPCWSTR> args = { L"-T", target, L"-E", entry, L"-O3", L"-HV", L"2021", L"-Qstrip_reflect", L"-Qstrip_debug", L"-I", includeDir.c_str() };
        for (auto& d : defines) owned.push_back(L"-D" + d);
        for (auto& d : owned) args.push_back(d.c_str());
        ComPtr<IDxcResult> result;
        check(compiler->Compile(&src, args.data(), (UINT)args.size(), includeHandler.Get(), IID_PPV_ARGS(&result)), "Compile");
        HRESULT status; result->GetStatus(&status);
        ComPtr<IDxcBlobUtf8> errors; result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
        if (FAILED(status)) throw Failure{ std::string("shader compile failed: ") + (errors && errors->GetStringLength() ? errors->GetStringPointer() : "no diagnostics") };
        if (errors && errors->GetStringLength() > 0) logf("dxc: %s\n", errors->GetStringPointer());
        ComPtr<IDxcBlob> blob; result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&blob), nullptr);
        return blob;
    }
};

// ------------------------------------------------------------------------------------------ GPU context
struct Stat { double median = 0, min = 0, max = 0; int reps = 0; };
struct Consts { uint32_t v[32]{}; uint32_t& operator()(int i, int c) { return v[i * 4 + c]; } };

struct Gpu
{
    bool warp = false, debug = false, gbv = false;
    ComPtr<ID3D12InfoQueue> infoQueue;
    uint32_t debugErrors = 0;
    ComPtr<IDXGIFactory6> factory;
    ComPtr<ID3D12Device5> dev;
    ComPtr<ID3D12CommandQueue> direct;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList6> list;
    ComPtr<ID3D12Fence> fence;
    UINT64 fenceValue = 0;
    HANDLE event = nullptr;
    ComPtr<ID3D12DescriptorHeap> heapCbv, heapDsv;
    UINT incCbv = 0, nextCbv = 0, incDsv = 0, nextDsv = 0;
    ComPtr<ID3D12QueryHeap> queryHeap;
    ComPtr<ID3D12Resource> queryReadback;
    UINT64 timestampFrequency = 0;
    ComPtr<ID3D12RootSignature> rootSig;
    std::string adapterName, driverVersion;
    int reps = 9, warm = 3;

    void init(bool useWarp, bool useDebug, bool useGbv)
    {
        warp = useWarp; debug = useDebug; gbv = useGbv;
        if (debug)
        {
            ComPtr<ID3D12Debug> dbg;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg))))
            {
                dbg->EnableDebugLayer();
                ComPtr<ID3D12Debug1> dbg1;
                if (gbv && SUCCEEDED(dbg.As(&dbg1))) dbg1->SetEnableGPUBasedValidation(TRUE);
                logf("D3D12 debug layer enabled%s\n", gbv ? " + GPU-based validation" : "");
            }
        }
        check(CreateDXGIFactory2(debug ? DXGI_CREATE_FACTORY_DEBUG : 0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
        if (warp)
        {
            ComPtr<IDXGIAdapter> a;
            check(factory->EnumWarpAdapter(IID_PPV_ARGS(&a)), "EnumWarpAdapter");
            if (FAILED(D3D12CreateDevice(a.Get(), D3D_FEATURE_LEVEL_12_2, IID_PPV_ARGS(&dev))))
                check(D3D12CreateDevice(a.Get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&dev)), "WARP device 12_1");
            adapterName = "WARP";
        }
        else
        {
            for (UINT i = 0;; ++i)
            {
                ComPtr<IDXGIAdapter4> a;
                if (FAILED(factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&a)))) break;
                DXGI_ADAPTER_DESC3 d; a->GetDesc3(&d);
                if (d.Flags & DXGI_ADAPTER_FLAG3_SOFTWARE) continue;
                if (SUCCEEDED(D3D12CreateDevice(a.Get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&dev))))
                {
                    char name[256]; wcstombs(name, d.Description, 255); adapterName = name;
                    LARGE_INTEGER umd{};
                    if (SUCCEEDED(a->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd)))
                    {
                        char v[64]; snprintf(v, sizeof v, "%u.%u.%u.%u", (unsigned)HIWORD(umd.HighPart), (unsigned)LOWORD(umd.HighPart), (unsigned)HIWORD(umd.LowPart), (unsigned)LOWORD(umd.LowPart));
                        driverVersion = v;
                    }
                    break;
                }
            }
            if (!dev) throw Failure{ "no D3D12 12_1 hardware adapter" };
        }
        if (debug) dev.As(&infoQueue);
        D3D12_FEATURE_DATA_SHADER_MODEL sm{ D3D_SHADER_MODEL_6_6 };
        check(dev->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof sm), "shader model");
        if (sm.HighestShaderModel < D3D_SHADER_MODEL_6_6) throw Failure{ "shader model 6.6 required" };
        D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7{}; dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &o7, sizeof o7);
        if (o7.MeshShaderTier == D3D12_MESH_SHADER_TIER_NOT_SUPPORTED) throw Failure{ "mesh shaders required" };
        { HMODULE core = GetModuleHandleA("D3D12Core.dll"); char path[MAX_PATH] = "(not loaded)"; if (core) GetModuleFileNameA(core, path, MAX_PATH); logf("Adapter: %s, driver %s, D3D12Core: %s\n", adapterName.c_str(), driverVersion.c_str(), path); }

        D3D12_COMMAND_QUEUE_DESC q{}; q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT; q.Priority = warp ? D3D12_COMMAND_QUEUE_PRIORITY_NORMAL : D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
        check(dev->CreateCommandQueue(&q, IID_PPV_ARGS(&direct)), "direct queue");
        check(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)), "alloc");
        check(dev->CreateCommandList1(0, D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_FLAG_NONE, IID_PPV_ARGS(&list)), "list");
        check(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        check(direct->GetTimestampFrequency(&timestampFrequency), "timestamp frequency");
        D3D12_DESCRIPTOR_HEAP_DESC h{}; h.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; h.NumDescriptors = 4096; h.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        check(dev->CreateDescriptorHeap(&h, IID_PPV_ARGS(&heapCbv)), "cbv heap");
        incCbv = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV; hd.NumDescriptors = 8;
        check(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heapDsv)), "dsv heap");
        incDsv = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        D3D12_QUERY_HEAP_DESC qh{}; qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; qh.Count = 64;
        check(dev->CreateQueryHeap(&qh, IID_PPV_ARGS(&queryHeap)), "query heap");
        queryReadback = buffer(64 * 8, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);

        D3D12_ROOT_PARAMETER1 param{};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; param.Constants.Num32BitValues = 32; param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_STATIC_SAMPLER_DESC samplers[2]{};
        for (int i = 0; i < 2; ++i)
        {
            samplers[i].Filter = i == 0 ? D3D12_FILTER_MIN_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            samplers[i].AddressU = samplers[i].AddressV = samplers[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samplers[i].MaxLOD = D3D12_FLOAT32_MAX; samplers[i].MaxAnisotropy = 1; samplers[i].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
            samplers[i].ShaderRegister = (UINT)i; samplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC rs{}; rs.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        rs.Desc_1_1.NumParameters = 1; rs.Desc_1_1.pParameters = &param; rs.Desc_1_1.NumStaticSamplers = 2; rs.Desc_1_1.pStaticSamplers = samplers;
        rs.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;
        ComPtr<ID3DBlob> blob, err;
        if (FAILED(D3D12SerializeVersionedRootSignature(&rs, &blob, &err))) throw Failure{ std::string("root signature: ") + (err ? (char*)err->GetBufferPointer() : "") };
        check(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rootSig)), "CreateRootSignature");
    }

    // ---- resources
    ComPtr<ID3D12Resource> buffer(UINT64 bytes, D3D12_HEAP_TYPE type = D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON,
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
    ComPtr<ID3D12Resource> texture(UINT w, UINT h, UINT depth, DXGI_FORMAT fmt, UINT mips, bool uav)
    {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{}; d.Dimension = depth > 1 ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = w; d.Height = h; d.DepthOrArraySize = (UINT16)depth; d.MipLevels = (UINT16)mips; d.Format = fmt; d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN; d.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
        ComPtr<ID3D12Resource> r;
        check(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, uav ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r)), "CreateCommittedResource(texture)");
        return r;
    }
    ComPtr<ID3D12Resource> depthTexture(UINT w, UINT h)
    {
        extern bool g_d16_fwd();
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1;
        d.Format = g_d16_fwd() ? DXGI_FORMAT_R16_TYPELESS : DXGI_FORMAT_R32_TYPELESS; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN; d.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE cv{}; cv.Format = g_d16_fwd() ? DXGI_FORMAT_D16_UNORM : DXGI_FORMAT_D32_FLOAT; cv.DepthStencil.Depth = 0.0f;
        ComPtr<ID3D12Resource> r;
        check(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, IID_PPV_ARGS(&r)), "CreateCommittedResource(depth)");
        return r;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE dsv(ID3D12Resource* r)
    {
        extern bool g_d16_fwd();
        D3D12_DEPTH_STENCIL_VIEW_DESC d{}; d.Format = g_d16_fwd() ? DXGI_FORMAT_D16_UNORM : DXGI_FORMAT_D32_FLOAT; d.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        auto h = heapDsv->GetCPUDescriptorHandleForHeapStart(); h.ptr += (SIZE_T)(nextDsv++) * incDsv; dev->CreateDepthStencilView(r, &d, h); return h;
    }
    UINT srvDepth(ID3D12Resource* r)
    {
        extern bool g_d16_fwd();
        D3D12_SHADER_RESOURCE_VIEW_DESC d{}; d.Format = g_d16_fwd() ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R32_FLOAT; d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; d.Texture2D.MipLevels = 1;
        UINT i = nextCbv++; dev->CreateShaderResourceView(r, &d, cbvHandle(i)); return i;
    }
    // Mesh pipeline writing hardware depth only (no pixel shader, no targets): the ROP path of the VSM page raster.
    ComPtr<ID3D12PipelineState> meshDepthPso(IDxcBlob* ms)
    {
        MeshStream s{};
        s.rs.value = rootSig.Get();
        s.ms.value = { ms->GetBufferPointer(), ms->GetBufferSize() };
        s.rast.value.FillMode = D3D12_FILL_MODE_SOLID; s.rast.value.CullMode = D3D12_CULL_MODE_NONE; s.rast.value.DepthClipEnable = TRUE;
        s.ds.value.DepthEnable = TRUE; s.ds.value.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL; s.ds.value.DepthFunc = D3D12_COMPARISON_FUNC_GREATER;
        extern bool g_d16_fwd();
        s.rtv.value.NumRenderTargets = 0; s.dsv.value = g_d16_fwd() ? DXGI_FORMAT_D16_UNORM : DXGI_FORMAT_D32_FLOAT;
        s.topo.value = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; s.sd.value = { 1, 0 }; s.mask.value = UINT_MAX;
        s.blend.value.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        D3D12_PIPELINE_STATE_STREAM_DESC d{ sizeof s, &s };
        ComPtr<ID3D12PipelineState> p; check(dev->CreatePipelineState(&d, IID_PPV_ARGS(&p)), "CreatePipelineState(mesh depth)"); return p;
    }
    ID3D12GraphicsCommandList6* begin()
    {
        check(alloc->Reset(), "alloc reset");
        check(list->Reset(alloc.Get(), nullptr), "list reset");
        ID3D12DescriptorHeap* heaps[] = { heapCbv.Get() };
        list->SetDescriptorHeaps(1, heaps);
        return list.Get();
    }
    void submitAndWait()
    {
        check(list->Close(), "Close");
        ID3D12CommandList* lists[] = { list.Get() };
        direct->ExecuteCommandLists(1, lists);
        check(direct->Signal(fence.Get(), ++fenceValue), "Signal");
        if (fence->GetCompletedValue() < fenceValue) { check(fence->SetEventOnCompletion(fenceValue, event), "SetEventOnCompletion"); WaitForSingleObject(event, INFINITE); }
        drainMessages();
    }
    void drainMessages()
    {
        if (!infoQueue) return;
        const UINT64 n = infoQueue->GetNumStoredMessages();
        for (UINT64 i = 0; i < n && i < 64; ++i)
        {
            SIZE_T len = 0; infoQueue->GetMessage(i, nullptr, &len);
            std::vector<uint8_t> buf(len); auto* m = (D3D12_MESSAGE*)buf.data();
            if (len && SUCCEEDED(infoQueue->GetMessage(i, m, &len))) { logf("  d3d12[%d]: %s\n", (int)m->Severity, m->pDescription); if (m->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) ++debugErrors; }
        }
        infoQueue->ClearStoredMessages();
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
    // Uploads mip 0 of a 2D or 3D texture from tightly packed rows (bpp bytes per texel); mips > 0 stay zero.
    void uploadTexture(ID3D12Resource* tex, const uint8_t* data, UINT w, UINT h, UINT depth, UINT bpp)
    {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp; UINT rows; UINT64 rowBytes, total;
        D3D12_RESOURCE_DESC d = tex->GetDesc();
        dev->GetCopyableFootprints(&d, 0, 1, 0, &fp, &rows, &rowBytes, &total);
        auto up = buffer(total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE);
        uint8_t* p; check(up->Map(0, nullptr, (void**)&p), "Map");
        for (UINT z = 0; z < depth; ++z)
            for (UINT y = 0; y < h; ++y)
                memcpy(p + fp.Offset + ((size_t)z * rows + y) * fp.Footprint.RowPitch, data + ((size_t)z * h + y) * w * bpp, (size_t)w * bpp);
        up->Unmap(0, nullptr);
        auto l = begin();
        const bool isUav = (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0;
        if (isUav) transition(l, tex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst{ tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {} }; dst.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION src{ up.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {} }; src.PlacedFootprint = fp;
        l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        if (isUav) transition(l, tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        submitAndWait();
    }

    // ---- descriptors (SM 6.6 heap indices)
    D3D12_CPU_DESCRIPTOR_HANDLE cbvHandle(UINT i) { auto h = heapCbv->GetCPUDescriptorHandleForHeapStart(); h.ptr += (SIZE_T)i * incCbv; return h; }
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
    UINT uavRaw(ID3D12Resource* r, UINT64 bytes)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC d{}; d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER; d.Format = DXGI_FORMAT_R32_TYPELESS; d.Buffer.NumElements = (UINT)(bytes / 4); d.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        UINT i = nextCbv++; dev->CreateUnorderedAccessView(r, nullptr, &d, cbvHandle(i)); return i;
    }
    UINT srvTexture(ID3D12Resource* r, DXGI_FORMAT fmt, UINT mips, bool volume = false)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC d{}; d.Format = fmt; d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (volume) { d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D; d.Texture3D.MipLevels = mips; }
        else { d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; d.Texture2D.MipLevels = mips; }
        UINT i = nextCbv++; dev->CreateShaderResourceView(r, &d, cbvHandle(i)); return i;
    }
    UINT uavTexture(ID3D12Resource* r, DXGI_FORMAT fmt)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC d{}; d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D; d.Format = fmt;
        UINT i = nextCbv++; dev->CreateUnorderedAccessView(r, nullptr, &d, cbvHandle(i)); return i;
    }

    // ---- pipelines
    ComPtr<ID3D12PipelineState> computePso(IDxcBlob* cs)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC d{}; d.pRootSignature = rootSig.Get(); d.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
        ComPtr<ID3D12PipelineState> p; check(dev->CreateComputePipelineState(&d, IID_PPV_ARGS(&p)), "CreateComputePipelineState"); return p;
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
    // Mesh + pixel pipeline without targets or depth (UAV-only raster), conservative rasterisation optional.
    ComPtr<ID3D12PipelineState> meshPso(IDxcBlob* ms, IDxcBlob* ps, bool conservative)
    {
        MeshStream s{};
        s.rs.value = rootSig.Get();
        s.ms.value = { ms->GetBufferPointer(), ms->GetBufferSize() };
        s.ps.value = { ps->GetBufferPointer(), ps->GetBufferSize() };
        s.rast.value.FillMode = D3D12_FILL_MODE_SOLID; s.rast.value.CullMode = D3D12_CULL_MODE_NONE; s.rast.value.DepthClipEnable = TRUE;
        s.rast.value.ConservativeRaster = conservative ? D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON : D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
        s.ds.value.DepthEnable = FALSE; s.ds.value.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        s.rtv.value.NumRenderTargets = 0; s.dsv.value = DXGI_FORMAT_UNKNOWN;
        s.topo.value = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; s.sd.value = { 1, 0 }; s.mask.value = UINT_MAX;
        s.blend.value.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        D3D12_PIPELINE_STATE_STREAM_DESC d{ sizeof s, &s };
        ComPtr<ID3D12PipelineState> p; check(dev->CreatePipelineState(&d, IID_PPV_ARGS(&p)), "CreatePipelineState(mesh)"); return p;
    }
    void setConstants(ID3D12GraphicsCommandList6* l, const uint32_t* c, bool graphics = false)
    {
        if (graphics) { l->SetGraphicsRootSignature(rootSig.Get()); l->SetGraphicsRoot32BitConstants(0, 32, c, 0); }
        else { l->SetComputeRootSignature(rootSig.Get()); l->SetComputeRoot32BitConstants(0, 32, c, 0); }
    }
    static void transition(ID3D12GraphicsCommandList6* l, ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = r; b.Transition.StateBefore = before; b.Transition.StateAfter = after;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; l->ResourceBarrier(1, &b);
    }
    static void uavBarrier(ID3D12GraphicsCommandList6* l)
    {
        D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; b.UAV.pResource = nullptr; l->ResourceBarrier(1, &b);
    }
    // GPU timestamps around 'rec'; 'pre' (optional) records untimed work before the first timestamp (cache flush).
    Stat time(const std::function<void(ID3D12GraphicsCommandList6*)>& rec, const std::function<void(ID3D12GraphicsCommandList6*)>& pre = nullptr, int repsOverride = -1)
    {
        const int n = repsOverride > 0 ? repsOverride : reps;
        std::vector<double> v;
        for (int i = 0; i < warm + n; ++i)
        {
            auto l = begin();
            if (pre) { pre(l); uavBarrier(l); }
            l->EndQuery(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
            rec(l);
            l->EndQuery(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
            l->ResolveQueryData(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, queryReadback.Get(), 0);
            submitAndWait();
            UINT64* q; D3D12_RANGE r{ 0, 16 }; check(queryReadback->Map(0, &r, (void**)&q), "Map query");
            const double ms = (double)(q[1] - q[0]) * 1000.0 / (double)timestampFrequency;
            queryReadback->Unmap(0, nullptr);
            if (i >= warm) v.push_back(ms);
        }
        std::sort(v.begin(), v.end());
        Stat s; s.reps = (int)v.size(); s.min = v.front(); s.max = v.back(); s.median = v[v.size() / 2];
        return s;
    }
};

static Gpu g;
static Dxc dxc;
static std::string g_shaderDir = DB_SHADER_DIR;
static uint32_t g_width = 3840, g_height = 2160;
static bool g_conservative = true, g_noHelper = false;  // WARP isolation switches
static bool g_d16 = false;  // --d16: VSM bench depth atlas in D16_UNORM instead of D32_FLOAT (revision 1 11.4 (6))
bool g_d16_fwd() { return g_d16; }
static ComPtr<ID3D12Resource> g_scratch; static UINT g_scratchUav = 0;  // 128 MB: warm-up target, L2 flush
static ComPtr<ID3D12PipelineState> g_warmPso, g_flushPso;

static const char* kWarmSource =
    "#include \"common.hlsli\"\n"
    "[numthreads(256,1,1)] void WarmCS(uint id : SV_DispatchThreadID) {\n"
    "  RWStructuredBuffer<uint> o = ResourceDescriptorHeap[P[0].y]; float4 a = float4(id, 1, 2, 3), b = a * 0.5 + 1, c = b, d = a; \n"
    "  [loop] for (uint i = 0; i < P[0].x && i < 8192; ++i) { a = a * b + c; b = b * c + d; c = c * d + a; d = d * a + b; }\n"
    "  if (a.x + b.y + c.z + d.w == 0x7fc00123) o[id & 1023] = 1; }\n"
    "[numthreads(256,1,1)] void FlushCS(uint id : SV_DispatchThreadID) {\n"
    "  RWStructuredBuffer<uint4> o = ResourceDescriptorHeap[P[0].y]; [unroll] for (uint i = 0; i < 8; ++i) o[(id * 8 + i) & (P[0].x - 1)] = uint4(id, i, 3, 4); }\n";

static void warmUp(const char* section)
{
    if (g.warp) { logf("\n== %s (WARP dry run, no warm-up)\n", section); return; }
    Consts c; c(0, 0) = 4096; c(0, 1) = g_scratchUav;
    const double t0 = now();
    while (now() - t0 < 1500) { auto l = g.begin(); g.setConstants(l, c.v); l->SetPipelineState(g_warmPso.Get()); l->Dispatch(8192, 1, 1); g.submitAndWait(); }
    logf("\n== %s (after 1.5 s warm-up)\n", section);
}
// Writes 128 MB so the L2 (64 MB) holds none of the benchmark's buffers.
static void flushL2(ID3D12GraphicsCommandList6* l)
{
    Consts c; c(0, 0) = (128u << 20) / 16; c(0, 1) = g_scratchUav;
    g.setConstants(l, c.v); l->SetPipelineState(g_flushPso.Get()); l->Dispatch((128u << 20) / 16 / 8 / 256, 1, 1);
}
static std::vector<uint8_t> randomBytes(size_t n, uint32_t seed)
{
    std::vector<uint8_t> v(n); std::mt19937 rng(seed);
    for (size_t i = 0; i < n; i += 4) { uint32_t r = rng(); memcpy(&v[i], &r, std::min<size_t>(4, n - i)); }
    return v;
}
static std::string src(const char* file) { return readFile(g_shaderDir + "/" + file); }
static std::wstring wdef(const char* name, uint32_t value) { wchar_t b[64]; swprintf(b, 64, L"%hs=%u", name, value); return b; }

// ------------------------------------------------------------------------------------------ 1. coverage: fragment pipeline
static void benchCoverage()
{
    warmUp("Band B fragment pipeline (conservative raster + exact area + mask + 24 B append, tile composite)");
    const std::string s = src("coverage2.hlsl");
    const uint32_t W = g_width, H = g_height, tilesX = (W + 7) / 8, tilesY = (H + 7) / 8, tiles = tilesX * tilesY;
    const uint32_t recordCap = g.warp ? (1u << 20) : (26u << 20);  // 26 M x 24 B = 624 MB
    auto records = g.buffer((UINT64)recordCap * 24); const UINT recUav = g.uavStructured(records.Get(), recordCap, 24), recSrv = g.srvStructured(records.Get(), recordCap, 24);
    auto counters = g.buffer(256); const UINT cntUav = g.uavStructured(counters.Get(), 64, 4);
    auto tileCount = g.buffer((UINT64)tiles * 4); const UINT tcUav = g.uavStructured(tileCount.Get(), tiles, 4), tcSrv = g.srvStructured(tileCount.Get(), tiles, 4);
    auto tileOffset = g.buffer((UINT64)tiles * 4); const UINT toUav = g.uavStructured(tileOffset.Get(), tiles, 4), toSrv = g.srvStructured(tileOffset.Get(), tiles, 4);
    auto tileCap = g.buffer((UINT64)tiles * 4); const UINT tcapUav = g.uavStructured(tileCap.Get(), tiles, 4), tcapSrv = g.srvStructured(tileCap.Get(), tiles, 4);
    auto total = g.buffer(256); const UINT totUav = g.uavStructured(total.Get(), 64, 4);
    // Material / K stand-in texture: 1024^2 RGBA8 with 4 mips (mip 0 uploaded, others zero).
    auto tex = g.texture(1024, 1024, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 4, false);
    { auto bytes = randomBytes(1024 * 1024 * 4, 7); g.uploadTexture(tex.Get(), bytes.data(), 1024, 1024, 1, 4); }
    const UINT texSrv = g.srvTexture(tex.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 4);
    auto out = g.texture(W, H, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 1, true); const UINT outUav = g.uavTexture(out.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);

    logf("  compiling coverage kernels\n");
    auto ms = dxc.compile(s, L"SliverMS", L"ms_6_6", {});
    auto msCards = dxc.compile(s, L"SliverMS", L"ms_6_6", { L"CARDS=1" });
    std::vector<std::wstring> helper; if (g_noHelper) helper.push_back(L"NO_HELPER=1");
    std::vector<std::wstring> countDefs = helper; countDefs.push_back(L"COUNT=1");
    auto psCount = dxc.compile(s, L"FragPS", L"ps_6_6", countDefs);
    ComPtr<IDxcBlob> psMode[3];
    for (uint32_t m = 0; m < 3; ++m) { std::vector<std::wstring> d = helper; d.push_back(wdef("APPEND_MODE", m)); psMode[m] = dxc.compile(s, L"FragPS", L"ps_6_6", d); }
    logf("  creating mesh pipelines\n");
    auto psoCount = g.meshPso(ms.Get(), psCount.Get(), g_conservative), psoCountCards = g.meshPso(msCards.Get(), psCount.Get(), g_conservative);
    ComPtr<ID3D12PipelineState> psoMode[3], psoModeCards[3];
    for (uint32_t m = 0; m < 3; ++m) { psoMode[m] = g.meshPso(ms.Get(), psMode[m].Get(), g_conservative); psoModeCards[m] = g.meshPso(msCards.Get(), psMode[m].Get(), g_conservative); }
    auto psoPrefix = g.computePso(dxc.compile(s, L"PrefixCS", L"cs_6_6", {}).Get());
    auto psoComposite = g.computePso(dxc.compile(s, L"CompositeCS", L"cs_6_6", {}).Get());
    logf("  pipelines ready\n");

    struct Case { const char* name; bool cards; float width, length; uint32_t slivers; };
    std::vector<Case> cases;
    if (g.warp) cases = { { "slivers 0.5 px x 20 px", false, 0.5f, 20.f, 1024 }, { "cards 4 px", true, 4.f, 4.f, 1024 } };
    else cases = { { "slivers 0.5 px x 20 px, F ~5 M", false, 0.5f, 20.f, 76000 }, { "slivers 0.5 px x 20 px, F ~10 M", false, 0.5f, 20.f, 152000 },
                   { "slivers 0.5 px x 20 px, F ~20 M", false, 0.5f, 20.f, 304000 }, { "slivers 0.25 px x 20 px, F ~10 M", false, 0.25f, 20.f, 152000 },
                   { "cards 4 px, F ~10 M", true, 4.f, 4.f, 380000 } };
    for (const Case& cs : cases)
    {
        const uint32_t groups = (cs.slivers + 31) / 32, gx = std::min<uint32_t>(groups, 512), gy = (groups + gx - 1) / gx;
        Consts c;
        c(0, 0) = asu((float)W); c(0, 1) = asu((float)H); c(0, 2) = asu(cs.width); c(0, 3) = asu(cs.length);
        c(1, 0) = recUav; c(1, 1) = cntUav; c(1, 2) = tcUav; c(1, 3) = toSrv;
        c(2, 0) = tcapSrv; c(2, 1) = 12345; c(2, 2) = gx; c(2, 3) = tilesX;
        c(3, 0) = recordCap; c(3, 1) = 1024; c(3, 2) = texSrv; c(3, 3) = outUav;
        auto recordRaster = [&](ID3D12GraphicsCommandList6* l, ID3D12PipelineState* pso) {
            D3D12_VIEWPORT vp{ 0, 0, (float)W, (float)H, 0, 1 }; D3D12_RECT sc{ 0, 0, (LONG)W, (LONG)H };
            l->RSSetViewports(1, &vp); l->RSSetScissorRects(1, &sc);
            l->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
            g.setConstants(l, c.v, true); l->SetPipelineState(pso); l->DispatchMesh(gx, gy, 1); Gpu::uavBarrier(l); };
        auto clearCounts = [&](ID3D12GraphicsCommandList6* l) {
            // Counters and tile counts to zero: ClearUnorderedAccessViewUint needs a non-shader-visible copy; a small kernel is simpler.
            const UINT zero[4] = { 0, 0, 0, 0 };
            (void)l; (void)zero; };
        (void)clearCounts;
        std::vector<uint32_t> zeroTiles(tiles, 0), zero64(64, 0);
        auto zeroBuffers = [&]() { g.upload(tileCount.Get(), zeroTiles.data(), (UINT64)tiles * 4); g.upload(counters.Get(), zero64.data(), 256); };
        // (a) count pass: tile counts -> prefix (offsets, capacities = count * 1.25 + 8) -> total capacity.
        zeroBuffers();
        logf("  %s: count pass\n", cs.name);
        Stat sCount = g.time([&](ID3D12GraphicsCommandList6* l) { recordRaster(l, cs.cards ? psoCountCards.Get() : psoCount.Get()); });
        zeroBuffers();
        { auto l = g.begin(); recordRaster(l, cs.cards ? psoCountCards.Get() : psoCount.Get()); g.submitAndWait(); }
        {
            Consts pc; pc(0, 0) = tiles; pc(1, 0) = tcSrv; pc(1, 1) = toUav; pc(1, 2) = tcapUav; pc(1, 3) = totUav;
            auto l = g.begin(); g.setConstants(l, pc.v); l->SetPipelineState(psoPrefix.Get()); l->Dispatch(1, 1, 1); g.submitAndWait();
        }
        logf("  prefix done\n");
        const auto countsHost = g.readback(tileCount.Get(), (UINT64)tiles * 4);
        const auto totalHost = g.readback(total.Get(), 256);
        double fragments = 0; uint32_t over1024 = 0, maxTile = 0;
        for (uint32_t t = 0; t < tiles; ++t) { const uint32_t n = ((const uint32_t*)countsHost.data())[t]; fragments += n; over1024 += n > 1024; maxTile = std::max(maxTile, n); }
        const uint32_t capacityTotal = ((const uint32_t*)totalHost.data())[0];
        if (capacityTotal > recordCap) { logf("  %s: capacity %u exceeds the record buffer (%u); skipped\n", cs.name, capacityTotal, recordCap); continue; }
        record("coverage", std::string(cs.name) + ": count pass (conservative raster, tile atomics only)", sCount.median, "ms",
               std::to_string(fragments / 1e6) + " M fragments, max/tile " + std::to_string(maxTile) + ", tiles > 1024: " + std::to_string(over1024));
        // (b) append passes, three modes. Tile counts are re-zeroed before every repetition (untimed, in the same list).
        auto zeroKernel = [&](ID3D12GraphicsCommandList6* l) {
            // Zero via copy from an upload buffer of zeros (kept alive for the run).
            (void)l; };
        (void)zeroKernel;
        auto zeroUpload = g.buffer((UINT64)tiles * 4 + 256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE);
        { void* p; check(zeroUpload->Map(0, nullptr, &p), "map zero"); memset(p, 0, (size_t)tiles * 4 + 256); zeroUpload->Unmap(0, nullptr); }
        auto zeroPre = [&](ID3D12GraphicsCommandList6* l) { l->CopyBufferRegion(tileCount.Get(), 0, zeroUpload.Get(), 0, (UINT64)tiles * 4); l->CopyBufferRegion(counters.Get(), 0, zeroUpload.Get(), 0, 256); };
        static const char* const modeNames[3] = { "global list, one atomic per wave", "tile segments, one atomic per fragment", "tile segments, one atomic per (wave, tile)" };
        for (uint32_t m = 0; m < 3; ++m)
        {
            ID3D12PipelineState* pso = cs.cards ? psoModeCards[m].Get() : psoMode[m].Get();
            Stat sa = g.time([&](ID3D12GraphicsCommandList6* l) { recordRaster(l, pso); }, zeroPre);
            const auto cnt = g.readback(counters.Get(), 256);
            const uint32_t appended = ((const uint32_t*)cnt.data())[0], overflow = ((const uint32_t*)cnt.data())[1];
            record("coverage", std::string(cs.name) + ": raster + area + mask + 24 B append (" + modeNames[m] + ")", sa.median, "ms",
                   std::to_string(appended / 1e6) + " M appended, " + std::to_string(overflow) + " overflow, " + std::to_string(sa.median * 1e6 / std::max(1u, appended)) + " ns/fragment");
        }
        if (!g.warp && std::string(cs.name).find("F ~10 M") != std::string::npos && !cs.cards && cs.width == 0.5f)
        {
            struct AV { const char* name; uint32_t area, mask, count; };
            const AV avs[] = { { "area: Green's theorem (register resident), mask 32", 1, 1, 0 }, { "area: Sutherland-Hodgman arrays, no mask", 0, 0, 0 },
                               { "area: Green's theorem, no mask", 1, 0, 0 }, { "count pass with Green's-theorem area", 1, 1, 1 } };
            for (const AV& av : avs)
            {
                std::vector<std::wstring> d = helper; d.push_back(wdef("AREA", av.area)); d.push_back(wdef("MASK", av.mask)); d.push_back(av.count ? L"COUNT=1" : wdef("APPEND_MODE", 1));
                auto psv = dxc.compile(s, L"FragPS", L"ps_6_6", d);
                auto psov = g.meshPso(ms.Get(), psv.Get(), g_conservative);
                Stat sv = g.time([&](ID3D12GraphicsCommandList6* l) { recordRaster(l, psov.Get()); }, zeroPre);
                record("coverage", std::string(cs.name) + ": " + av.name + (av.count ? "" : " (tile segments, per-fragment atomic)"), sv.median, "ms",
                       std::to_string(sv.median * 1e6 / fragments) + " ns/fragment");
            }
        }
        // (c) composite from the tile segments of mode 1 (records as left by the last repetition of mode 2: same layout).
        {
            Consts cc = c; cc(1, 0) = recSrv; cc(1, 1) = tcSrv; cc(1, 2) = toSrv; cc(2, 0) = tcapSrv;
            Stat sc = g.time([&](ID3D12GraphicsCommandList6* l) { g.setConstants(l, cc.v); l->SetPipelineState(psoComposite.Get()); l->Dispatch(tilesX, tilesY, 1); Gpu::uavBarrier(l); }, flushL2);
            const double perFrag = sc.median * 1e6 / std::max(1.0, fragments), perPixel = sc.median * 1e6 / ((double)W * H);
            record("coverage", std::string(cs.name) + ": tile composite (groupshared sort by pixel/depth, mask union, 2 taps/fragment)", sc.median, "ms",
                   std::to_string(perFrag) + " ns/fragment, " + std::to_string(perPixel) + " ns/pixel (all pixels)");
        }
        if (g.warp)
        {
            const auto o = g.readback(counters.Get(), 256);
            logf("  WARP check: %.0f fragments counted, %u appended in mode 2, %u overflow\n", fragments, ((const uint32_t*)o.data())[0], ((const uint32_t*)o.data())[1]);
        }
    }
}

// ------------------------------------------------------------------------------------------ 2. bricks: DDA march
static void benchBricks()
{
    warmUp("Band C brick DDA (16^3 bricks, table indirection, camera and sun marches, entry maps)");
    const std::string s = src("bricks.hlsl");
    const uint32_t W = g_width, H = g_height;
    const uint32_t Bx = g.warp ? 6 : 24, By = g.warp ? 4 : 8, Bz = g.warp ? 6 : 24, bricks = Bx * By * Bz;
    std::vector<uint32_t> table(bricks);
    { std::mt19937 rng(3); for (uint32_t i = 0; i < bricks; ++i) table[i] = i; std::shuffle(table.begin(), table.end(), rng); }
    auto tableBuf = g.buffer((UINT64)bricks * 4); g.upload(tableBuf.Get(), table.data(), (UINT64)bricks * 4);
    const UINT tableSrv = g.srvStructured(tableBuf.Get(), bricks, 4);
    auto output = g.buffer((UINT64)W * H * 32); const UINT outUav = g.uavStructured(output.Get(), W * H, 32);
    auto maps = g.buffer((UINT64)bricks * 256); const UINT mapsUav = g.uavRaw(maps.Get(), (UINT64)bricks * 256);
    for (uint32_t voxelBytes : { 1u, 8u })
    {
        const UINT64 bytes = (UINT64)bricks * 4096 * voxelBytes;
        // Sparse density: 30 % of voxels carry density 0.02..0.3 (in exp2 units per voxel), the rest 0.
        std::vector<uint8_t> vox(bytes, 0);
        { std::mt19937 rng(11); for (UINT64 i = 0; i < (UINT64)bricks * 4096; ++i) if ((rng() & 1023) < 307) vox[i * voxelBytes] = (uint8_t)(5 + (rng() % 70)); }
        auto voxBuf = g.buffer(bytes); g.upload(voxBuf.Get(), vox.data(), bytes);
        const UINT voxSrv = g.srvRaw(voxBuf.Get(), bytes);
        for (uint32_t steps : { 16u, 32u, 48u })
        {
            auto pso = g.computePso(dxc.compile(s, L"MarchCS", L"cs_6_6", { wdef("VOXEL_BYTES", voxelBytes), wdef("STEPS", steps) }).Get());
            for (uint32_t mode : { 0u, 1u })
            {
                Consts c; c(0, 0) = W; c(0, 1) = H; c(0, 2) = steps; c(0, 3) = mode;
                c(1, 0) = tableSrv; c(1, 1) = voxSrv; c(1, 2) = outUav; c(1, 3) = mapsUav; c(2, 0) = Bx; c(2, 1) = By; c(2, 2) = Bz; c(2, 3) = bricks;
                Stat st = g.time([&](ID3D12GraphicsCommandList6* l) { g.setConstants(l, c.v); l->SetPipelineState(pso.Get()); l->Dispatch((W + 7) / 8, (H + 7) / 8, 1); Gpu::uavBarrier(l); }, flushL2);
                char name[200];
                snprintf(name, sizeof name, "%s march, %u B voxels (%.0f MB), %u steps, 32 B record", mode == 0 ? "camera" : "sun (orthographic)", voxelBytes, bytes / 1048576.0, steps);
                record("bricks", name, st.median, "ms", std::to_string(st.median * 1e6 / ((double)W * H)) + " ns/pixel, " + std::to_string(st.median * 1e6 / ((double)W * H * steps)) + " ns/step (upper bound: early exits)");
            }
            if (steps == 32 || g.warp)
            {
                // Revision 1 11.4 (5): the receiver sun march (from the ground up toward the sun, 4 B T only) against the
                // texel sun march (top face down) with and without the 32 B record.
                auto psoT = g.computePso(dxc.compile(s, L"MarchCS", L"cs_6_6", { wdef("VOXEL_BYTES", voxelBytes), wdef("STEPS", steps), L"OUT_T=1" }).Get());
                for (uint32_t mode : { 1u, 2u })
                {
                    Consts c; c(0, 0) = W; c(0, 1) = H; c(0, 2) = steps; c(0, 3) = mode;
                    c(1, 0) = tableSrv; c(1, 1) = voxSrv; c(1, 2) = outUav; c(1, 3) = mapsUav; c(2, 0) = Bx; c(2, 1) = By; c(2, 2) = Bz; c(2, 3) = bricks;
                    Stat st = g.time([&](ID3D12GraphicsCommandList6* l) { g.setConstants(l, c.v); l->SetPipelineState(psoT.Get()); l->Dispatch((W + 7) / 8, (H + 7) / 8, 1); Gpu::uavBarrier(l); }, flushL2);
                    char name[200];
                    snprintf(name, sizeof name, "%s, %u B voxels, %u steps, 4 B T only", mode == 1 ? "sun texel march (top face down)" : "receiver sun march (ground up toward the sun)", voxelBytes, steps);
                    record("bricks", name, st.median, "ms", std::to_string(st.median * 1e6 / ((double)W * H)) + " ns/pixel (receiver)");
                }
                // Revision 1 14.4 item 1: sparse octree with empty bricks skipped. Occupancy = fraction of bricks that hold
                // voxels; the rest are marked empty in the table and the march jumps to their exit.
                if (voxelBytes == 1)
                {
                    auto psoS = g.computePso(dxc.compile(s, L"MarchCS", L"cs_6_6", { wdef("VOXEL_BYTES", voxelBytes), wdef("STEPS", steps), L"OUT_T=1", L"SPARSE=1" }).Get());
                    for (float occupancy : { 1.0f, 0.6f, 0.4f, 0.2f })
                    {
                        std::vector<uint32_t> sparse(table);
                        { std::mt19937 rng(17); for (uint32_t i = 0; i < bricks; ++i) if ((rng() % 1000) >= (uint32_t)(occupancy * 1000)) sparse[i] = 0xFFFFFFFFu; }
                        auto sparseBuf = g.buffer((UINT64)bricks * 4); g.upload(sparseBuf.Get(), sparse.data(), (UINT64)bricks * 4);
                        const UINT sparseSrv = g.srvStructured(sparseBuf.Get(), bricks, 4);
                        Consts c; c(0, 0) = W; c(0, 1) = H; c(0, 2) = steps; c(0, 3) = 2;
                        c(1, 0) = sparseSrv; c(1, 1) = voxSrv; c(1, 2) = outUav; c(1, 3) = mapsUav; c(2, 0) = Bx; c(2, 1) = By; c(2, 2) = Bz; c(2, 3) = bricks;
                        Stat st = g.time([&](ID3D12GraphicsCommandList6* l) { g.setConstants(l, c.v); l->SetPipelineState(psoS.Get()); l->Dispatch((W + 7) / 8, (H + 7) / 8, 1); Gpu::uavBarrier(l); }, flushL2);
                        char name[200];
                        snprintf(name, sizeof name, "receiver sun march, sparse skip, brick occupancy %.0f %%, %u B voxels, %u steps, 4 B T only", occupancy * 100, voxelBytes, steps);
                        record("bricks", name, st.median, "ms", std::to_string(st.median * 1e6 / ((double)W * H)) + " ns/receiver");
                    }
                }
            }
        }
        {
            auto pso = g.computePso(dxc.compile(s, L"EntryMapCS", L"cs_6_6", { wdef("VOXEL_BYTES", voxelBytes) }).Get());
            Consts c; c(0, 0) = W; c(0, 1) = H; c(1, 1) = voxSrv; c(1, 3) = mapsUav; c(2, 3) = bricks;
            Stat st = g.time([&](ID3D12GraphicsCommandList6* l) { g.setConstants(l, c.v); l->SetPipelineState(pso.Get()); l->Dispatch(bricks, 1, 1); Gpu::uavBarrier(l); }, flushL2);
            char name[200];
            snprintf(name, sizeof name, "entry maps: %u bricks x 256 cells x 16 steps, %u B voxels", bricks, voxelBytes);
            record("bricks", name, st.median, "ms", std::to_string(st.median * 1e6 / bricks) + " ns/brick");
        }
        if (g.warp)
        {
            const auto o = g.readback(output.Get(), 64);
            logf("  WARP check: first record T=%g depth=%g entries=%u\n", ((const float*)o.data())[0], ((const float*)o.data())[1], ((const uint32_t*)o.data())[3]);
        }
    }
}

// ------------------------------------------------------------------------------------------ 3. bands
static void benchBands()
{
    warmUp("Banded pixel passes (resolve 8 B in / 12 B out, shade 32-48 B in / 4 B out)");
    const std::string s = src("bands.hlsl");
    for (uint32_t resIndex = 0; resIndex < (g.warp ? 1u : 2u); ++resIndex)
    {
        const uint32_t W = resIndex == 0 ? g_width : 2560, H = resIndex == 0 ? g_height : 1440;
        auto make = [&](DXGI_FORMAT f, uint32_t bpp, bool uav, uint32_t seed) {
            auto t = g.texture(W, H, 1, f, 1, uav);
            auto bytes = randomBytes((size_t)W * H * bpp, seed); g.uploadTexture(t.Get(), bytes.data(), W, H, 1, bpp);
            return t; };
        auto vis = make(DXGI_FORMAT_R32_UINT, 4, false, 1), depth = make(DXGI_FORMAT_R32_FLOAT, 4, false, 2), gbuffer = make(DXGI_FORMAT_R32G32_UINT, 8, true, 3), word = make(DXGI_FORMAT_R32_UINT, 4, true, 4);
        auto shadow = make(DXGI_FORMAT_R32_UINT, 4, false, 5), irr = make(DXGI_FORMAT_R32G32_UINT, 8, false, 6), e0 = make(DXGI_FORMAT_R32G32_UINT, 8, false, 7), e1 = make(DXGI_FORMAT_R32G32_UINT, 8, false, 8), e2 = make(DXGI_FORMAT_R32G32_UINT, 8, false, 9);
        auto out = g.texture(W, H, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 1, true);
        Consts c;
        c(0, 0) = W; c(0, 1) = H;
        c(1, 0) = g.srvTexture(vis.Get(), DXGI_FORMAT_R32_UINT, 1); c(1, 1) = g.srvTexture(depth.Get(), DXGI_FORMAT_R32_FLOAT, 1);
        const UINT gbUav = g.uavTexture(gbuffer.Get(), DXGI_FORMAT_R32G32_UINT), gbSrv = g.srvTexture(gbuffer.Get(), DXGI_FORMAT_R32G32_UINT, 1);
        const UINT wdUav = g.uavTexture(word.Get(), DXGI_FORMAT_R32_UINT), wdSrv = g.srvTexture(word.Get(), DXGI_FORMAT_R32_UINT, 1);
        c(2, 0) = g.srvTexture(shadow.Get(), DXGI_FORMAT_R32_UINT, 1); c(2, 1) = g.srvTexture(irr.Get(), DXGI_FORMAT_R32G32_UINT, 1); c(2, 2) = g.uavTexture(out.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
        c(2, 3) = g.srvTexture(e0.Get(), DXGI_FORMAT_R32G32_UINT, 1); c(3, 0) = g.srvTexture(e1.Get(), DXGI_FORMAT_R32G32_UINT, 1); c(3, 1) = g.srvTexture(e2.Get(), DXGI_FORMAT_R32G32_UINT, 1);
        auto psoResolve = g.computePso(dxc.compile(s, L"ResolveCS", L"cs_6_6", {}).Get());
        for (uint32_t extra : { 0u, 2u })
        {
            auto psoShade = g.computePso(dxc.compile(s, L"ShadeCS", L"cs_6_6", { wdef("EXTRA_READS", extra) }).Get());
            for (uint32_t bands : { 1u, 4u, 8u, 16u })
            {
                if (g.warp && bands > 4) continue;
                const uint32_t rowsPer = (H + bands - 1) / bands;
                Stat st = g.time([&](ID3D12GraphicsCommandList6* l) {
                    for (uint32_t b = 0; b < bands; ++b)
                    {
                        const uint32_t y0 = b * rowsPer, y1 = std::min(H, y0 + rowsPer);
                        Consts cr = c; cr(0, 2) = y0; cr(0, 3) = y1; cr(1, 2) = gbUav; cr(1, 3) = wdUav;
                        g.setConstants(l, cr.v); l->SetPipelineState(psoResolve.Get()); l->Dispatch((W + 7) / 8, (y1 - y0 + 7) / 8, 1);
                        Gpu::transition(l, gbuffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                        Gpu::transition(l, word.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                        Consts csd = c; csd(0, 2) = y0; csd(0, 3) = y1; csd(1, 2) = gbSrv; csd(1, 3) = wdSrv;
                        g.setConstants(l, csd.v); l->SetPipelineState(psoShade.Get()); l->Dispatch((W + 7) / 8, (y1 - y0 + 7) / 8, 1);
                        Gpu::transition(l, gbuffer.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                        Gpu::transition(l, word.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                        Gpu::uavBarrier(l);
                    } }, flushL2);
                char name[200];
                snprintf(name, sizeof name, "%ux%u resolve + shade (%u B/px read in shade), %u band%s", W, H, 32 + extra * 8, bands, bands == 1 ? " (full screen)" : "s");
                record("bands", name, st.median, "ms", std::to_string(st.median * 1e6 / ((double)W * H)) + " ns/pixel");
            }
        }
    }
}

// ------------------------------------------------------------------------------------------ 4. shading kernel
static void benchShade()
{
    warmUp("Shading kernel: tile groupshared probe SH, in-kernel air fetches, K tap, 8 lights, register pressure");
    const std::string s = src("shade2.hlsl");
    const uint32_t W = g_width, H = g_height, probesX = (W + 7) / 8, probesY = (H + 7) / 8;
    auto gbuffer = g.texture(W, H, 1, DXGI_FORMAT_R32G32_UINT, 1, false); { auto b = randomBytes((size_t)W * H * 8, 21); g.uploadTexture(gbuffer.Get(), b.data(), W, H, 1, 8); }
    auto depth = g.texture(W, H, 1, DXGI_FORMAT_R32_FLOAT, 1, false);
    { std::vector<float> d((size_t)W * H); std::mt19937 rng(22); for (auto& v : d) v = 1.0f + (rng() % 10000) * 0.05f; g.uploadTexture(depth.Get(), (const uint8_t*)d.data(), W, H, 1, 4); }
    auto vis = g.texture(W, H, 1, DXGI_FORMAT_R32_UINT, 1, false); { auto b = randomBytes((size_t)W * H * 4, 23); g.uploadTexture(vis.Get(), b.data(), W, H, 1, 4); }
    // Probes: 80 B records {position, normal (oct as uint), 27 fp16 SH + occlusion}.
    const uint32_t probes = probesX * probesY;
    std::vector<float> probeData((size_t)probes * 20);
    { std::mt19937 rng(24); for (uint32_t p = 0; p < probes; ++p) { float* r = &probeData[(size_t)p * 20]; r[0] = (float)(p % probesX) * 8 * 0.02f; r[1] = (float)(p / probesX) * 8 * 0.01f; r[2] = 5.0f + (rng() % 100) * 0.1f; uint32_t oct = 0x7FFF7FFFu; memcpy(&r[3], &oct, 4);
        for (int i = 4; i < 20; ++i) { uint32_t two = (uint32_t)(0x3000 + (rng() % 0x0800)) | ((uint32_t)(0x3000 + (rng() % 0x0800)) << 16); memcpy(&r[i], &two, 4); } } }
    auto probeBuf = g.buffer((UINT64)probes * 80); g.upload(probeBuf.Get(), probeData.data(), (UINT64)probes * 80);
    const UINT probeSrv = g.srvStructured(probeBuf.Get(), probes, 80);
    const uint32_t VX = 160, VY = 90, VZ = 195;
    auto volume = g.texture(VX, VY, VZ, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, false); { auto b = randomBytes((size_t)VX * VY * VZ * 8, 25); for (size_t i = 1; i < b.size(); i += 2) b[i] &= 0x3F; g.uploadTexture(volume.Get(), b.data(), VX, VY, VZ, 8); }
    const UINT volumeSrv = g.srvTexture(volume.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, 1, true);
    auto atlas = g.texture(probesX * 14, probesY * 8, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 1, false); { auto b = randomBytes((size_t)probesX * 14 * probesY * 8 * 4, 26); g.uploadTexture(atlas.Get(), b.data(), probesX * 14, probesY * 8, 1, 4); }
    const UINT atlasSrv = g.srvTexture(atlas.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 1);
    std::vector<float> lights(8 * 20, 0); for (int i = 0; i < 8; ++i) { float* l = &lights[i * 20]; l[0] = (float)(i * 3 - 10); l[1] = 4; l[2] = (float)(20 + i * 7); l[3] = 30; l[4] = 1; l[5] = 0.8f; l[6] = 0.6f; l[7] = 200; }
    auto lightBuf = g.buffer(8 * 80); g.upload(lightBuf.Get(), lights.data(), 8 * 80); const UINT lightSrv = g.srvStructured(lightBuf.Get(), 8, 80);
    auto out = g.texture(W, H, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 1, true); const UINT outUav = g.uavTexture(out.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
    Consts c; c(0, 0) = W; c(0, 1) = H; c(0, 2) = probesX; c(0, 3) = probesY;
    c(1, 0) = g.srvTexture(gbuffer.Get(), DXGI_FORMAT_R32G32_UINT, 1); c(1, 1) = g.srvTexture(depth.Get(), DXGI_FORMAT_R32_FLOAT, 1); c(1, 2) = g.srvTexture(vis.Get(), DXGI_FORMAT_R32_UINT, 1); c(1, 3) = probeSrv;
    c(2, 0) = volumeSrv; c(2, 1) = atlasSrv; c(2, 2) = lightSrv; c(2, 3) = outUav;
    struct Variant { const char* name; uint32_t tileSh, air, ktap, lights, extra; };
    const Variant variants[] = {
        { "full: tile SH + air 3 + K + 8 lights", 1, 1, 1, 8, 0 },
        { "per-pixel 4 probe loads instead of tile SH", 0, 1, 1, 8, 0 },
        { "no air fetches", 1, 0, 1, 8, 0 },
        { "no K tap", 1, 1, 0, 8, 0 },
        { "no local lights", 1, 1, 1, 0, 0 },
        { "inputs and sun only (no SH, air, K, lights)", 0, 0, 0, 0, 0 },
        { "full + 32 extra live values", 1, 1, 1, 8, 32 },
        { "full + 64 extra live values", 1, 1, 1, 8, 64 },
        { "full + 128 extra live values", 1, 1, 1, 8, 128 },
    };
    for (const Variant& v : variants)
    {
        std::vector<std::wstring> defs = { wdef("TILE_SH", v.tileSh), wdef("AIR", v.air), wdef("KTAP", v.ktap), wdef("LIGHTS", v.lights), wdef("EXTRA", v.extra) };
        if (v.name[0] == 'i') defs[0] = L"TILE_SH=0";  // the input-only variant does no SH at all: keep TILE_SH 0 and weight nothing
        auto pso = g.computePso(dxc.compile(s, L"ShadeCS", L"cs_6_6", defs).Get());
        Stat st = g.time([&](ID3D12GraphicsCommandList6* l) { g.setConstants(l, c.v); l->SetPipelineState(pso.Get()); l->Dispatch((W + 7) / 8, (H + 7) / 8, 1); Gpu::uavBarrier(l); }, flushL2);
        record("shade", std::string("4K-class shading kernel: ") + v.name, st.median, "ms", std::to_string(st.median * 1e6 / ((double)W * H)) + " ns/pixel");
    }
}

// ------------------------------------------------------------------------------------------ 5. VSM dirty-page raster (S request)
static void benchVsm()
{
    warmUp("VSM dirty-page raster: UAV InterlockedMax pool vs hardware depth atlas + encode/pagemax pass");
    const std::string s = src("vsmraster.hlsl");
    auto ms = dxc.compile(s, L"PageMS", L"ms_6_6", {});
    auto psPool = dxc.compile(s, L"PoolPS", L"ps_6_6", { L"UAV=1" });
    auto psoUav = g.meshPso(ms.Get(), psPool.Get(), false);
    auto psoDepth = g.meshDepthPso(ms.Get());
    auto psoEncode = g.computePso(dxc.compile(s, L"EncodeCS", L"cs_6_6", {}).Get());
    auto psoClear = g.computePso(dxc.compile(s, L"ClearCS", L"cs_6_6", {}).Get());
    struct Case { const char* name; uint32_t pagesX, pagesY, trisPerPage; float edge; };
    std::vector<Case> cases;
    if (g.warp || g_width <= 256) cases = { { "6x6 pages, 64 tris/page, edge 8 px", 6, 6, 64, 8.f } };  // WARP / small hardware debug pass
    else cases = { { "4K sun moving: 3600 dirty pages (60x60), 576 tris/page (2.07 M), edge 8 px (32 px^2)", 60, 60, 576, 8.f },
                   { "same pages, 2304 tris/page (8.3 M), edge 2 px (2 px^2)", 60, 60, 2304, 2.f },
                   { "design wind case: 400 dirty pages (20x20), 576 tris/page, edge 8 px", 20, 20, 576, 8.f },
                   { "1440p sun moving: 1936 dirty pages (44x44), 1536 tris/page (3.0 M), edge 8 px", 44, 44, 1536, 8.f } };
    for (const Case& cs : cases)
    {
        const uint32_t pages = cs.pagesX * cs.pagesY, W = cs.pagesX * 128, H = cs.pagesY * 128;
        const UINT64 poolBytes = (UINT64)pages * 16384 * 4, blockBytes = (UINT64)pages * 17 * 16 * 8;
        auto pool = g.buffer(poolBytes); const UINT poolUav = g.uavRaw(pool.Get(), poolBytes);
        auto blocks = g.buffer(blockBytes); const UINT blocksUav = g.uavRaw(blocks.Get(), blockBytes);
        auto atlas = g.depthTexture(W, H); const D3D12_CPU_DESCRIPTOR_HANDLE dsv = g.dsv(atlas.Get()); const UINT atlasSrv = g.srvDepth(atlas.Get());
        const uint32_t groupsPerPage = std::max(cs.trisPerPage / 64, 1u), groups = pages * groupsPerPage, gx = std::min<uint32_t>(groups, 4096), gy = (groups + gx - 1) / gx;
        const double tris = (double)groups * 64, fragments = tris * 0.5 * cs.edge * cs.edge;
        Consts c; c(0, 0) = cs.pagesX; c(0, 1) = cs.pagesY; c(0, 2) = asu(cs.edge); c(0, 3) = cs.trisPerPage;
        c(1, 0) = poolUav; c(1, 1) = 77; c(1, 2) = gx; c(1, 3) = (uint32_t)std::min<UINT64>(poolBytes, 0xFFFFFFF0u); c(2, 0) = atlasSrv; c(2, 1) = blocksUav;
        auto viewport = [&](ID3D12GraphicsCommandList6* l) { D3D12_VIEWPORT vp{ 0, 0, (float)W, (float)H, 0, 1 }; D3D12_RECT sc{ 0, 0, (LONG)W, (LONG)H }; l->RSSetViewports(1, &vp); l->RSSetScissorRects(1, &sc); };
        auto clearPool = [&](ID3D12GraphicsCommandList6* l) { g.setConstants(l, c.v); l->SetPipelineState(psoClear.Get()); l->Dispatch((uint32_t)((poolBytes / 16 + 255) / 256), 1, 1); };
        // (i) UAV InterlockedMax path (pool cleared untimed before every repetition).
        Stat su = g.time([&](ID3D12GraphicsCommandList6* l) {
            viewport(l); l->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
            g.setConstants(l, c.v, true); l->SetPipelineState(psoUav.Get()); l->DispatchMesh(gx, gy, 1); Gpu::uavBarrier(l); }, clearPool);
        record("vsm", std::string(cs.name) + ": UAV InterlockedMax into the raw pool (current S path)", su.median, "ms",
               std::to_string(fragments / 1e6) + " M texel fragments, " + std::to_string(su.median * 1e6 / fragments) + " ns/fragment, " + std::to_string(su.median * 1e6 / pages) + " ns/page");
        // (ii) hardware depth into the atlas (clear untimed), (iii) encode + pagemax pass.
        auto clearDepth = [&](ID3D12GraphicsCommandList6* l) { l->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr); };
        Stat sd = g.time([&](ID3D12GraphicsCommandList6* l) {
            viewport(l); l->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
            g.setConstants(l, c.v, true); l->SetPipelineState(psoDepth.Get()); l->DispatchMesh(gx, gy, 1); }, clearDepth);
        record("vsm", std::string(cs.name) + (g_d16 ? ": hardware depth (ROP) into a D16 page atlas, no pixel shader" : ": hardware depth (ROP) into a D32 page atlas, no pixel shader"), sd.median, "ms",
               std::to_string(sd.median * 1e6 / fragments) + " ns/fragment (c_rop), " + std::to_string(sd.median * 1e6 / pages) + " ns/page");
        Stat se = g.time([&](ID3D12GraphicsCommandList6* l) {
            Gpu::transition(l, atlas.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            g.setConstants(l, c.v); l->SetPipelineState(psoEncode.Get()); l->Dispatch(pages * 16, 1, 1);
            Gpu::transition(l, atlas.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE); Gpu::uavBarrier(l); }, flushL2);
        record("vsm", std::string(cs.name) + (g_d16 ? ": encode atlas -> pool (2 B read + 4 B write per texel) + 8^2/32^2 block min/max" : ": encode atlas -> pool (4 B read + 4 B write per texel) + 8^2/32^2 block min/max"), se.median, "ms",
               std::to_string(se.median * 1e6 / ((double)pages * 16384)) + " ns/texel, " + std::to_string((double)pages * 16384 * 8 / (se.median * 1e-3) / 1e9) + " GB/s, " + std::to_string(se.median * 1e6 / pages) + " ns/page");
        record("vsm", std::string(cs.name) + ": ROP + encode total", sd.median + se.median, "ms", std::to_string((sd.median + se.median) / su.median) + " x the UAV path");
        if (g.warp)
        {
            const auto o = g.readback(pool.Get(), 4096);
            uint32_t nz = 0; for (int i = 0; i < 1024; ++i) nz += ((const uint32_t*)o.data())[i] != 0;
            logf("  WARP check: first page row 0..7 has %u non-zero encoded depths of 1024\n", nz);
        }
    }
}

// ------------------------------------------------------------------------------------------ main
static std::string jsonEscape(const std::string& s) { std::string o; for (char c : s) { if (c == '"' || c == '\\') o += '\\'; if (c == '\n') { o += "\\n"; continue; } o += c; } return o; }
int main(int argc, char** argv)
{
    bool doCoverage = true, doBricks = true, doBands = true, doShade = true, doVsm = true, warp = false, debug = false, gbv = false;
    std::string dxcDir = DB_DXC_DIR, outDir = DB_RESULT_DIR;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto only = [&](bool& keep) { doCoverage = doBricks = doBands = doShade = doVsm = false; keep = true; };
        if (a == "--only-coverage") only(doCoverage); else if (a == "--only-bricks") only(doBricks); else if (a == "--only-bands") only(doBands); else if (a == "--only-shade") only(doShade); else if (a == "--only-vsm") only(doVsm);
        else if (a == "--warp") warp = true;
        else if (a == "--debug") debug = true;
        else if (a == "--gbv") { debug = true; gbv = true; }
        else if (a == "--no-conservative") g_conservative = false;
        else if (a == "--no-helperlane") g_noHelper = true;
        else if (a == "--d16") g_d16 = true;
        else if (a == "--width" && i + 1 < argc) g_width = (uint32_t)atoi(argv[++i]);
        else if (a == "--height" && i + 1 < argc) g_height = (uint32_t)atoi(argv[++i]);
        else if (a == "--reps" && i + 1 < argc) g.reps = atoi(argv[++i]);
        else if (a == "--quick") { g.reps = 1; g.warm = 0; }
        else if (a == "--out" && i + 1 < argc) outDir = argv[++i];
        else if (a == "--dxc" && i + 1 < argc) dxcDir = argv[++i];
        else { fprintf(stderr, "unknown argument %s\n", a.c_str()); return 2; }
    }
    if (!warp && !getenv("UNX_GPU_LOCK")) { fprintf(stderr, "hardware runs measure the GPU: run through Tools/CI/GpuLock.ps1 (or use --warp for the dry run)\n"); return 3; }
    if (warp) { if (g_width == 3840) { g_width = 256; g_height = 144; } g.reps = 1; g.warm = 0; }
    int code = 0;
    try
    {
        g.init(warp, debug, gbv);
        dxc.load(dxcDir);
        dxc.includeDir.assign(g_shaderDir.begin(), g_shaderDir.end());
        logf("DXC: %s, shaders: %s, resolution %ux%u, reps %d%s\n", dxcDir.c_str(), g_shaderDir.c_str(), g_width, g_height, g.reps, warp ? " (WARP)" : "");
        g_scratch = g.buffer(128u << 20); g_scratchUav = g.uavStructured(g_scratch.Get(), (128u << 20) / 16, 16);
        const std::string warmSrc = kWarmSource;
        g_warmPso = g.computePso(dxc.compile(warmSrc, L"WarmCS", L"cs_6_6", {}).Get());
        g_flushPso = g.computePso(dxc.compile(warmSrc, L"FlushCS", L"cs_6_6", {}).Get());
        record("env", "timestamp frequency", (double)g.timestampFrequency, "Hz");
        if (doCoverage) benchCoverage();
        if (doBricks) benchBricks();
        if (doBands) benchBands();
        if (doShade) benchShade();
        if (doVsm) benchVsm();
    }
    catch (Failure& f)
    {
        logf("\nFAILED: %s\n", f.what.c_str());
        g.drainMessages();
        HRESULT reason = g.dev ? g.dev->GetDeviceRemovedReason() : S_OK;
        if (FAILED(reason)) { logf("device removed reason 0x%08X\n", (unsigned)reason); code = 87; }  // GpuLock.ps1 convention: 87 = device removed (TDR)
        else code = 1;
    }
    SYSTEMTIME st; GetLocalTime(&st);
    char stamp[64]; snprintf(stamp, sizeof stamp, "%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    CreateDirectoryA(outDir.c_str(), nullptr);
    const std::string base = outDir + "/designbench_" + (warp ? "warp_" : "") + stamp;
    std::ofstream js(base + ".json");
    js << "{\n \"adapter\": \"" << jsonEscape(g.adapterName) << "\", \"driver\": \"" << g.driverVersion << "\", \"agility_sdk\": " << D3D12SDKVersion << ", \"resolution\": \"" << g_width << "x" << g_height << "\", \"reps\": " << g.reps << ", \"timestamp\": \"" << stamp << "\",\n \"results\": [\n";
    for (size_t i = 0; i < g_results.size(); ++i)
    {
        const Result& r = g_results[i];
        js << "  {\"section\": \"" << r.section << "\", \"name\": \"" << jsonEscape(r.name) << "\", \"value\": " << r.value << ", \"unit\": \"" << r.unit << "\", \"note\": \"" << jsonEscape(r.note) << "\"}" << (i + 1 < g_results.size() ? ",\n" : "\n");
    }
    js << " ]\n}\n";
    std::ofstream md(base + ".md");
    md << "# DesignBench " << stamp << (warp ? " (WARP dry run)" : "") << "\n\nAdapter: " << g.adapterName << ", driver " << g.driverVersion << ", Agility SDK " << D3D12SDKVersion << ", " << g_width << "x" << g_height << ", reps " << g.reps << "\n\n| section | measurement | value | unit | note |\n|---|---|---:|---|---|\n";
    for (const Result& r : g_results) md << "| " << r.section << " | " << r.name << " | " << r.value << " | " << r.unit << " | " << r.note << " |\n";
    std::ofstream lg(base + ".log"); lg << g_log;
    if (g.debugErrors) { logf("debug layer: %u error/corruption messages\n", g.debugErrors); code = code ? code : 4; }
    else if (g.debug) logf("debug layer: no error messages\n");
    logf("\nResults written to %s.{json,md,log}\n", base.c_str());
    return code;
}
