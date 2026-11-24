#include "SpirvReflection.h"

#include <ShaderCompilerCommon/ShaderReflection.h>
#include <spirv_reflect.h>

// SPIR-V 리플렉션(Docs/Design/ShaderSystem 16.1절 SSD30·SSD33)이다.
namespace rocks::shader {

namespace spirv_reflection_detail {

// SPIRV-Reflect 모듈 수명이다.
class Module {
public:
    Module() noexcept = default;
    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;

    ~Module() {
        if (open_) {
            spvReflectDestroyShaderModule(&module_);
        }
    }

    bool Open(Span<const uint32> code) noexcept {
        open_ = spvReflectCreateShaderModule(code.Size() * sizeof(uint32), code.Data(), &module_) ==
                SPV_REFLECT_RESULT_SUCCESS;
        return open_;
    }

    const SpvReflectShaderModule* Get() const noexcept {
        return &module_;
    }

private:
    SpvReflectShaderModule module_ = {};
    bool open_ = false;
};

// 열거 결과 칸 상한이다(셰이더 하나의 자원·입출력은 이보다 적다 — 넘으면 리플렉션 실패로 본다).
constexpr uint32 MaxReflectedItems = 256;

constexpr StringView GlobalsName("$Globals");

// 시스템 값 변수의 HLSL 시맨틱 이름이다(SPIR-V BuiltIn → 7.2절 7항 철자). 모르면 빈 값이다.
StringView BuiltInSemantic(int builtIn) noexcept {
    switch (builtIn) {
    case SpvBuiltInPosition:
    case SpvBuiltInFragCoord:
        return "SV_Position";
    case SpvBuiltInVertexIndex:
    case SpvBuiltInVertexId:
        return "SV_VertexID";
    case SpvBuiltInInstanceIndex:
    case SpvBuiltInInstanceId:
        return "SV_InstanceID";
    case SpvBuiltInFrontFacing:
        return "SV_IsFrontFace";
    case SpvBuiltInFragDepth:
        return "SV_Depth";
    case SpvBuiltInGlobalInvocationId:
        return "SV_DispatchThreadID";
    case SpvBuiltInWorkgroupId:
        return "SV_GroupID";
    case SpvBuiltInLocalInvocationId:
        return "SV_GroupThreadID";
    case SpvBuiltInLocalInvocationIndex:
        return "SV_GroupIndex";
    case SpvBuiltInLayer:
        return "SV_RenderTargetArrayIndex";
    case SpvBuiltInViewportIndex:
        return "SV_ViewportArrayIndex";
    default:
        return StringView();
    }
}

class Converter {
public:
    Converter(ShaderReflection& out, String& diagnostics) noexcept
        : out_(out), diagnostics_(diagnostics) {
    }

    ShaderToolOutcome Run(const SpvReflectShaderModule& raw,
                          const SpvReflectShaderModule& rewritten) noexcept {
        SpvReflectDescriptorBinding* rawBindings[MaxReflectedItems] = {};
        uint32 rawCount = MaxReflectedItems;
        SpvReflectDescriptorBinding* bindings[MaxReflectedItems] = {};
        uint32 count = MaxReflectedItems;
        if (!Enumerate(spvReflectEnumerateDescriptorBindings, raw, rawCount, rawBindings) ||
            !Enumerate(spvReflectEnumerateDescriptorBindings, rewritten, count, bindings)) {
            return Tool("SPIR-V descriptor reflection failed");
        }

        for (uint32 index = 0; index < count; ++index) {
            const SpvReflectDescriptorBinding* original = nullptr;
            for (uint32 candidate = 0; candidate < rawCount; ++candidate) {
                original = rawBindings[candidate]->spirv_id == bindings[index]->spirv_id
                               ? rawBindings[candidate]
                               : original;
            }

            if (original == nullptr) {
                return Tool("rewritten SPIR-V binding has no source binding");
            }

            const ShaderToolOutcome added = AddBinding(*bindings[index], *original);
            if (added != ShaderToolOutcome::Success) {
                return added;
            }
        }

        SpvReflectBlockVariable* blocks[MaxReflectedItems] = {};
        count = MaxReflectedItems;
        if (!Enumerate(spvReflectEnumeratePushConstantBlocks, rewritten, count, blocks)) {
            return Tool("SPIR-V push constant reflection failed");
        }

        for (uint32 index = 0; index < count; ++index) {
            ShaderReflectedResource resource;
            resource.kind = ShaderResourceKind::ConstantBuffer;
            resource.name = Name(blocks[index]->name);
            resource.rootConstants = true;
            resource.constantBufferBytes = blocks[index]->size;
            if (!AddMembers(resource, *blocks[index]) || !out_.resources.TryPushBack(resource)) {
                return Tool("SPIR-V push constant member reflection failed");
            }
        }

        SpvReflectInterfaceVariable* variables[MaxReflectedItems] = {};
        count = MaxReflectedItems;
        if (!Enumerate(spvReflectEnumerateInputVariables, raw, count, variables) ||
            !AddSignature(variables, count, out_.inputs)) {
            return Tool("SPIR-V input reflection failed");
        }

        count = MaxReflectedItems;
        if (!Enumerate(spvReflectEnumerateOutputVariables, raw, count, variables) ||
            !AddSignature(variables, count, out_.outputs)) {
            return Tool("SPIR-V output reflection failed");
        }

        return ShaderToolOutcome::Success;
    }

private:
    template <class Function, class Item>
    static bool Enumerate(Function function, const SpvReflectShaderModule& module, uint32& count,
                          Item** items) noexcept {
        uint32 total = 0;
        if (function(&module, &total, nullptr) != SPV_REFLECT_RESULT_SUCCESS || total > count) {
            return false;
        }

        count = total;
        return total == 0 || function(&module, &count, items) == SPV_REFLECT_RESULT_SUCCESS;
    }

