#include "DxcSpirv.h"

#include <ShaderFormatVulkan/SpirvRewrite.h>
#include <ShaderFormatVulkan/VulkanShaderCompiler.h>
#include <spirv-tools/libspirv.h>
#include <spirv_reflect.h>

namespace rocks::shader {

namespace {

void Append(String& text, StringView part) noexcept {
    auto appended = text.TryAppend(part);
    (void)appended;
}

// 도구 열기 실패 코드를 오류 코드로 옮긴다.
ErrorCode LoadError(ShaderToolLoadCode code) noexcept {
    switch (code) {
    case ShaderToolLoadCode::UnsupportedHost:
        return ErrorCode::Unsupported;
    case ShaderToolLoadCode::NotBootstrapped:
        return ErrorCode::NotFound;
    default:
        return ErrorCode::DependencyFailed;
    }
}

// SPIRV-Reflect 형식의 셰이더 입력 타입 계열이다. 모르면 false다.
bool InputType(SpvReflectFormat format, graphics::VertexShaderType& type) noexcept {
    switch (format) {
    case SPV_REFLECT_FORMAT_R32_SFLOAT:
    case SPV_REFLECT_FORMAT_R32G32_SFLOAT:
    case SPV_REFLECT_FORMAT_R32G32B32_SFLOAT:
    case SPV_REFLECT_FORMAT_R32G32B32A32_SFLOAT:
        type = graphics::VertexShaderType::Float;
        return true;
    case SPV_REFLECT_FORMAT_R32_SINT:
    case SPV_REFLECT_FORMAT_R32G32_SINT:
    case SPV_REFLECT_FORMAT_R32G32B32_SINT:
    case SPV_REFLECT_FORMAT_R32G32B32A32_SINT:
        type = graphics::VertexShaderType::Int;
        return true;
    case SPV_REFLECT_FORMAT_R32_UINT:
    case SPV_REFLECT_FORMAT_R32G32_UINT:
    case SPV_REFLECT_FORMAT_R32G32B32_UINT:
    case SPV_REFLECT_FORMAT_R32G32B32A32_UINT:
        type = graphics::VertexShaderType::Uint;
        return true;
    default:
        return false;
    }
}

// SPIRV-Reflect descriptor 종류를 Rocks 종류로 옮긴다. 모르면 false다.
bool BindingKind(SpvReflectDescriptorType type, graphics::ShaderBindingKind& kind) noexcept {
    switch (type) {
    case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        kind = graphics::ShaderBindingKind::ConstantBuffer;
        return true;
    case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        kind = graphics::ShaderBindingKind::StorageBuffer;
        return true;
    case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_IMAGE:
    case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
        kind = graphics::ShaderBindingKind::StorageImage;
        return true;
    case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
    case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
    case SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
        kind = graphics::ShaderBindingKind::SampledImage;
        return true;
    case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLER:
        kind = graphics::ShaderBindingKind::Sampler;
        return true;
    default:
        return false;
    }
}

// SPIRV-Reflect descriptor의 모양이다. 구조화·raw 구분은 고쳐 쓰기 전 모듈의
// SPV_GOOGLE_user_type(-fspv-reflect)에서 온다.
graphics::ShaderResourceShape BindingShape(SpvReflectDescriptorType type,
                                           SpvReflectUserType user) noexcept {
    switch (type) {
    case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
    case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
        return graphics::ShaderResourceShape::TexelBuffer;
    case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        return user == SPV_REFLECT_USER_TYPE_BYTE_ADDRESS_BUFFER ||
                       user == SPV_REFLECT_USER_TYPE_RW_BYTE_ADDRESS_BUFFER
                   ? graphics::ShaderResourceShape::RawBuffer
                   : graphics::ShaderResourceShape::StructuredBuffer;
    case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        return graphics::ShaderResourceShape::ConstantBuffer;
    case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLER:
        return graphics::ShaderResourceShape::Sampler;
    default:
        return graphics::ShaderResourceShape::Image;
    }
}

// 이미지·texel buffer의 원소 타입 계열이다(SPIRV-Reflect는 sampled type을 type_flags·signedness로
// 둔다).
graphics::PixelShaderType SampledType(const SpvReflectDescriptorBinding& binding) noexcept {
    const SpvReflectTypeDescription* type = binding.type_description;
    if (type == nullptr ||
        (binding.descriptor_type != SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE &&
         binding.descriptor_type != SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_IMAGE &&
         binding.descriptor_type != SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER &&
         binding.descriptor_type != SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER)) {
        return graphics::PixelShaderType::None;
    }

    if ((type->type_flags & SPV_REFLECT_TYPE_FLAG_FLOAT) != 0) {
        return graphics::PixelShaderType::Float;
    }

    if ((type->type_flags & SPV_REFLECT_TYPE_FLAG_INT) != 0) {
        return type->traits.numeric.scalar.signedness != 0 ? graphics::PixelShaderType::Int
                                                           : graphics::PixelShaderType::Uint;
    }

    return graphics::PixelShaderType::None;
}

// SPIRV-Reflect 모듈 수명이다.
class ReflectedModule {
public:
    ReflectedModule() noexcept = default;
    ReflectedModule(const ReflectedModule&) = delete;
    ReflectedModule& operator=(const ReflectedModule&) = delete;

