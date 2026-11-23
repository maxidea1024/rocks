#include <Platform/Path.h>
#include <Platform/Win64/ComRef.h>
#include <Platform/Win64/WideString.h>
#include <ShaderToolLoader/ShaderToolLoader.h>
#include <string.h>

// dxcapi.h는 Windows에서 COM 형식(IUnknown·HRESULT·REFCLSID)을 쓰므로 Windows 헤더 뒤에
// include한다. 이 번역 단위가 엔진 경고 규칙으로 경고 0인지가 P3에서 넘어온 확인 항목이다
// (Docs/Design/ShaderSystem 8절 진척). 빌드가 WIN32_LEAN_AND_MEAN을 정의하므로 COM 기본
// 형식(IUnknown)과 IStream 헤더를 직접 더한다.

#include <Platform/Win64/WindowsHeadersBegin.h>

#include <Windows.h>
#include <Unknwn.h>
#include <objidl.h>
#include <dxc/dxcapi.h>

#include <Platform/Win64/WindowsHeadersEnd.h>

namespace rocks::shader {

namespace {

// 저장소 루트의 하위 경로다(Rocks.Tooling.Common ThirdPartyStore.DefaultRoot와 같다).
constexpr char StoreSuffix[] = "\\Rocks\\ThirdParty";

} // namespace

// Windows 저장소는 %LOCALAPPDATA%(FOLDERID_LocalAppData) 아래다. PlatformBase의 UserCache가 이
// 폴더다(이름과 달리 OS가 지우는 캐시 폴더가 아니며 roaming도 아니다). .NET의
// Environment.SpecialFolder.LocalApplicationData와 같은 위치다.
platform::FileStatus GetShaderToolStoreRoot(char* buffer, usize capacity, usize& bytes) noexcept {
    bytes = 0;
    usize base = 0;
    if (const platform::FileStatus found = platform::GetStandardDirectory(
            platform::StandardDirectory::UserCache, buffer, capacity, base);
        !found) {
        if (found.code == platform::FileCode::Overflow) {
            bytes = base + sizeof(StoreSuffix) - 1;
        }

        return found;
    }

    const usize total = base + sizeof(StoreSuffix) - 1;
    if (total + 1 > capacity) {
        bytes = total;
        return {platform::FileCode::Overflow, 0};
    }

    memcpy(buffer + base, StoreSuffix, sizeof(StoreSuffix));
    bytes = total;
    return {};
}

ShaderToolLoadStatus QueryDxcVersion(const DxcFunctions& functions, uint32& major,
                                     uint32& minor) noexcept {
    major = 0;
    minor = 0;
    ShaderToolLoadStatus status;
    status.tool = DxcToolName;
    void* address = functions.address[DxcFunctions::CreateInstance];
    if (address == nullptr) {
        status.code = ShaderToolLoadCode::WrongState;
        return status;
    }

    const auto create = reinterpret_cast<DxcCreateInstanceProc>(address);
    platform::windows::ComRef<IDxcVersionInfo> info;
    const HRESULT created = create(CLSID_DxcCompiler, IID_PPV_ARGS(info.Out()));
    if (FAILED(created) || !info) {
        status.code = ShaderToolLoadCode::CallFailed;
        status.file.nativeError = static_cast<uint32>(created);
        return status;
    }

    UINT32 versionMajor = 0;
    UINT32 versionMinor = 0;
    const HRESULT read = info->GetVersion(&versionMajor, &versionMinor);
    if (FAILED(read)) {
        status.code = ShaderToolLoadCode::CallFailed;
        status.file.nativeError = static_cast<uint32>(read);
        return status;
    }

    major = versionMajor;
    minor = versionMinor;
    return status;
}

namespace dxc_compile_detail {

// DXC가 요청한 include 이름을 UTF-8로 바꿀 버퍼 크기다.
constexpr usize MaxIncludeNameBytes = MaxShaderToolPathBytes;

ShaderToolLoadStatus DxcCallFailed(HRESULT result) noexcept {
    ShaderToolLoadStatus status;
    status.code = ShaderToolLoadCode::CallFailed;
    status.tool = DxcToolName;
    status.file.nativeError = static_cast<uint32>(result);
    return status;
}

// IDxcIncludeHandler를 DxcIncludeSource에 잇는다. 호출 스택에만 살고 DXC는 Compile 동안만 쥐므로
// 참조 수로 지우지 않는다.
class IncludeHandler final : public IDxcIncludeHandler {
public:
    IncludeHandler(IDxcUtils* utils, const DxcIncludeSource& source) noexcept
        : utils_(utils), source_(source) {
    }

