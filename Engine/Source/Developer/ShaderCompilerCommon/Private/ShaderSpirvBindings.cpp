#include <ShaderCompilerCommon/ShaderSpirvBindings.h>

#include "ShaderCompilerCommonInternal.h"

#include <Core/Algorithms.h>
#include <spirv_reflect.h>

// SPIR-V 경로 바인딩 정규화(Docs/Design/ShaderSystem 16.1절 SSD30·SSD33·SSD34·SSD36·SSD39·SSD40)다.
// 규칙은 공개 헤더 주석을 따른다.
namespace rocks::shader {

namespace spirv_bindings_detail {

// 변환 결과다.
enum class ConvertOutcome : uint8 {
    Success,
    CompileError,
    ToolError
};

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
// texture 번호 사용 표시 비트 수(u32 — Metal texture 인덱스 상한 31보다 넉넉하다)다.
constexpr uint32 MetalIndexMaskBits = 32;

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
    case SpvBuiltInPointSize:
        // HLSL에는 점 크기 시스템 값이 없다(D3D10 이후 점은 늘 1 pixel). SPIR-V 형식의 VS가
        // [[vk::builtin("PointSize")]]로 쓰는 출력이다(Vulkan·GLES 점 기본도형).
        return "SV_PointSize";
    default:
        return StringView();
    }
}

class Converter {
public:
    Converter(ShaderReflection& out, String& diagnostics) noexcept
        : out_(out), diagnostics_(diagnostics) {
    }

