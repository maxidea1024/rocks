#include "D3DReflection.h"

#include <Platform/Win64/ComRef.h>
#include <ShaderCompilerCommon/ShaderReflection.h>
#include <string.h>

// D3D 리플렉션(Docs/Design/ShaderSystem 16.1절 SSD30·SSD33)이다.
// dxcapi.h·d3d12shader.h·d3d11shader.h는 COM 형식을 쓰므로 Windows 헤더 묶음 안에서
// include한다(도구 함수는 표에서 얻은 주소로 부른다).

#include <Platform/Win64/WindowsHeadersBegin.h>

#include <Windows.h>
#include <Unknwn.h>
#include <objidl.h>
#include <d3d11shader.h>
#include <d3d12shader.h>
#include <d3dcompiler.h>
#include <dxc/dxcapi.h>

#include <Platform/Win64/WindowsHeadersEnd.h>

namespace rocks::shader {

namespace d3d_reflection_detail {

using platform::windows::ComRef;

// 루트 상수 위치다(SSD32 — D3D12 register(b0, space3), D3D11 register(b13)).
constexpr UINT D3D12RootConstantsSpace = 3;
constexpr UINT D3D12RootConstantsRegister = 0;
constexpr UINT D3D11RootConstantsRegister = 13;
// PS 진단 버퍼 space다(16.1절 SSD34 — register(u0, space999)).
constexpr UINT D3D12DebugBufferSpace = 999;
// D3D11 루트 상수 cbuffer 이름 접미다(Platform.rsh 전개 Name##RootConstants).
constexpr StringView RootConstantsSuffix("RootConstants");
constexpr StringView GlobalsName("$Globals");
// SPIRV-Cross HLSL이 상수 버퍼 이름 앞에 붙이는 접두다(SPIR-V 블록 타입 이름 type.<이름>).
constexpr StringView CrossTypePrefix("type_");

// D3D12·D3D11 리플렉션 인터페이스는 같은 모양이다(이름만 다르다).
struct D3D12Traits {
    using Reflection = ID3D12ShaderReflection;
    using ShaderDesc = D3D12_SHADER_DESC;
    using BindDesc = D3D12_SHADER_INPUT_BIND_DESC;
    using BufferDesc = D3D12_SHADER_BUFFER_DESC;
    using VariableDesc = D3D12_SHADER_VARIABLE_DESC;
    using TypeDesc = D3D12_SHADER_TYPE_DESC;
    using SignatureDesc = D3D12_SIGNATURE_PARAMETER_DESC;
    using Type = ID3D12ShaderReflectionType;

    static UINT Space(const BindDesc& bind) noexcept {
        return bind.Space;
    }
};

struct D3D11Traits {
    using Reflection = ID3D11ShaderReflection;
    using ShaderDesc = D3D11_SHADER_DESC;
    using BindDesc = D3D11_SHADER_INPUT_BIND_DESC;
    using BufferDesc = D3D11_SHADER_BUFFER_DESC;
    using VariableDesc = D3D11_SHADER_VARIABLE_DESC;
    using TypeDesc = D3D11_SHADER_TYPE_DESC;
    using SignatureDesc = D3D11_SIGNATURE_PARAMETER_DESC;
    using Type = ID3D11ShaderReflectionType;