    HRESULT STDMETHODCALLTYPE LoadSource(LPCWSTR fileName, IDxcBlob** include) override {
        if (include == nullptr) {
            return E_POINTER;
        }

        *include = nullptr;
        if (fileName == nullptr || source_.load == nullptr) {
            return E_INVALIDARG;
        }

        char name[MaxIncludeNameBytes];
        const platform::windows::TextConversion converted = platform::windows::ConvertWideToUtf8(
            fileName, platform::windows::WideLength(fileName, MaxIncludeNameBytes), name,
            sizeof(name), platform::windows::TextFit::Exact);
        if (!converted) {
            return E_INVALIDARG;
        }

        Span<const uint8> bytes;
        if (!source_.load(source_.context, StringView(name, converted.length), bytes)) {
            return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        }

        platform::windows::ComRef<IDxcBlobEncoding> blob;
        const HRESULT created = utils_->CreateBlob(bytes.Data(), static_cast<UINT32>(bytes.Size()),
                                                   DXC_CP_UTF8, blob.Out());
        if (FAILED(created)) {
            return created;
        }

        *include = blob.Detach();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override {
        if (object == nullptr) {
            return E_POINTER;
        }

        if (IsEqualIID(id, __uuidof(IDxcIncludeHandler)) || IsEqualIID(id, __uuidof(IUnknown))) {
            *object = static_cast<IDxcIncludeHandler*>(this);
            AddRef();
            return S_OK;
        }

        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++references_;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        return --references_;
    }

private:
    IDxcUtils* utils_;
    const DxcIncludeSource& source_;
    ULONG references_ = 1;
};

// UTF-8 인자를 UTF-16으로 바꿔 storage에 잇달아 두고 시작 주소를 pointers에 둔다.
bool ConvertArguments(Span<const StringView> arguments, StringView sourceName,
                      Array<wchar_t>& storage, Array<const wchar_t*>& pointers) noexcept {
    usize units = sourceName.ByteCount() + 1;
    for (const StringView argument : arguments) {
        units += argument.ByteCount() + 1;
    }

    // UTF-16 단위 수는 UTF-8 byte 수를 넘지 않는다. 먼저 다 잡아 두어 주소가 바뀌지 않게 한다.
    if (!storage.TryResize(units)) {
        return false;
    }

    usize used = 0;
    const auto convert = [&storage, &pointers, &used](StringView text) noexcept {
        const platform::windows::TextConversion converted = platform::windows::ConvertUtf8ToWide(
            text.Data(), text.ByteCount(), storage.Data() + used, storage.Size() - used,
            platform::windows::TextFit::Exact);
        if (!converted || !pointers.TryPushBack(storage.Data() + used)) {
            return false;
        }

        used += converted.length + 1;
        return true;
    };
    for (const StringView argument : arguments) {
        if (!convert(argument)) {
            return false;
        }
    }

    // IDxcCompiler3는 위치 인자를 주 파일 이름으로 쓴다.
    return sourceName.IsEmpty() || convert(sourceName);
}

} // namespace dxc_compile_detail

ShaderToolLoadStatus CompileWithDxc(const DxcFunctions& functions, const DxcCompileRequest& request,
                                    DxcCompileResult& result) noexcept {
    using namespace dxc_compile_detail;
    using platform::windows::ComRef;
    result.object.Clear();
    result.diagnostics.Clear();
    result.compiled = false;
    ShaderToolLoadStatus status;
    status.tool = DxcToolName;
    void* address = functions.address[DxcFunctions::CreateInstance];
    if (address == nullptr) {
        status.code = ShaderToolLoadCode::WrongState;
        return status;
    }

    const auto create = reinterpret_cast<DxcCreateInstanceProc>(address);
    ComRef<IDxcCompiler3> compiler;
    ComRef<IDxcUtils> utils;
    if (const HRESULT created = create(CLSID_DxcCompiler, IID_PPV_ARGS(compiler.Out()));
        FAILED(created)) {
        return DxcCallFailed(created);
    }

    if (const HRESULT created = create(CLSID_DxcUtils, IID_PPV_ARGS(utils.Out()));
        FAILED(created)) {
        return DxcCallFailed(created);
    }

    Array<wchar_t> storage(result.object.GetAllocator());
    Array<const wchar_t*> arguments(result.object.GetAllocator());
    if (!ConvertArguments(request.arguments, request.sourceName, storage, arguments)) {
        status.code = ShaderToolLoadCode::InvalidArgument;
        return status;
    }

    const DxcIncludeSource none;
    IncludeHandler handler(utils.Get(), request.includes != nullptr ? *request.includes : none);
    DxcBuffer source = {};
    source.Ptr = request.source.Data();
    source.Size = request.source.ByteCount();
    source.Encoding = DXC_CP_UTF8;
    ComRef<IDxcResult> compiled;
    if (const HRESULT ran =
            compiler->Compile(&source, arguments.Data(), static_cast<UINT32>(arguments.Size()),
                              &handler, IID_PPV_ARGS(compiled.Out()));
        FAILED(ran)) {
        return DxcCallFailed(ran);
    }

    ComRef<IDxcBlobUtf8> errors;
    if (SUCCEEDED(compiled->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(errors.Out()), nullptr)) &&
        errors && errors->GetStringLength() > 0 &&
        !result.diagnostics.TryAppend(
            StringView(errors->GetStringPointer(), errors->GetStringLength()))) {
        return DxcCallFailed(E_OUTOFMEMORY);
    }

    HRESULT compileStatus = E_FAIL;
    if (const HRESULT read = compiled->GetStatus(&compileStatus); FAILED(read)) {
        return DxcCallFailed(read);
    }

    if (FAILED(compileStatus)) {
        return status;
    }

    ComRef<IDxcBlob> object;
    if (const HRESULT read =
            compiled->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(object.Out()), nullptr);
        FAILED(read) || !object) {
        return DxcCallFailed(FAILED(read) ? read : E_FAIL);
    }

    const auto* bytes = static_cast<const uint8*>(object->GetBufferPointer());
    if (!result.object.TryAppend(Span<const uint8>(bytes, object->GetBufferSize()))) {
        return DxcCallFailed(E_OUTOFMEMORY);
    }

    result.compiled = true;
    return status;
}

} // namespace rocks::shader
