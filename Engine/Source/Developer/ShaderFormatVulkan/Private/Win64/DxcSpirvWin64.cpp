#include "DxcSpirv.h"

#include <Platform/Memory.h>

// dxcapi.h는 Windows에서 COM 형식(IUnknown·HRESULT)을 쓰므로 Windows 헤더 뒤에 include한다
// (ShaderToolLoader Win64 구현과 같은 순서). 빌드가 WIN32_LEAN_AND_MEAN을 정의하므로 COM 기본
// 형식과 IStream 헤더를 직접 더한다.
#include <Windows.h>
#include <Unknwn.h>
#include <objidl.h>
#include <dxc/dxcapi.h>

namespace rocks::shader {

namespace {

// DXC 인자 수의 상한이다(아래 고정 인자 + 여유).
constexpr usize MaxArguments = 16;

// COM 참조를 소멸 때 놓는다.
template <class T>
class ComHolder {
public:
    ComHolder() noexcept = default;
    ComHolder(const ComHolder&) = delete;
    ComHolder& operator=(const ComHolder&) = delete;

    ~ComHolder() {
        if (pointer_ != nullptr) {
            pointer_->Release();
        }
    }

    T* Get() const noexcept {
        return pointer_;
    }

    T* operator->() const noexcept {
        return pointer_;
    }

    void** Out() noexcept {
        return reinterpret_cast<void**>(&pointer_);
    }

private:
    T* pointer_ = nullptr;
};

void Append(String& text, StringView part) noexcept {
    auto appended = text.TryAppend(part);
    (void)appended;
}

} // namespace

Result<void> CompileHlslToSpirv(const DxcFunctions& dxc, graphics::ShaderStage stage,
                                StringView hlsl, Array<uint32>& words,
                                String& diagnostics) noexcept {
    const auto create =
        reinterpret_cast<DxcCreateInstanceProc>(dxc.address[DxcFunctions::CreateInstance]);
    ComHolder<IDxcCompiler3> compiler;
    if (create == nullptr ||
        FAILED(create(CLSID_DxcCompiler, __uuidof(IDxcCompiler3), compiler.Out()))) {
        Append(diagnostics, StringView("DxcCreateInstance(IDxcCompiler3) failed"));
        return Result<void>::Failure(ErrorCode::DependencyFailed);
    }

    const bool vertex = stage == graphics::ShaderStage::Vertex;
    const wchar_t* arguments[MaxArguments] = {};
    usize count = 0;
    arguments[count++] = L"-E";
    arguments[count++] = vertex ? L"VSMain" : L"PSMain";
    arguments[count++] = L"-T";
    arguments[count++] = vertex ? L"vs_6_0" : L"ps_6_0";
    // 행렬 행 우선(ShaderSystem 7.2절 6항), Vulkan 1.1 SPIR-V와 D3D 규약 위치 w(8항).
    arguments[count++] = L"-Zpr";
    arguments[count++] = L"-spirv";
    arguments[count++] = L"-fspv-target-env=vulkan1.1";
    arguments[count++] = L"-fvk-use-dx-position-w";
    // 정점 입력 시맨틱(ATTRIBUTEn)을 location 재작성에 쓴다. 재작성이 그 장식을 지운다.
    arguments[count++] = L"-fspv-reflect";

    DxcBuffer source = {};
    source.Ptr = hlsl.Data();
    source.Size = hlsl.ByteCount();
    source.Encoding = DXC_CP_UTF8;
    ComHolder<IDxcResult> result;
    if (FAILED(compiler->Compile(&source, arguments, static_cast<UINT32>(count), nullptr,
                                 __uuidof(IDxcResult), result.Out()))) {
        Append(diagnostics, StringView("IDxcCompiler3::Compile failed"));
        return Result<void>::Failure(ErrorCode::DependencyFailed);
    }

    ComHolder<IDxcBlobUtf8> errors;
    if (SUCCEEDED(
            result->GetOutput(DXC_OUT_ERRORS, __uuidof(IDxcBlobUtf8), errors.Out(), nullptr)) &&
        errors.Get() != nullptr && errors->GetStringLength() > 0) {
        Append(diagnostics, StringView(errors->GetStringPointer(), errors->GetStringLength()));
    }

    HRESULT status = E_FAIL;
    if (FAILED(result->GetStatus(&status)) || FAILED(status)) {
        return Result<void>::Failure(ErrorCode::DependencyFailed);
    }

    ComHolder<IDxcBlob> object;
    if (FAILED(result->GetOutput(DXC_OUT_OBJECT, __uuidof(IDxcBlob), object.Out(), nullptr)) ||
        object.Get() == nullptr || object->GetBufferSize() % sizeof(uint32) != 0) {
        Append(diagnostics, StringView("DXC produced no SPIR-V object"));
        return Result<void>::Failure(ErrorCode::DependencyFailed);
    }

    const usize wordCount = object->GetBufferSize() / sizeof(uint32);
    auto resized = words.TryResize(wordCount);
    if (!resized) {
        return resized;
    }

    platform::MemoryCopy(words.Data(), object->GetBufferPointer(), object->GetBufferSize());
    return Result<void>::Success();
}

} // namespace rocks::shader