    static UINT Space(const BindDesc&) noexcept {
        return 0;
    }
};

ShaderReflectedBase BaseOf(const D3D_SHADER_VARIABLE_CLASS type,
                           const D3D_SHADER_VARIABLE_TYPE value) noexcept {
    if (type == D3D_SVC_STRUCT) {
        return ShaderReflectedBase::Struct;
    }

    switch (value) {
    case D3D_SVT_FLOAT:
        return ShaderReflectedBase::Float;
    case D3D_SVT_INT:
        return ShaderReflectedBase::Int;
    case D3D_SVT_UINT:
        return ShaderReflectedBase::Uint;
    case D3D_SVT_BOOL:
        return ShaderReflectedBase::Bool;
    case D3D_SVT_DOUBLE:
        return ShaderReflectedBase::Double;
    case D3D_SVT_MIN16FLOAT:
    case D3D_SVT_MIN10FLOAT:
    case D3D_SVT_MIN16INT:
    case D3D_SVT_MIN12INT:
    case D3D_SVT_MIN16UINT:
    case D3D_SVT_FLOAT16:
    case D3D_SVT_INT16:
    case D3D_SVT_UINT16:
        return ShaderReflectedBase::Half;
    default:
        return ShaderReflectedBase::Other;
    }
}

// 자원 종류다. 모르는 종류면 false다.
bool KindOf(D3D_SHADER_INPUT_TYPE type, ShaderResourceKind& kind) noexcept {
    switch (type) {
    case D3D_SIT_CBUFFER:
        kind = ShaderResourceKind::ConstantBuffer;
        return true;
    case D3D_SIT_TEXTURE:
        kind = ShaderResourceKind::SampledTexture;
        return true;
    case D3D_SIT_SAMPLER:
        kind = ShaderResourceKind::Sampler;
        return true;
    case D3D_SIT_STRUCTURED:
    case D3D_SIT_BYTEADDRESS:
        kind = ShaderResourceKind::StorageBufferRead;
        return true;
    case D3D_SIT_UAV_RWSTRUCTURED:
    case D3D_SIT_UAV_RWBYTEADDRESS:
        kind = ShaderResourceKind::StorageBufferReadWrite;
        return true;
    case D3D_SIT_UAV_RWTYPED:
        kind = ShaderResourceKind::StorageImage;
        return true;
    default:
        return false;
    }
}

// 자원 모양이다(값 1 v3). 모르는 차원이면 None이다(규칙이 거부한다).
ShaderResourceShape ShapeOf(D3D_SHADER_INPUT_TYPE type, D3D_SRV_DIMENSION dimension) noexcept {
    switch (type) {
    case D3D_SIT_STRUCTURED:
    case D3D_SIT_UAV_RWSTRUCTURED:
        return ShaderResourceShape::StructuredBuffer;
    case D3D_SIT_BYTEADDRESS:
    case D3D_SIT_UAV_RWBYTEADDRESS:
        return ShaderResourceShape::RawBuffer;
    case D3D_SIT_TEXTURE:
    case D3D_SIT_UAV_RWTYPED:
        break;
    default:
        return ShaderResourceShape::None;
    }

    switch (dimension) {
    case D3D_SRV_DIMENSION_BUFFER:
        return ShaderResourceShape::TypedBuffer;
    case D3D_SRV_DIMENSION_TEXTURE1D:
        return ShaderResourceShape::Texture1D;
    case D3D_SRV_DIMENSION_TEXTURE1DARRAY:
        return ShaderResourceShape::Texture1DArray;
    case D3D_SRV_DIMENSION_TEXTURE2D:
        return ShaderResourceShape::Texture2D;
    case D3D_SRV_DIMENSION_TEXTURE2DARRAY:
        return ShaderResourceShape::Texture2DArray;
    case D3D_SRV_DIMENSION_TEXTURE2DMS:
        return ShaderResourceShape::Texture2DMultisample;
    case D3D_SRV_DIMENSION_TEXTURE2DMSARRAY:
        return ShaderResourceShape::Texture2DMultisampleArray;
    case D3D_SRV_DIMENSION_TEXTURE3D:
        return ShaderResourceShape::Texture3D;
    case D3D_SRV_DIMENSION_TEXTURECUBE:
        return ShaderResourceShape::TextureCube;
    case D3D_SRV_DIMENSION_TEXTURECUBEARRAY:
        return ShaderResourceShape::TextureCubeArray;
    default:
        return ShaderResourceShape::None;
    }
}

ShaderComponentType ReturnComponent(D3D_RESOURCE_RETURN_TYPE type) noexcept {
    switch (type) {
    case D3D_RETURN_TYPE_SINT:
        return ShaderComponentType::Int;
    case D3D_RETURN_TYPE_UINT:
        return ShaderComponentType::Uint;
    default:
        return ShaderComponentType::Float;
    }
}

template <class Traits>
class Walker {
public:
    // crossNames: SPIRV-Cross HLSL 출력(D3D11 대체 경로)의 이름이다 — 상수 버퍼는 type_<이름>,
    // 멤버는 <이름>_<멤버>로 바뀌어 나오므로 원래 이름으로 되돌린다(SSD38).
    Walker(D3DCodeKind kind, bool crossNames, ShaderReflection& out, String& diagnostics) noexcept
        : kind_(kind), crossNames_(crossNames), out_(out), diagnostics_(diagnostics) {
    }

