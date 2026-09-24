// unx_shaderc: build-time HLSL -> DXIL compiler (one kernel per invocation).
//
//   unx_shaderc --src <file.hlsl> --profile cs_6_6 --entry main --out <file.dxil> [--depfile <file.d>]
//               [--max-kb 200] [-I <dir>]... [-D NAME=VALUE]...
//
// Writes the stripped DXIL (what PSO creation consumes), a PDB next to it for PIX, and a Make-style depfile
// listing every included file so Ninja rebuilds exactly the kernels an include change affects. Fails when the
// DXIL exceeds --max-kb (ARCHITECTURE_KO.md 4.4). Warnings are errors.
#include <windows.h>
#include <ole2.h>  // IUnknown, IStream, BSTR for dxcapi.h (WIN32_LEAN_AND_MEAN drops them)
#include <dxcapi.h>
#include <wrl/client.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace
{
[[noreturn]] void fail(const std::string& message)
{
    std::fprintf(stderr, "unx_shaderc: error: %s\n", message.c_str());
    std::exit(1);
}

std::wstring widen(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

// Records every file the compiler opens so the depfile is exact.
class RecordingIncludeHandler final : public IDxcIncludeHandler
{
public:
    RecordingIncludeHandler(IDxcUtils* utils) : m_utils(utils) { utils->CreateDefaultIncludeHandler(&m_default); }
    HRESULT STDMETHODCALLTYPE LoadSource(LPCWSTR fileName, IDxcBlob** includeSource) override
    {
        fs::path p = fs::absolute(fs::path(fileName)).lexically_normal();
        HRESULT hr = m_default->LoadSource(fileName, includeSource);
        if (SUCCEEDED(hr)) m_files.insert(p.generic_string());
        return hr;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override
    {
        if (riid == __uuidof(IDxcIncludeHandler) || riid == __uuidof(IUnknown)) { *object = this; return S_OK; }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    const std::set<std::string>& files() const { return m_files; }

private:
    IDxcUtils* m_utils;
    ComPtr<IDxcIncludeHandler> m_default;
    std::set<std::string> m_files;
};

std::string depEscape(const std::string& path)
{
    std::string out;
    for (char c : path)
    {
        if (c == ' ') out += '\\';
        if (c == '$') out += '$';
        out += c;
    }
    return out;
}

void writeFile(const fs::path& path, const void* data, size_t size)
{
    fs::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary);
    if (!f) fail("cannot write " + path.string());
    f.write(static_cast<const char*>(data), (std::streamsize)size);
}
} // namespace

int main(int argc, char** argv)
{
    std::string src, profile, entry, out, depfile;
    size_t maxKb = 0;
    std::vector<std::string> includes, defines;
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        auto next = [&]() -> std::string { if (i + 1 >= argc) fail("missing value after " + a); return argv[++i]; };
        if (a == "--src") src = next();
        else if (a == "--profile") profile = next();
        else if (a == "--entry") entry = next();
        else if (a == "--out") out = next();
        else if (a == "--depfile") depfile = next();
        else if (a == "--max-kb") maxKb = std::stoul(next());
        else if (a == "-I") includes.push_back(next());
        else if (a == "-D") defines.push_back(next());
        else fail("unknown argument " + a);
    }
    if (src.empty() || profile.empty() || entry.empty() || out.empty()) fail("--src, --profile, --entry and --out are required");

    std::wstring dxcDir = UNX_DXC_DIR;
    SetDllDirectoryW(dxcDir.c_str());  // dxil.dll (validator/signing) is loaded from next to dxcompiler.dll
    HMODULE module = LoadLibraryW((dxcDir + L"\\dxcompiler.dll").c_str());
    if (!module) fail("cannot load dxcompiler.dll from " + narrow(dxcDir));
    auto create = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(module, "DxcCreateInstance"));
    ComPtr<IDxcUtils> utils;
    ComPtr<IDxcCompiler3> compiler;
    if (FAILED(create(CLSID_DxcUtils, IID_PPV_ARGS(&utils))) || FAILED(create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler))))
        fail("DxcCreateInstance failed");

    fs::path srcPath = fs::absolute(src).lexically_normal();
    ComPtr<IDxcBlobEncoding> source;
    if (FAILED(utils->LoadFile(srcPath.wstring().c_str(), nullptr, &source))) fail("cannot read " + srcPath.string());

    std::vector<std::wstring> args = {
        srcPath.wstring(), L"-T", widen(profile), L"-E", widen(entry),
        L"-HV", L"2021", L"-O3", L"-WX", L"-Zi", L"-Qstrip_debug", L"-Qstrip_reflect", L"-Zsb",
        L"-D", L"UNX_SHADER=1",
    };
    args.push_back(L"-I"); args.push_back(srcPath.parent_path().wstring());
    for (const auto& inc : includes) { args.push_back(L"-I"); args.push_back(widen(inc)); }
    for (const auto& def : defines) { args.push_back(L"-D"); args.push_back(widen(def)); }
    std::vector<LPCWSTR> argv16;
    for (const auto& a : args) argv16.push_back(a.c_str());

    RecordingIncludeHandler includeHandler(utils.Get());
    DxcBuffer buffer{ source->GetBufferPointer(), source->GetBufferSize(), DXC_CP_UTF8 };
    ComPtr<IDxcResult> result;
    if (FAILED(compiler->Compile(&buffer, argv16.data(), (UINT32)argv16.size(), &includeHandler, IID_PPV_ARGS(&result))))
        fail("Compile call failed");

    ComPtr<IDxcBlobUtf8> errors;
    result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
    HRESULT status = S_OK;
    result->GetStatus(&status);
    if (errors && errors->GetStringLength() > 0) std::fprintf(stderr, "%s", errors->GetStringPointer());
    if (FAILED(status)) fail("compilation failed: " + srcPath.string());

    ComPtr<IDxcBlob> dxil;
    result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&dxil), nullptr);
    if (!dxil) fail("no DXIL produced");
    const size_t bytes = dxil->GetBufferSize();
    if (maxKb && bytes > maxKb * 1024)
        fail(srcPath.string() + ": DXIL " + std::to_string(bytes / 1024) + " KB exceeds the " + std::to_string(maxKb) + " KB kernel limit (split the kernel)");

    fs::path outPath = fs::absolute(out);
    writeFile(outPath, dxil->GetBufferPointer(), bytes);

    ComPtr<IDxcBlob> pdb;
    ComPtr<IDxcBlobUtf16> pdbName;
    if (SUCCEEDED(result->GetOutput(DXC_OUT_PDB, IID_PPV_ARGS(&pdb), &pdbName)) && pdb)
    {
        fs::path pdbPath = outPath;
        pdbPath.replace_extension(".pdb");
        writeFile(pdbPath, pdb->GetBufferPointer(), pdb->GetBufferSize());
    }

    if (!depfile.empty())
    {
        std::string text = depEscape(outPath.generic_string()) + ": " + depEscape(srcPath.generic_string());
        for (const auto& f : includeHandler.files())
            if (f != srcPath.generic_string()) text += " \\\n  " + depEscape(f);
        text += "\n";
        writeFile(fs::absolute(depfile), text.data(), text.size());
    }
    return 0;
}