    static StringView Name(const char* name) noexcept {
        return name != nullptr ? StringView(name) : StringView();
    }

    ShaderToolOutcome Tool(StringView message) noexcept {
        (void)diagnostics_.TryAppend(message);
        return ShaderToolOutcome::ToolError;
    }

    ShaderToolOutcome Compile(StringView name, StringView message) noexcept {
        (void)(diagnostics_.TryAppend("resource '") && diagnostics_.TryAppend(name) &&
               diagnostics_.TryAppend("': ") && diagnostics_.TryAppend(message));
        return ShaderToolOutcome::CompileError;
    }

    // 블록 멤버 하나와 (구조체면) 그 자식이다.
    bool AddMember(const SpvReflectBlockVariable& variable, uint32 parent) noexcept {
        const SpvReflectTypeDescription* type = variable.type_description;
        if (type == nullptr) {
            return false;
        }

        ShaderReflectedMember member;
        member.name = Name(variable.name);
        member.offset = variable.offset;
        member.bytes = variable.size;
        member.parent = parent;
        const SpvReflectTypeFlags flags = type->type_flags;
        const uint32 width = variable.numeric.scalar.width;
        if ((flags & SPV_REFLECT_TYPE_FLAG_STRUCT) != 0) {
            member.base = ShaderReflectedBase::Struct;
        } else if ((flags & SPV_REFLECT_TYPE_FLAG_BOOL) != 0) {
            member.base = ShaderReflectedBase::Bool;
        } else if (width == 64) {
            member.base = (flags & SPV_REFLECT_TYPE_FLAG_FLOAT) != 0 ? ShaderReflectedBase::Double
                                                                     : ShaderReflectedBase::Other;
        } else if (width == 16) {
            member.base = ShaderReflectedBase::Half;
        } else if ((flags & SPV_REFLECT_TYPE_FLAG_FLOAT) != 0) {
            member.base = ShaderReflectedBase::Float;
        } else if ((flags & SPV_REFLECT_TYPE_FLAG_INT) != 0) {
            member.base = variable.numeric.scalar.signedness != 0 ? ShaderReflectedBase::Int
                                                                  : ShaderReflectedBase::Uint;
        } else {
            member.base = ShaderReflectedBase::Other;
        }

        if ((flags & SPV_REFLECT_TYPE_FLAG_MATRIX) != 0) {
            member.rows = variable.numeric.matrix.row_count;
            member.columns = variable.numeric.matrix.column_count;
        } else if ((flags & SPV_REFLECT_TYPE_FLAG_VECTOR) != 0) {
            member.columns = variable.numeric.vector.component_count;
        }

        if ((flags & SPV_REFLECT_TYPE_FLAG_ARRAY) != 0) {
            // 다차원 배열은 규칙 밖 타입으로 둔다.
            if (variable.array.dims_count != 1) {
                member.base = ShaderReflectedBase::Other;
            }

            member.elements = variable.array.dims_count > 0 ? variable.array.dims[0] : 0;
        }

        const uint32 index = static_cast<uint32>(out_.members.Size());
        if (!out_.members.TryPushBack(member)) {
            return false;
        }

        if (member.base != ShaderReflectedBase::Struct) {
            return true;
        }

        for (uint32 child = 0; child < variable.member_count; ++child) {
            if (!AddMember(variable.members[child], index)) {
                return false;
            }
        }

        return true;
    }