    ShaderToolOutcome Run(typename Traits::Reflection& reflection) noexcept {
        typename Traits::ShaderDesc desc = {};
        if (FAILED(reflection.GetDesc(&desc))) {
            return Tool("shader reflection has no description");
        }

        for (UINT index = 0; index < desc.BoundResources; ++index) {
            typename Traits::BindDesc bind = {};
            if (FAILED(reflection.GetResourceBindingDesc(index, &bind))) {
                return Tool("shader reflection binding query failed");
            }

            const ShaderToolOutcome added = AddResource(reflection, bind);
            if (added != ShaderToolOutcome::Success) {
                return added;
            }
        }

        for (UINT index = 0; index < desc.InputParameters; ++index) {
            typename Traits::SignatureDesc parameter = {};
            if (FAILED(reflection.GetInputParameterDesc(index, &parameter)) ||
                !out_.inputs.TryPushBack(Signature(parameter))) {
                return Tool("shader reflection input signature query failed");
            }
        }

        for (UINT index = 0; index < desc.OutputParameters; ++index) {
            typename Traits::SignatureDesc parameter = {};
            if (FAILED(reflection.GetOutputParameterDesc(index, &parameter)) ||
                !out_.outputs.TryPushBack(Signature(parameter))) {
                return Tool("shader reflection output signature query failed");
            }
        }

        return ShaderToolOutcome::Success;
    }

private:
    ShaderToolOutcome Tool(StringView message) noexcept {
        (void)diagnostics_.TryAppend(message);
        return ShaderToolOutcome::ToolError;
    }

    ShaderToolOutcome Compile(StringView name, StringView message) noexcept {
        (void)(diagnostics_.TryAppend("resource '") && diagnostics_.TryAppend(name) &&
               diagnostics_.TryAppend("': ") && diagnostics_.TryAppend(message));
        return ShaderToolOutcome::CompileError;
    }

    static StringView Name(const char* name) noexcept {
        return name != nullptr ? StringView(name) : StringView();
    }

    static StringView StripPrefix(StringView name, StringView prefix) noexcept {
        return name.ByteCount() > prefix.ByteCount() &&
                       name.StartsWith(prefix, StringComparison::CaseSensitive)
                   ? StringView(name.Data() + prefix.ByteCount(),
                                name.ByteCount() - prefix.ByteCount())
                   : name;
    }

    // SPIRV-Cross 이름이면 <블록>_<멤버>에서 <멤버>를 꺼낸다.
    StringView MemberName(StringView block, StringView name) const noexcept {
        if (!crossNames_ || name.ByteCount() <= block.ByteCount() + 1 ||
            !name.StartsWith(block, StringComparison::CaseSensitive) ||
            name.Data()[block.ByteCount()] != '_') {
            return name;
        }

        return StringView(name.Data() + block.ByteCount() + 1,
                          name.ByteCount() - block.ByteCount() - 1);
    }

    static ShaderReflectedSignature
    Signature(const typename Traits::SignatureDesc& parameter) noexcept {
        ShaderReflectedSignature element;
        element.semantic =
            parameter.SemanticName != nullptr ? StringView(parameter.SemanticName) : StringView();
        element.index = parameter.SemanticIndex;
        element.component = parameter.ComponentType == D3D_REGISTER_COMPONENT_SINT32
                                ? ShaderComponentType::Int
                                : (parameter.ComponentType == D3D_REGISTER_COMPONENT_UINT32
                                       ? ShaderComponentType::Uint
                                       : ShaderComponentType::Float);
        return element;
    }

    // 멤버 하나와 (구조체면) 그 자식을 더한다.
    bool AddMember(typename Traits::Type* type, StringView name, uint32 offset, uint32 bytes,
                   uint32 parent) noexcept {
        typename Traits::TypeDesc desc = {};
        if (type == nullptr || FAILED(type->GetDesc(&desc))) {
            return false;
        }

        ShaderReflectedMember member;
        member.name = name;
        member.base = BaseOf(desc.Class, desc.Type);
        member.rows = desc.Rows;
        member.columns = desc.Columns;
        member.elements = desc.Elements;
        member.offset = offset;
        member.bytes = bytes;
        member.parent = parent;
        const uint32 index = static_cast<uint32>(out_.members.Size());
        if (!out_.members.TryPushBack(member)) {
            return false;
        }

        if (desc.Class != D3D_SVC_STRUCT) {
            return true;
        }

        for (UINT child = 0; child < desc.Members; ++child) {
            typename Traits::Type* childType = type->GetMemberTypeByIndex(child);
            typename Traits::TypeDesc childDesc = {};
            if (childType == nullptr || FAILED(childType->GetDesc(&childDesc)) ||
                !AddMember(childType, Name(type->GetMemberTypeName(child)), childDesc.Offset, 0,
                           index)) {
                return false;
            }
        }

        return true;
    }