    ConvertOutcome Run(const SpvReflectShaderModule& raw,
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

            const ConvertOutcome added = AddBinding(*bindings[index], *original);
            if (added != ConvertOutcome::Success) {
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

        return ConvertOutcome::Success;
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

    ConvertOutcome Tool(StringView message) noexcept {
        (void)diagnostics_.TryAppend(message);
        return ConvertOutcome::ToolError;
    }

    ConvertOutcome Compile(StringView name, StringView message) noexcept {
        (void)(diagnostics_.TryAppend("resource '") && diagnostics_.TryAppend(name) &&
               diagnostics_.TryAppend("': ") && diagnostics_.TryAppend(message));
        return ConvertOutcome::CompileError;
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

    ConvertOutcome AddBinding(const SpvReflectDescriptorBinding& binding,
                              const SpvReflectDescriptorBinding& original) noexcept {
        ShaderReflectedResource resource;
        resource.name = Name(binding.name);
        resource.slot = binding.binding;
        resource.count = binding.count;
        resource.debugBuffer = binding.set == SpirvDebugBufferSet;
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

        resource.shape = ShapeOf(binding, original);
        if (resource.shape == ShaderResourceShape::StructuredBuffer) {
            resource.structureStride = StructureStride(binding);
        }

        if (resource.kind == ShaderResourceKind::SampledTexture ||
            resource.kind == ShaderResourceKind::StorageImage) {
            resource.component = ComponentOf(binding.type_description);
        }

        if (resource.kind == ShaderResourceKind::ConstantBuffer) {
            if (resource.name == GlobalsName) {
                out_.hasGlobals = true;
                return ConvertOutcome::Success;
            }

            resource.constantBufferBytes = binding.block.size;
            if (!AddMembers(resource, binding.block)) {
                return Tool("SPIR-V constant buffer member reflection failed");
            }
        }

        return out_.resources.TryPushBack(resource) ? ConvertOutcome::Success
                                                    : Tool("out of memory");
    }

    // 자원 모양이다(값 1 v3). 고쳐 쓰기 전 모듈(original)의 user type으로 구조화·raw를 가른다.
    static ShaderResourceShape ShapeOf(const SpvReflectDescriptorBinding& binding,
                                       const SpvReflectDescriptorBinding& original) noexcept {
        switch (binding.descriptor_type) {
        case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
        case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
            return ShaderResourceShape::TypedBuffer;
        case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            return original.user_type == SPV_REFLECT_USER_TYPE_BYTE_ADDRESS_BUFFER ||
                           original.user_type == SPV_REFLECT_USER_TYPE_RW_BYTE_ADDRESS_BUFFER
                       ? ShaderResourceShape::RawBuffer
                       : ShaderResourceShape::StructuredBuffer;
        case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            break;
        default:
            return ShaderResourceShape::None;
        }

        const bool arrayed = binding.image.arrayed != 0;
        switch (binding.image.dim) {
        case SpvDim1D:
            return arrayed ? ShaderResourceShape::Texture1DArray : ShaderResourceShape::Texture1D;
        case SpvDim2D:
            if (binding.image.ms != 0) {
                return arrayed ? ShaderResourceShape::Texture2DMultisampleArray
                               : ShaderResourceShape::Texture2DMultisample;
            }

            return arrayed ? ShaderResourceShape::Texture2DArray : ShaderResourceShape::Texture2D;
        case SpvDim3D:
            return ShaderResourceShape::Texture3D;
        case SpvDimCube:
            return arrayed ? ShaderResourceShape::TextureCubeArray
                           : ShaderResourceShape::TextureCube;
        default:
            return ShaderResourceShape::None;
        }
    }

    // 구조화 버퍼 원소 byte다(DXC 블록 struct { T _m0[]; }의 런타임 배열 stride).
    static uint32 StructureStride(const SpvReflectDescriptorBinding& binding) noexcept {
        const SpvReflectTypeDescription* type = binding.type_description;
        if (type != nullptr && type->member_count > 0 && type->members != nullptr &&
            type->members[0].traits.array.stride != 0) {
            return type->members[0].traits.array.stride;
        }

        if (binding.block.member_count == 0 || binding.block.members == nullptr) {
            return 0;
        }

        return binding.block.members[0].array.stride;
    }

    // 원소 기본 타입이다(이미지의 sampled type — SPIRV-Reflect는 그 숫자 특성을 이미지 타입에
    // 둔다).
    static ShaderComponentType ComponentOf(const SpvReflectTypeDescription* type) noexcept {
        if (type == nullptr || (type->type_flags & SPV_REFLECT_TYPE_FLAG_FLOAT) != 0) {
            return ShaderComponentType::Float;
        }

        if ((type->type_flags & SPV_REFLECT_TYPE_FLAG_INT) != 0) {
            return type->traits.numeric.scalar.signedness != 0 ? ShaderComponentType::Int
                                                               : ShaderComponentType::Uint;
        }

        return ShaderComponentType::Float;
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
                element.component = ComponentOf(variable.type_description);
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

} // namespace spirv_bindings_detail

bool AssignMetalBindingIndices(ShaderBindingLayout& layout, Array<MetalResourceBinding>& out,
                               uint32& sizeIndex, String& diagnostics) noexcept {
    using namespace spirv_bindings_detail;
    out.Clear();
    sizeIndex = NoMetalIndex;
    const uint32 stageSet = layout.stage == ShaderStagePixel ? 1u : 0u;
    const uint32 maxBuffer =
        layout.stage == ShaderStageCompute ? MetalComputeMaxBuffer : MetalGraphicsMaxBuffer;
    const auto isDebug = [](const ShaderBindingResource& resource) noexcept {
        return resource.kind == ShaderResourceKind::StorageBufferReadWrite &&
               resource.name == StringView("RocksDebugBuffer");
    };
    const auto push = [&](uint32 set, uint32 binding, uint32 buffer, uint32 texture,
                          uint32 sampler) noexcept {
        return static_cast<bool>(
            out.TryPushBack(MetalResourceBinding{set, binding, buffer, texture, sampler}));
    };
    if (layout.rootConstantBytes > 0 &&
        !push(MetalPushConstantSet, 0, MetalRootConstantsBuffer, NoMetalIndex, NoMetalIndex)) {
        return false;
    }

    // buffer: 1부터 UAV(texture 번호도 같은 값), 상수 버퍼, 구조화 SRV, 크기 side table.
    uint32 nextBuffer = MetalRootConstantsBuffer + 1;
    uint32 usedTextures = 0;
    bool storage = false;
    const ShaderResourceKind order[] = {
        ShaderResourceKind::StorageBufferReadWrite, ShaderResourceKind::StorageImage,
        ShaderResourceKind::ConstantBuffer, ShaderResourceKind::StorageBufferRead};
    for (const ShaderResourceKind kind : order) {
        for (ShaderBindingResource& resource : layout.resources) {
            if (resource.kind != kind) {
                continue;
            }

            const uint32 binding = resource.slot;
            if (isDebug(resource)) {
                if (!push(SpirvDebugBufferSet, binding, MetalDebugBuffer, NoMetalIndex,
                          NoMetalIndex)) {
                    return false;
                }

                resource.slot = static_cast<uint16>(MetalDebugBuffer);
                continue;
            }

            const bool unordered = kind == ShaderResourceKind::StorageBufferReadWrite ||
                                   kind == ShaderResourceKind::StorageImage;
            storage = storage || kind == ShaderResourceKind::StorageBufferReadWrite ||
                      kind == ShaderResourceKind::StorageBufferRead;
            const uint32 buffer = nextBuffer++;
            if (unordered && buffer < MetalIndexMaskBits) {
                usedTextures |= 1u << buffer;
            }

            if (!push(stageSet, binding, buffer, unordered ? buffer : NoMetalIndex, NoMetalIndex)) {
                return false;
            }

            resource.slot = static_cast<uint16>(buffer);
        }
    }

    if (storage) {
        sizeIndex = nextBuffer++;
    }

    const uint32 lastBuffer = nextBuffer - 1;
    if (nextBuffer > MetalRootConstantsBuffer + 1 && lastBuffer > maxBuffer) {
        (void)diagnostics.TryAppend(
            "Metal buffer index exceeds the stage limit (vertex and pixel 21, "
            "compute 30 — ShaderSystem 7.2 item 4)");
        return false;
    }

    // texture: UAV가 차지한 번호를 비우고 SRV 텍스처는 0과 남은 가장 낮은 번호부터. sampler: 0부터.
    uint32 nextTexture = 0;
    uint32 nextSampler = 0;
    for (ShaderBindingResource& resource : layout.resources) {
        const uint32 binding = resource.slot;
        if (resource.kind == ShaderResourceKind::SampledTexture) {
            while (nextTexture < MetalIndexMaskBits && (usedTextures & (1u << nextTexture)) != 0) {
                ++nextTexture;
            }

            if (!push(stageSet, binding, NoMetalIndex, nextTexture, NoMetalIndex)) {
                return false;
            }

            resource.slot = static_cast<uint16>(nextTexture++);
        } else if (resource.kind == ShaderResourceKind::Sampler) {
            if (!push(stageSet, binding, NoMetalIndex, NoMetalIndex, nextSampler)) {
                return false;
            }

            resource.slot = static_cast<uint16>(nextSampler++);
        }
    }

    // storage image의 slot은 texture 번호(= buffer 번호)다. 값 1 규칙대로 (종류, slot) 순으로 다시
    // 맞춘다.
    SortShaderBindingResources(layout);
    return true;
}

void SortShaderBindingResources(ShaderBindingLayout& layout) noexcept {
    layout.resources.Sort(
        [](const ShaderBindingResource& a, const ShaderBindingResource& b) noexcept {
            return a.kind < b.kind || (a.kind == b.kind && a.slot < b.slot);
        });
    Array<ShaderBindingMember> members(layout.members.GetAllocator());
    if (!members.TryReserve(layout.members.Size())) {
        return;
    }

    for (ShaderBindingResource& resource : layout.resources) {
        const uint32 first = static_cast<uint32>(members.Size());
        for (uint32 member = 0; member < resource.memberCount; ++member) {
            (void)members.TryPushBack(layout.members[resource.firstMember + member]);
        }

        resource.firstMember = first;
    }

    layout.members = Move(members);
}

ShaderWorkerJobStatus BuildSpirvBindingLayout(const SpirvBindingRequest& request,
                                              SpirvBindingResult& out,
                                              String& diagnostics) noexcept {
    using namespace spirv_bindings_detail;
    out.bindingLayout.Clear();
    out.metalBindings.Clear();
    out.metalBufferSizeIndex = NoMetalIndex;
    if (request.format == nullptr) {
        (void)diagnostics.TryAppend("SPIR-V binding request has no format");
        return ShaderWorkerJobStatus::ToolError;
    }

    Module source;
    Module module;
    if (!source.Open(request.raw) || !module.Open(request.rewritten)) {
        (void)diagnostics.TryAppend("SPIRV-Reflect could not parse the module");
        return ShaderWorkerJobStatus::ToolError;
    }

    Allocator& allocator = out.bindingLayout.GetAllocator();
    ShaderReflection reflection(allocator);
    reflection.stage = request.stage;
    reflection.requiredFeatures = request.requiredFeatures;
    if (request.hasUses) {
        reflection.hasSamplerUses = true;
        if (!reflection.samplerUses.TryAppend(request.uses)) {
            return ShaderWorkerJobStatus::ToolError;
        }
    }

    Converter converter(reflection, diagnostics);
    const ConvertOutcome converted = converter.Run(*source.Get(), *module.Get());
    if (converted != ConvertOutcome::Success) {
        return converted == ConvertOutcome::CompileError ? ShaderWorkerJobStatus::CompileError
                                                         : ShaderWorkerJobStatus::ToolError;
    }

    ShaderBindingLayout layout(allocator);
    if (!NormalizeShaderReflection(*request.format, reflection, request.layouts, layout,
                                   diagnostics)) {
        return ShaderWorkerJobStatus::CompileError;
    }

    if (request.format->backend == ShaderBackend::OpenGL &&
        !BuildGlesPlan(reflection, layout, request.uses, out.gles, diagnostics)) {
        return ShaderWorkerJobStatus::CompileError;
    }

    if (request.format->backend == ShaderBackend::Metal &&
        !AssignMetalBindingIndices(layout, out.metalBindings, out.metalBufferSizeIndex,
                                   diagnostics)) {
        return ShaderWorkerJobStatus::CompileError;
    }

    if (!EncodeShaderBindingLayout(layout, out.bindingLayout)) {
        (void)diagnostics.TryAppend("binding layout encoding failed");
        return ShaderWorkerJobStatus::ToolError;
    }

    return ShaderWorkerJobStatus::Success;
}

bool CheckStorageImageFormats(StringView source, String& diagnostics) noexcept {
    constexpr StringView Types[] = {"RWTexture1DArray", "RWTexture2DArray", "RWTexture1D",
                                    "RWTexture2D",      "RWTexture3D",      "RWBuffer"};
    constexpr StringView Attribute("[[vk::image_format(");
    const char* text = source.Data();
    const usize size = source.ByteCount();
    const auto identifier = [](char value) noexcept {
        return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
               (value >= '0' && value <= '9') || value == '_';
    };
    for (usize at = 0; at < size; ++at) {
        if (!identifier(text[at]) || (at > 0 && identifier(text[at - 1]))) {
            continue;
        }

        usize end = at;
        while (end < size && identifier(text[end])) {
            ++end;
        }

        const StringView word(text + at, end - at);
        bool storage = false;
        for (const StringView type : Types) {
            storage = storage || word == type;
        }

        if (!storage) {
            at = end - 1;
            continue;
        }

        // 바로 앞(공백 제외)에 이어진 [[…]] 묶음 중 하나가 vk::image_format(으로 시작해야 한다
        // (RWTextureCube는 표지 attribute를 하나 더 단다 — Platform.rsh).
        const auto skipSpace = [text](usize position) noexcept {
            while (position > 0 && (text[position - 1] == ' ' || text[position - 1] == '\t' ||
                                    text[position - 1] == '\r' || text[position - 1] == '\n')) {
                --position;
            }

            return position;
        };
        usize before = skipSpace(at);
        bool formatted = false;
        while (!formatted && before >= 2 && text[before - 1] == ']' && text[before - 2] == ']') {
            usize open = before - 2;
            while (open >= 2 && !(text[open - 1] == '[' && text[open - 2] == '[')) {
                --open;
            }

            if (open < 2) {
                break;
            }

            formatted = StringView(text + open - 2, size - (open - 2))
                            .StartsWith(Attribute, StringComparison::CaseSensitive);
            before = skipSpace(open - 2);
        }

        if (!formatted) {
            (void)(diagnostics.TryAppend("storage image declaration '") &&
                   diagnostics.TryAppend(word) &&
                   diagnostics.TryAppend("' needs ROCKS_IMAGE_FORMAT(<format>) (ShaderSystem 7.2 "
                                         "item 1)"));
            return false;
        }

        at = end - 1;
    }

    return true;
}

} // namespace rocks::shader
