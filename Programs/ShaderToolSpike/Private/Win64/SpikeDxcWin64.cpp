#include "SpikeTools.h"

#include <string.h>

// dxcapi.h는 Windows에서 COM 형식(IUnknown·HRESULT)을 쓰므로 Windows 헤더 뒤에 include한다. 빌드가
// WIN32_LEAN_AND_MEAN을 정의하므로 IUnknown·IStream 헤더를 직접 더한다(ShaderToolLoaderWin64.cpp와
// 같다). d3d12shader.h·d3dcompiler.h는 Windows SDK 헤더이며 형식·상수만 쓴다(링크하지 않고 도구
// 표가 연 함수를 부른다).
#include <Platform/Win64/ComRef.h>
#include <Platform/Win64/WideString.h>

#include <Platform/Win64/WindowsHeadersBegin.h>

#include <Windows.h>
#include <Unknwn.h>
#include <objidl.h>
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <d3d12shader.h>

#include <Platform/Win64/WindowsHeadersEnd.h>

#include <dxc/dxcapi.h>

namespace rocks::spike {

using platform::windows::ComRef;

namespace {

// DXC 인자 개수 상한이다(진입점·프로필·SPIR-V 옵션·strip 옵션).
constexpr usize MaxDxcArguments = 12;
// 진입점·프로필 이름의 UTF-16 버퍼 크기다(NUL 포함).
constexpr usize MaxWideName = 64;

// 진입점·프로필 이름(UTF-8)을 UTF-16으로 바꾼다. 넘치거나 잘못된 UTF-8이면 거짓이다.
bool Widen(const char* text, wchar_t (&out)[MaxWideName]) noexcept {
    return static_cast<bool>(platform::windows::ConvertUtf8ToWide(
        text, strlen(text), out, MaxWideName, platform::windows::TextFit::Exact));
}

bool CopyBlob(IDxcBlob* blob, ByteArray& out) noexcept {
    if (blob == nullptr) {
        return false;
    }

    if (!out.TryResize(blob->GetBufferSize())) {
        return false;
    }

    memcpy(out.Data(), blob->GetBufferPointer(), out.Size());
    return true;
}

const char* InputTypeName(D3D_SHADER_INPUT_TYPE type) noexcept {
    switch (type) {
    case D3D_SIT_CBUFFER:
        return "cbuffer";
    case D3D_SIT_TBUFFER:
        return "tbuffer";
    case D3D_SIT_TEXTURE:
        return "texture";
    case D3D_SIT_SAMPLER:
        return "sampler";
    case D3D_SIT_UAV_RWTYPED:
        return "uav-typed";
    case D3D_SIT_STRUCTURED:
        return "structured";
    case D3D_SIT_UAV_RWSTRUCTURED:
        return "uav-structured";
    case D3D_SIT_BYTEADDRESS:
        return "byte-address";
    case D3D_SIT_UAV_RWBYTEADDRESS:
        return "uav-byte-address";
    default:
        return "other";
    }
}

void WriteSignature(ID3D12ShaderReflection* reflection, UINT count, bool input,
                    Report& report) noexcept {
    report.BeginArray(input ? StringView("inputs") : StringView("outputs"));
    for (UINT index = 0; index < count; ++index) {
        D3D12_SIGNATURE_PARAMETER_DESC parameter = {};
        const HRESULT read = input ? reflection->GetInputParameterDesc(index, &parameter)
                                   : reflection->GetOutputParameterDesc(index, &parameter);
        if (FAILED(read)) {
            continue;
        }

        report.BeginObject();
        report.String("semantic", StringView(parameter.SemanticName));
        report.UInt("semanticIndex", parameter.SemanticIndex);
        report.UInt("register", parameter.Register);
        report.UInt("systemValue", static_cast<uint64>(parameter.SystemValueType));
        report.UInt("componentType", static_cast<uint64>(parameter.ComponentType));
        report.UInt("mask", parameter.Mask);
        report.EndObject();
    }

    report.EndArray();
}

// DXC 리플렉션 부분을 IDxcUtils::CreateReflection으로 연다.
bool OpenDxilReflection(const shader::DxcFunctions& dxc, Span<const uint8> reflectionPart,
                        ComRef<ID3D12ShaderReflection>& reflection) noexcept {
    const auto create =
        reinterpret_cast<DxcCreateInstanceProc>(dxc.address[shader::DxcFunctions::CreateInstance]);
    ComRef<IDxcUtils> utils;
    if (create == nullptr || FAILED(create(CLSID_DxcUtils, IID_PPV_ARGS(utils.Out())))) {
        return false;
    }

    DxcBuffer buffer = {};
    buffer.Ptr = reflectionPart.Data();
    buffer.Size = reflectionPart.Size();
    return SUCCEEDED(utils->CreateReflection(&buffer, IID_PPV_ARGS(reflection.Out())));
}

} // namespace

DxcOutput::DxcOutput() noexcept
    : code(SpikeAllocator()), reflection(SpikeAllocator()), diagnostics(SpikeAllocator()) {
}

bool CompileWithDxc(const shader::DxcFunctions& dxc, const ShaderInput& input, DxcTarget target,
                    const char* shaderModel, DxcOutput& output) noexcept {
    const auto create =
        reinterpret_cast<DxcCreateInstanceProc>(dxc.address[shader::DxcFunctions::CreateInstance]);
    ComRef<IDxcCompiler3> compiler;
    if (create == nullptr || FAILED(create(CLSID_DxcCompiler, IID_PPV_ARGS(compiler.Out())))) {
        output.diagnostics.Append(StringView("DxcCreateInstance(IDxcCompiler3) failed"));
        return false;
    }

    char profileText[MaxWideName] = {};
    StringBuilder<MaxWideName> profileBuilder(SpikeAllocator());
    profileBuilder.Append(StringView(StageProfilePrefix(input.stage)))
        .Append(StringView("_"))
        .Append(StringView(shaderModel));
    if (profileBuilder.HasFailed() || profileBuilder.ByteCount() + 1 > sizeof(profileText)) {
        return false;
    }

    memcpy(profileText, profileBuilder.CString(), profileBuilder.ByteCount() + 1);
    wchar_t entry[MaxWideName];
    wchar_t profile[MaxWideName];
    if (!Widen(input.entry, entry) || !Widen(profileText, profile)) {
        output.diagnostics.Append(StringView("entry or profile is not a short valid UTF-8 name"));
        return false;
    }

    const wchar_t* arguments[MaxDxcArguments] = {};
    usize count = 0;
    arguments[count++] = L"-E";
    arguments[count++] = entry;
    arguments[count++] = L"-T";
    arguments[count++] = profile;
    // 행렬은 행 우선으로 담는다(ShaderSystem 7.2절 6항, UE D3DShaderCompilerDXC.cpp:189–190).
    arguments[count++] = L"-Zpr";
    if (target == DxcTarget::Spirv) {
        arguments[count++] = L"-spirv";
        arguments[count++] = L"-fspv-target-env=vulkan1.1";
        arguments[count++] = L"-fvk-use-dx-position-w";
    } else {
        // 리플렉션은 DXC_OUT_REFLECTION으로 따로 받고 코드에서는 뺀다(UE D3D12 경로와 같다).
        arguments[count++] = L"-Qstrip_reflect";
    }

    DxcBuffer buffer = {};
    buffer.Ptr = input.source;
    buffer.Size = strlen(input.source);
    buffer.Encoding = DXC_CP_UTF8;
    ComRef<IDxcResult> result;
    if (FAILED(compiler->Compile(&buffer, arguments, static_cast<UINT32>(count), nullptr,
                                 IID_PPV_ARGS(result.Out())))) {
        output.diagnostics.Append(StringView("IDxcCompiler3::Compile failed"));
        return false;
    }

    ComRef<IDxcBlobUtf8> errors;
    if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(errors.Out()), nullptr)) &&
        errors.Get() != nullptr && errors->GetStringLength() > 0) {
        output.diagnostics.Append(
            StringView(errors->GetStringPointer(), errors->GetStringLength()));
    }

    HRESULT status = E_FAIL;
    if (FAILED(result->GetStatus(&status)) || FAILED(status)) {
        return false;
    }

    ComRef<IDxcBlob> object;
    if (FAILED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(object.Out()), nullptr)) ||
        !CopyBlob(object.Get(), output.code)) {
        output.diagnostics.Append(StringView("no DXC object output"));
        return false;
    }

    if (target == DxcTarget::Dxil) {
        ComRef<IDxcBlob> reflection;
        if (FAILED(
                result->GetOutput(DXC_OUT_REFLECTION, IID_PPV_ARGS(reflection.Out()), nullptr)) ||
            !CopyBlob(reflection.Get(), output.reflection)) {
            output.diagnostics.Append(StringView("no DXC reflection output"));
            return false;
        }
    }

    return true;
}