    ShaderToolOutcome AddResource(typename Traits::Reflection& reflection,
                                  const typename Traits::BindDesc& bind) noexcept {
        StringView name = Name(bind.Name);
        ShaderReflectedResource resource;
        if (!KindOf(bind.Type, resource.kind)) {
            return Compile(name, "resource type is not supported (append, consume, counter and "
                                 "texture buffers are rejected)");
        }

        if (crossNames_ && resource.kind == ShaderResourceKind::ConstantBuffer) {
            name = StripPrefix(name, CrossTypePrefix);
        }

        // 멤버 이름 앞의 <블록 이름>_(SPIRV-Cross)을 떼는 기준이다(루트 상수 접미를 떼기 전 이름).
        const StringView block = name;

        resource.name = name;
        resource.slot = bind.BindPoint;
        resource.count = bind.BindCount;
        resource.shape = ShapeOf(bind.Type, bind.Dimension);
        const bool typed = resource.shape != ShaderResourceShape::None &&
                           resource.shape != ShaderResourceShape::StructuredBuffer &&
                           resource.shape != ShaderResourceShape::RawBuffer;
        resource.component = typed ? ReturnComponent(bind.ReturnType) : ShaderComponentType::None;
        // 구조화 버퍼의 NumSamples는 원소 stride다(D3D12_SHADER_INPUT_BIND_DESC).
        resource.structureStride =
            resource.shape == ShaderResourceShape::StructuredBuffer ? bind.NumSamples : 0u;
        const bool constantBuffer = resource.kind == ShaderResourceKind::ConstantBuffer;
        const UINT space = Traits::Space(bind);
        if (kind_ == D3DCodeKind::Dxil) {
            resource.rootConstants = constantBuffer && space == D3D12RootConstantsSpace &&
                                     bind.BindPoint == D3D12RootConstantsRegister;
        } else {
            resource.rootConstants = constantBuffer && bind.BindPoint == D3D11RootConstantsRegister;
            if (resource.rootConstants &&
                name.EndsWith(RootConstantsSuffix, StringComparison::CaseSensitive)) {
                resource.name =
                    StringView(name.Data(), name.ByteCount() - RootConstantsSuffix.ByteCount());
            }
        }

        resource.debugBuffer = kind_ == D3DCodeKind::Dxil && space == D3D12DebugBufferSpace &&
                               resource.kind == ShaderResourceKind::StorageBufferReadWrite;
        if (space != 0 && !resource.rootConstants && !resource.debugBuffer) {
            return Compile(name, "register spaces other than 0 are reserved for the engine");
        }

        if (constantBuffer && name == GlobalsName) {
            out_.hasGlobals = true;
            return ShaderToolOutcome::Success;
        }

        if (constantBuffer) {
            auto* buffer = reflection.GetConstantBufferByName(bind.Name);
            typename Traits::BufferDesc bufferDesc = {};
            if (buffer == nullptr || FAILED(buffer->GetDesc(&bufferDesc))) {
                return Tool("constant buffer reflection failed");
            }

            resource.constantBufferBytes = bufferDesc.Size;
            resource.firstMember = static_cast<uint32>(out_.members.Size());
            for (UINT variable = 0; variable < bufferDesc.Variables; ++variable) {
                auto* value = buffer->GetVariableByIndex(variable);
                typename Traits::VariableDesc variableDesc = {};
                if (value == nullptr || FAILED(value->GetDesc(&variableDesc)) ||
                    !AddMember(value->GetType(), MemberName(block, Name(variableDesc.Name)),
                               variableDesc.StartOffset, variableDesc.Size,
                               NoShaderReflectedParent)) {
                    return Tool("constant buffer member reflection failed");
                }
            }

            resource.memberCount = static_cast<uint32>(out_.members.Size()) - resource.firstMember;
        }

        if (!out_.resources.TryPushBack(resource)) {
            return Tool("out of memory");
        }

        return ShaderToolOutcome::Success;
    }