    ~ReflectedModule() {
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

// 리플렉션 열거 결과를 담을 고정 칸 수다(개발 경로의 셰이더는 이보다 적다).
constexpr uint32 MaxReflectedItems = 64;

Result<void> ReflectFailure(String& diagnostics, StringView message) noexcept {
    Append(diagnostics, message);
    return Result<void>::Failure(ErrorCode::DependencyFailed);
}

// 고쳐 쓴 SPIR-V의 리플렉션(정점 입력·binding·그리기 상수 크기)을 out에 채운다. raw는 고쳐 쓰기 전
// DXC 출력이며 원문 register(고쳐 쓰기 전 set·binding)와 user type을 같은 SPIR-V id로 찾는다
// (고쳐 쓰기는 id를 바꾸지 않는다).
Result<void> Reflect(graphics::ShaderStage stage, Span<const uint32> raw,
                     graphics::CompiledShader& out, String& diagnostics) noexcept {
    ReflectedModule module;
    ReflectedModule source;
    if (!module.Open(out.code.View()) || !source.Open(raw)) {
        return ReflectFailure(diagnostics, StringView("SPIRV-Reflect could not parse the module"));
    }

    uint32 sourceCount = MaxReflectedItems;
    SpvReflectDescriptorBinding* sourceBindings[MaxReflectedItems] = {};
    if (spvReflectEnumerateDescriptorBindings(source.Get(), &sourceCount, nullptr) !=
            SPV_REFLECT_RESULT_SUCCESS ||
        sourceCount > MaxReflectedItems ||
        spvReflectEnumerateDescriptorBindings(source.Get(), &sourceCount, sourceBindings) !=
            SPV_REFLECT_RESULT_SUCCESS) {
        return ReflectFailure(diagnostics, StringView("source binding reflection failed"));
    }

    uint32 count = MaxReflectedItems;
    if (stage == graphics::ShaderStage::Vertex) {
        SpvReflectInterfaceVariable* inputs[MaxReflectedItems] = {};
        if (spvReflectEnumerateInputVariables(module.Get(), &count, nullptr) !=
                SPV_REFLECT_RESULT_SUCCESS ||
            count > MaxReflectedItems ||
            spvReflectEnumerateInputVariables(module.Get(), &count, inputs) !=
                SPV_REFLECT_RESULT_SUCCESS) {
            return ReflectFailure(diagnostics, StringView("vertex input reflection failed"));
        }

        for (uint32 index = 0; index < count; ++index) {
            const SpvReflectInterfaceVariable& input = *inputs[index];
            if ((input.decoration_flags & SPV_REFLECT_DECORATION_BUILT_IN) != 0) {
                continue;
            }

            graphics::ShaderVertexInput entry;
            entry.location = input.location;
            if (!InputType(input.format, entry.type)) {
                return ReflectFailure(diagnostics,
                                      StringView("vertex input uses a non 32-bit component type"));
            }

            auto pushed = out.vertexInputs.TryPushBack(entry);
            if (!pushed) {
                return pushed;
            }
        }
    }

    count = MaxReflectedItems;
    SpvReflectDescriptorBinding* bindings[MaxReflectedItems] = {};
    if (spvReflectEnumerateDescriptorBindings(module.Get(), &count, nullptr) !=
            SPV_REFLECT_RESULT_SUCCESS ||
        count > MaxReflectedItems ||
        spvReflectEnumerateDescriptorBindings(module.Get(), &count, bindings) !=
            SPV_REFLECT_RESULT_SUCCESS) {
        return ReflectFailure(diagnostics, StringView("descriptor binding reflection failed"));
    }

    for (uint32 index = 0; index < count; ++index) {
        const SpvReflectDescriptorBinding& reflected = *bindings[index];
        graphics::ShaderBinding binding;
        binding.set = reflected.set;
        binding.binding = reflected.binding;
        if (!BindingKind(reflected.descriptor_type, binding.kind)) {
            return ReflectFailure(diagnostics, StringView("unsupported descriptor type"));
        }

        const SpvReflectDescriptorBinding* original = nullptr;
        for (uint32 candidate = 0; candidate < sourceCount; ++candidate) {
            if (sourceBindings[candidate]->spirv_id == reflected.spirv_id) {
                original = sourceBindings[candidate];
            }
        }

        if (original == nullptr) {
            return ReflectFailure(diagnostics,
                                  StringView("rewritten binding has no source binding"));
        }

        binding.sourceRegister = original->binding;
        binding.sourceSpace = original->set;
        binding.shape = BindingShape(reflected.descriptor_type, original->user_type);
        binding.sampledType = SampledType(reflected);
        // StructuredBuffer<T>는 { T _m0[]; } block이라 첫 member 배열의 stride가 원소 byte다.
        if (binding.shape == graphics::ShaderResourceShape::StructuredBuffer &&
            reflected.block.member_count != 0 && reflected.block.members != nullptr) {
            binding.structureStride = reflected.block.members[0].array.stride;
        }

        binding.writable =
            reflected.descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
            reflected.descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER ||
            (reflected.descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER &&
             (reflected.decoration_flags & SPV_REFLECT_DECORATION_NON_WRITABLE) == 0);

        auto pushed = out.bindings.TryPushBack(binding);
        if (!pushed) {
            return pushed;
        }
    }

    count = MaxReflectedItems;
    SpvReflectBlockVariable* blocks[MaxReflectedItems] = {};
    if (spvReflectEnumeratePushConstantBlocks(module.Get(), &count, nullptr) !=
            SPV_REFLECT_RESULT_SUCCESS ||
        count > MaxReflectedItems ||
        spvReflectEnumeratePushConstantBlocks(module.Get(), &count, blocks) !=
            SPV_REFLECT_RESULT_SUCCESS) {
        return ReflectFailure(diagnostics, StringView("push constant reflection failed"));
    }

    for (uint32 index = 0; index < count; ++index) {
        const uint32 end = blocks[index]->offset + blocks[index]->size;
        out.rootConstantBytes = end > out.rootConstantBytes ? end : out.rootConstantBytes;
    }

    return Result<void>::Success();
}

template <class Function>
Function Lookup(const SpirvToolsFunctions& tools, usize index) noexcept {
    return reinterpret_cast<Function>(tools.address[index]);
}

// spirv-val(Vulkan 1.1 환경)로 고쳐 쓴 모듈을 검증한다(ShaderSystem 7.2절 7항).
Result<void> Validate(const SpirvToolsFunctions& tools, Span<const uint32> code,
                      String& diagnostics) noexcept {
    using Index = SpirvToolsFunctions;
    const auto contextCreate = Lookup<decltype(&spvContextCreate)>(tools, Index::ContextCreate);
    const auto contextDestroy = Lookup<decltype(&spvContextDestroy)>(tools, Index::ContextDestroy);
    const auto optionsCreate =
        Lookup<decltype(&spvValidatorOptionsCreate)>(tools, Index::ValidatorOptionsCreate);
    const auto optionsDestroy =
        Lookup<decltype(&spvValidatorOptionsDestroy)>(tools, Index::ValidatorOptionsDestroy);
    const auto validate =
        Lookup<decltype(&spvValidateWithOptions)>(tools, Index::ValidateWithOptions);
    const auto diagnosticDestroy =
        Lookup<decltype(&spvDiagnosticDestroy)>(tools, Index::DiagnosticDestroy);
    spv_context context = contextCreate(SPV_ENV_VULKAN_1_1);
    spv_validator_options options = optionsCreate();
    spv_const_binary_t binary{code.Data(), code.Size()};
    spv_diagnostic diagnostic = nullptr;
    const spv_result_t result = validate(context, options, &binary, &diagnostic);
    if (result != SPV_SUCCESS) {
        Append(diagnostics, StringView("spirv-val rejected the rewritten SPIR-V: "));
        if (diagnostic != nullptr && diagnostic->error != nullptr) {
            Append(diagnostics, StringView(diagnostic->error));
        }
    }

    diagnosticDestroy(diagnostic);
    optionsDestroy(options);
    contextDestroy(context);
    return result == SPV_SUCCESS ? Result<void>::Success()
                                 : Result<void>::Failure(ErrorCode::DependencyFailed);
}

} // namespace

struct VulkanShaderCompiler::State {
    explicit State(Allocator& allocator) noexcept : allocator(allocator) {
    }