    bool AddMembers(ShaderReflectedResource& resource,
                    const SpvReflectBlockVariable& block) noexcept {
        resource.firstMember = static_cast<uint32>(out_.members.Size());
        for (uint32 child = 0; child < block.member_count; ++child) {
            if (!AddMember(block.members[child], NoShaderReflectedParent)) {
                return false;
            }
        }

        resource.memberCount = static_cast<uint32>(out_.members.Size()) - resource.firstMember;
        return true;
    }

    ShaderToolOutcome AddBinding(const SpvReflectDescriptorBinding& binding,
                                 const SpvReflectDescriptorBinding& original) noexcept {
        ShaderReflectedResource resource;
        resource.name = Name(binding.name);
        resource.slot = binding.binding;
        resource.count = binding.count;
        switch (binding.descriptor_type) {
        case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            resource.kind = ShaderResourceKind::ConstantBuffer;
            break;
        case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            switch (original.user_type) {
            case SPV_REFLECT_USER_TYPE_STRUCTURED_BUFFER:
            case SPV_REFLECT_USER_TYPE_BYTE_ADDRESS_BUFFER:
                resource.kind = ShaderResourceKind::StorageBufferRead;
                break;
            case SPV_REFLECT_USER_TYPE_RW_STRUCTURED_BUFFER:
            case SPV_REFLECT_USER_TYPE_RW_BYTE_ADDRESS_BUFFER:
                resource.kind = ShaderResourceKind::StorageBufferReadWrite;
                break;
            default:
                return Compile(resource.name, "resource type is not supported (append, consume and "
                                              "counter buffers are rejected)");
            }

            break;
        case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            resource.kind = ShaderResourceKind::SampledTexture;
            break;
        case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_IMAGE:
        case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
            resource.kind = ShaderResourceKind::StorageImage;
            break;
        case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLER:
            resource.kind = ShaderResourceKind::Sampler;
            break;
        default:
            return Compile(resource.name, "descriptor type is not supported");
        }

        if (resource.kind == ShaderResourceKind::ConstantBuffer) {
            if (resource.name == GlobalsName) {
                out_.hasGlobals = true;
                return ShaderToolOutcome::Success;
            }

            resource.constantBufferBytes = binding.block.size;
            if (!AddMembers(resource, binding.block)) {
                return Tool("SPIR-V constant buffer member reflection failed");
            }
        }

        return out_.resources.TryPushBack(resource) ? ShaderToolOutcome::Success
                                                    : Tool("out of memory");
    }

    bool AddSignature(SpvReflectInterfaceVariable** variables, uint32 count,
                      Array<ShaderReflectedSignature>& out) noexcept {
        for (uint32 index = 0; index < count; ++index) {
            const SpvReflectInterfaceVariable& variable = *variables[index];
            ShaderReflectedSignature element;
            if ((variable.decoration_flags & SPV_REFLECT_DECORATION_BUILT_IN) != 0) {
                element.semantic = BuiltInSemantic(variable.built_in);
                if (element.semantic.IsEmpty()) {
                    element.semantic = "SV_Unknown";
                }
            } else {
                element = SplitShaderSemantic(Name(variable.semantic));
            }

            if (!out.TryPushBack(element)) {
                return false;
            }
        }

        return true;
    }

    ShaderReflection& out_;
    String& diagnostics_;
};

} // namespace spirv_reflection_detail

ShaderToolOutcome BuildSpirvBindingLayout(const ShaderFormatDefinition& format, uint32 stage,
                                          Span<const ShaderParameterLayout> layouts,
                                          Span<const uint32> raw, Span<const uint32> rewritten,
                                          Array<uint8>& out, String& diagnostics) noexcept {
    using namespace spirv_reflection_detail;
    out.Clear();
    Module source;
    Module module;
    if (!source.Open(raw) || !module.Open(rewritten)) {
        (void)diagnostics.TryAppend("SPIRV-Reflect could not parse the module");
        return ShaderToolOutcome::ToolError;
    }

    ShaderReflection reflection(out.GetAllocator());
    reflection.stage = stage;
    Converter converter(reflection, diagnostics);
    const ShaderToolOutcome converted = converter.Run(*source.Get(), *module.Get());
    if (converted != ShaderToolOutcome::Success) {
        return converted;
    }

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

} // namespace rocks::shader