bool DumpDxilReflection(const shader::DxcFunctions& dxc, Span<const uint8> reflectionPart,
                        Report& report) noexcept {
    ComRef<ID3D12ShaderReflection> reflection;
    if (!OpenDxilReflection(dxc, reflectionPart, reflection)) {
        return false;
    }

    D3D12_SHADER_DESC desc = {};
    if (FAILED(reflection->GetDesc(&desc))) {
        return false;
    }

    report.UInt("version", desc.Version);
    report.BeginArray("bindings");
    for (UINT index = 0; index < desc.BoundResources; ++index) {
        D3D12_SHADER_INPUT_BIND_DESC bind = {};
        if (FAILED(reflection->GetResourceBindingDesc(index, &bind))) {
            return false;
        }

        report.BeginObject();
        report.String("name", StringView(bind.Name));
        report.String("type", StringView(InputTypeName(bind.Type)));
        report.UInt("bindPoint", bind.BindPoint);
        report.UInt("bindCount", bind.BindCount);
        report.UInt("space", bind.Space);
        report.UInt("dimension", static_cast<uint64>(bind.Dimension));
        report.EndObject();
    }

    report.EndArray();
    report.BeginArray("constantBuffers");
    for (UINT index = 0; index < desc.ConstantBuffers; ++index) {
        ID3D12ShaderReflectionConstantBuffer* buffer = reflection->GetConstantBufferByIndex(index);
        D3D12_SHADER_BUFFER_DESC bufferDesc = {};
        if (buffer == nullptr || FAILED(buffer->GetDesc(&bufferDesc))) {
            return false;
        }

        report.BeginObject();
        report.String("name", StringView(bufferDesc.Name));
        report.UInt("size", bufferDesc.Size);
        report.BeginArray("members");
        for (UINT member = 0; member < bufferDesc.Variables; ++member) {
            D3D12_SHADER_VARIABLE_DESC variable = {};
            ID3D12ShaderReflectionVariable* reflected = buffer->GetVariableByIndex(member);
            if (reflected == nullptr || FAILED(reflected->GetDesc(&variable))) {
                return false;
            }

            report.BeginObject();
            report.String("name", StringView(variable.Name));
            report.UInt("offset", variable.StartOffset);
            report.UInt("size", variable.Size);
            report.EndObject();
        }

        report.EndArray();
        report.EndObject();
    }

    report.EndArray();
    WriteSignature(reflection.Get(), desc.InputParameters, true, report);
    WriteSignature(reflection.Get(), desc.OutputParameters, false, report);
    return true;
}