    Allocator& allocator;
    ShaderToolLibrary dxc;
    ShaderToolLibrary spirvTools;
    platform::DynamicLibraryLease dxcLease;
    platform::DynamicLibraryLease spirvToolsLease;
    DxcFunctions dxcFunctions;
    SpirvToolsFunctions spirvToolsFunctions;

    ~State() {
        // 함수 표를 놓은 뒤 라이브러리를 내린다(lease가 남으면 Close가 거부한다).
        dxcLease.Reset();
        spirvToolsLease.Reset();
        auto dxcClosed = dxc.Close();
        auto toolsClosed = spirvTools.Close();
        (void)dxcClosed;
        (void)toolsClosed;
    }
};

namespace {

// 도구 하나를 표에서 찾아 저장소에서 열고 함수 표를 얻는다.
template <class Functions>
Result<void> OpenTool(const char* name, const char* root, usize rootBytes,
                      ShaderToolLibrary& library, Functions& functions,
                      platform::DynamicLibraryLease& lease, String& diagnostics) noexcept {
    const ShaderTool* tool = FindShaderTool(StringView(name));
    ShaderToolLoadStatus status;
    if (tool == nullptr) {
        status.code = ShaderToolLoadCode::UnsupportedHost;
    } else {
        status = library.Open(*tool, root, rootBytes);
        if (status) {
            status = library.FindFunctions(functions, lease);
        }
    }

    if (status) {
        return Result<void>::Success();
    }

    Append(diagnostics, StringView(name));
    Append(diagnostics, StringView(": "));
    Append(diagnostics, StringView(ShaderToolLoadMessage(status.code)));
    return Result<void>::Failure(LoadError(status.code));
}

} // namespace

VulkanShaderCompiler::VulkanShaderCompiler(UniquePtr<State> state) noexcept : state_(Move(state)) {
}

VulkanShaderCompiler::~VulkanShaderCompiler() noexcept = default;

Result<UniquePtr<VulkanShaderCompiler>> VulkanShaderCompiler::Create(Allocator& allocator,
                                                                     String& diagnostics) noexcept {
    using Created = Result<UniquePtr<VulkanShaderCompiler>>;
    char root[MaxShaderToolPathBytes];
    usize rootBytes = 0;
    if (!GetShaderToolStoreRoot(root, sizeof(root), rootBytes)) {
        Append(diagnostics, StringView("shader tool store is not available on this host"));
        return Created::Failure(ErrorCode::Unsupported);
    }

    auto state = UniquePtr<State>::TryMake(allocator, allocator);
    if (!state) {
        return Created::Failure(state.Error());
    }

    State& tools = *state.Value();
    auto dxc = OpenTool(DxcToolName, root, rootBytes, tools.dxc, tools.dxcFunctions, tools.dxcLease,
                        diagnostics);
    if (!dxc) {
        return Created::Failure(dxc.Error());
    }

    auto validator = OpenTool(SpirvToolsToolName, root, rootBytes, tools.spirvTools,
                              tools.spirvToolsFunctions, tools.spirvToolsLease, diagnostics);
    if (!validator) {
        return Created::Failure(validator.Error());
    }

    return UniquePtr<VulkanShaderCompiler>::TryMake(allocator, Move(state.Value()));
}

Result<void> VulkanShaderCompiler::CompileSpirv(graphics::ShaderStage stage, StringView hlsl,
                                                graphics::CompiledShader& out,
                                                String& diagnostics) noexcept {
    out.code.Clear();
    out.vertexInputs.Clear();
    out.bindings.Clear();
    out.rootConstantBytes = 0;
    Array<uint32> raw(state_->allocator);
    auto compiled = CompileHlslToSpirv(state_->dxcFunctions, stage, hlsl, raw, diagnostics);
    if (!compiled) {
        return compiled;
    }

    auto rewritten =
        RewriteSpirv(raw.View(), stage, StringView(DrawConstantsBlockName), out.code, diagnostics);
    if (!rewritten) {
        return rewritten;
    }

    auto validated = Validate(state_->spirvToolsFunctions, out.code.View(), diagnostics);
    if (!validated) {
        return validated;
    }

    return Reflect(stage, raw.View(), out, diagnostics);
}

} // namespace rocks::shader