    D3DCodeKind kind_;
    bool crossNames_ = false;
    ShaderReflection& out_;
    String& diagnostics_;
};

// 공용 규칙을 거쳐 값 1 byte를 만든다(리플렉션 객체가 살아 있는 동안 — 이름이 그것을 가리킨다).
ShaderToolOutcome Normalize(const ShaderFormatDefinition& format,
                            Span<const ShaderParameterLayout> layouts, ShaderReflection& reflection,
                            Array<uint8>& out, String& diagnostics) noexcept {
    ShaderBindingLayout layout(out.GetAllocator());
    if (!NormalizeShaderReflection(format, reflection, layouts, layout, diagnostics)) {
        return ShaderToolOutcome::CompileError;
    }

    if (!EncodeShaderBindingLayout(layout, out)) {
        (void)diagnostics.TryAppend("binding layout encoding failed");
        return ShaderToolOutcome::ToolError;
    }

    return ShaderToolOutcome::Success;
}

} // namespace d3d_reflection_detail

ShaderToolOutcome BuildD3DBindingLayout(D3DCodeKind kind, const ShaderToolSet& tools,
                                        const ShaderFormatDefinition& format, uint32 stage,
                                        uint32 requiredFeatures,
                                        Span<const ShaderParameterLayout> layouts,
                                        Span<const ShaderSamplerUse> uses, bool hasUses,
                                        Span<const uint8> code, Array<uint8>& out,
                                        String& diagnostics) noexcept {
    using namespace d3d_reflection_detail;
    out.Clear();
    ShaderReflection reflection(out.GetAllocator());
    reflection.stage = stage;
    reflection.requiredFeatures = requiredFeatures;
    reflection.signatureOrdered = true;
    if (hasUses) {
        reflection.hasSamplerUses = true;
        if (!reflection.samplerUses.TryAppend(uses)) {
            (void)diagnostics.TryAppend("out of memory");
            return ShaderToolOutcome::ToolError;
        }
    }
    if (kind == D3DCodeKind::Dxil) {
        const auto create = reinterpret_cast<DxcCreateInstanceProc>(
            tools.Dxc().address[DxcFunctions::CreateInstance]);
        ComRef<IDxcUtils> utils;
        ComRef<ID3D12ShaderReflection> shader;
        if (create == nullptr || FAILED(create(CLSID_DxcUtils, IID_PPV_ARGS(utils.Out())))) {
            (void)diagnostics.TryAppend("DXC utilities are not available");
            return ShaderToolOutcome::ToolError;
        }

        const DxcBuffer buffer{code.Data(), code.Size(), DXC_CP_ACP};
        if (FAILED(utils->CreateReflection(&buffer, IID_PPV_ARGS(shader.Out())))) {
            (void)diagnostics.TryAppend("DXC could not reflect the DXIL container");
            return ShaderToolOutcome::ToolError;
        }

        Walker<D3D12Traits> walker(kind, false, reflection, diagnostics);
        const ShaderToolOutcome walked = walker.Run(*shader.Get());
        return walked == ShaderToolOutcome::Success
                   ? Normalize(format, layouts, reflection, out, diagnostics)
                   : walked;
    }

    const auto reflect =
        reinterpret_cast<decltype(&D3DReflect)>(tools.Fxc().address[FxcFunctions::Reflect]);
    ComRef<ID3D11ShaderReflection> shader;
    if (reflect == nullptr ||
        FAILED(reflect(code.Data(), code.Size(), IID_PPV_ARGS(shader.Out())))) {
        (void)diagnostics.TryAppend("FXC could not reflect the DXBC object");
        return ShaderToolOutcome::ToolError;
    }

    Walker<D3D11Traits> walker(kind, hasUses, reflection, diagnostics);
    const ShaderToolOutcome walked = walker.Run(*shader.Get());
    return walked == ShaderToolOutcome::Success
               ? Normalize(format, layouts, reflection, out, diagnostics)
               : walked;
}

} // namespace rocks::shader