bool ReadDxilRequires(const shader::DxcFunctions& dxc, Span<const uint8> reflectionPart,
                      ShaderRequires& result) noexcept {
    result = ShaderRequires{};
    ComRef<ID3D12ShaderReflection> reflection;
    if (!OpenDxilReflection(dxc, reflectionPart, reflection)) {
        return false;
    }

    result.flags = reflection->GetRequiresFlags();
    result.viewportAndLayerFromAnyShader =
        (result.flags &
         D3D_SHADER_REQUIRES_VIEWPORT_AND_RT_ARRAY_INDEX_FROM_ANY_SHADER_FEEDING_RASTERIZER) != 0;
    return true;
}

bool ReadDxbcRequires(const shader::FxcFunctions& fxc, Span<const uint8> code,
                      ShaderRequires& result) noexcept {
    result = ShaderRequires{};
    const auto reflect =
        reinterpret_cast<decltype(&D3DReflect)>(fxc.address[shader::FxcFunctions::Reflect]);
    ComRef<ID3D11ShaderReflection> reflection;
    if (reflect == nullptr ||
        FAILED(reflect(code.Data(), code.Size(), IID_PPV_ARGS(reflection.Out())))) {
        return false;
    }

    result.flags = reflection->GetRequiresFlags();
    result.viewportAndLayerFromAnyShader =
        (result.flags &
         D3D_SHADER_REQUIRES_VIEWPORT_AND_RT_ARRAY_INDEX_FROM_ANY_SHADER_FEEDING_RASTERIZER) != 0;
    return true;
}

bool CompileWithFxc(const shader::FxcFunctions& fxc, StringView source, const char* name,
                    const char* entry, const char* profile, ByteArray& code,
                    StringBuilderBase& diagnostics) noexcept {
    const auto compile = reinterpret_cast<pD3DCompile>(fxc.address[shader::FxcFunctions::Compile]);
    if (compile == nullptr) {
        diagnostics.Append(StringView("FXC is not open"));
        return false;
    }

    ComRef<ID3DBlob> object;
    ComRef<ID3DBlob> errors;
    const HRESULT compiled =
        compile(source.Data(), source.ByteCount(), name, nullptr, nullptr, entry, profile,
                D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_PACK_MATRIX_ROW_MAJOR, 0, object.Out(),
                errors.Out());
    if (errors) {
        // FXC 문구 blob은 끝 NUL을 크기에 넣는다.
        const char* text = static_cast<const char*>(errors->GetBufferPointer());
        usize bytes = errors->GetBufferSize();
        while (bytes > 0 && text[bytes - 1] == '\0') {
            --bytes;
        }

        diagnostics.Append(StringView(text, bytes));
    }

    bool ok = SUCCEEDED(compiled) && object;
    if (ok) {
        ok = static_cast<bool>(code.TryResize(object->GetBufferSize()));
        if (ok) {
            memcpy(code.Data(), object->GetBufferPointer(), code.Size());
        }
    }

    return ok;
}

} // namespace rocks::spike
