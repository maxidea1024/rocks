#include <ShaderToolLoader/ShaderToolCalls.h>

#include <Base/InitializerList.h>

#include <spirv-tools/libspirv.h>
#include <spirv_cross_c.h>
#include <stdio.h>

// 형식 처리기 공용 도구 호출(Docs/Design/ShaderSystem 11.1절 SH1e E2)이다. 규칙은 공개 헤더 주석을
// 따른다.
namespace rocks::shader {

namespace tool_calls_detail {

// 고정 DXC 인자 수의 상한이다.
constexpr usize MaxSpirvDxcArguments = 16;
// SPIR-V 단어 크기다.
constexpr usize SpirvWordBytes = sizeof(uint32);
// SPIRV-Cross HLSL 셰이더 모델 값(5.0)이다.
constexpr uint32 Hlsl50ShaderModel = 50;

constexpr StringView OptimizationFlags[ShaderOptimizationLevels] = {"-O0", "-O1", "-O2", "-O3"};

template <class Function, class Table>
Function ToolFunction(const Table& table, usize index) noexcept {
    return reinterpret_cast<Function>(table.address[index]);
}

void Note(String& diagnostics, StringView message, StringView detail = StringView()) noexcept {
    (void)(diagnostics.TryAppend(message) &&
           (detail.IsEmpty() || (diagnostics.TryAppend(": ") && diagnostics.TryAppend(detail))));
}

} // namespace tool_calls_detail

bool ValidateSpirv(const SpirvToolsFunctions& tools, Span<const uint8> spirv,
                   String& diagnostics) noexcept {
    using namespace tool_calls_detail;
    using Index = SpirvToolsFunctions;
    const auto contextCreate =
        ToolFunction<decltype(&spvContextCreate)>(tools, Index::ContextCreate);
    const auto contextDestroy =
        ToolFunction<decltype(&spvContextDestroy)>(tools, Index::ContextDestroy);
    const auto optionsCreate =
        ToolFunction<decltype(&spvValidatorOptionsCreate)>(tools, Index::ValidatorOptionsCreate);
    const auto optionsDestroy =
        ToolFunction<decltype(&spvValidatorOptionsDestroy)>(tools, Index::ValidatorOptionsDestroy);
    const auto validate =
        ToolFunction<decltype(&spvValidateWithOptions)>(tools, Index::ValidateWithOptions);
    const auto diagnosticDestroy =
        ToolFunction<decltype(&spvDiagnosticDestroy)>(tools, Index::DiagnosticDestroy);
    if (contextCreate == nullptr || validate == nullptr || spirv.Size() % SpirvWordBytes != 0) {
        Note(diagnostics, "spirv-val is not available or the module is not word aligned");
        return false;
    }

    spv_context context = contextCreate(SPV_ENV_VULKAN_1_1);
    spv_validator_options options = optionsCreate();
    spv_const_binary_t binary{reinterpret_cast<const uint32*>(spirv.Data()),
                              spirv.Size() / SpirvWordBytes};
    spv_diagnostic diagnostic = nullptr;
    const spv_result_t validated = validate(context, options, &binary, &diagnostic);
    if (validated != SPV_SUCCESS && diagnostic != nullptr && diagnostic->error != nullptr) {
        Note(diagnostics, StringView(diagnostic->error));
    }

    diagnosticDestroy(diagnostic);
    optionsDestroy(options);
    contextDestroy(context);
    return validated == SPV_SUCCESS;
}

ShaderToolOutcome CompileHlslToSpirv(const ShaderToolSet& tools, const SpirvCompileRequest& request,
                                     Array<uint8>& spirv, String& diagnostics) noexcept {
    using namespace tool_calls_detail;
    spirv.Clear();
    diagnostics.Clear();
    if (request.optimization >= ShaderOptimizationLevels) {
        Note(diagnostics, "invalid optimization level");
        return ShaderToolOutcome::CompileError;
    }

    StringView arguments[MaxSpirvDxcArguments] = {"-E",
                                                  request.entry,
                                                  "-T",
                                                  request.profile,
                                                  "-HV",
                                                  request.hlsl2021 ? StringView("2021")
                                                                   : StringView("2018"),
                                                  OptimizationFlags[request.optimization],
                                                  "-Zpr",
                                                  "-WX",
                                                  "-spirv",
                                                  "-fspv-target-env=vulkan1.1"};
    usize count = 11;
    if (request.dxPositionW) {
        arguments[count++] = "-fvk-use-dx-position-w";
    }

    if (request.reflect) {
        arguments[count++] = "-fspv-reflect";
    }

    DxcCompileRequest dxc;
    dxc.source = request.source;
    dxc.arguments = Span<const StringView>(arguments, count);
    DxcCompileResult compiled(spirv.GetAllocator());
    const ShaderToolLoadStatus ran = CompileWithDxc(tools.Dxc(), dxc, compiled);
    if (!ran) {
        Note(diagnostics, "DXC call failed", StringView(ShaderToolLoadCodeName(ran.code)));
        return ran.code == ShaderToolLoadCode::UnsupportedHost ? ShaderToolOutcome::UnsupportedHost
                                                               : ShaderToolOutcome::ToolError;
    }

    if (!compiled.compiled) {
        Note(diagnostics, "DXC compile failed", compiled.diagnostics.View());
        return ShaderToolOutcome::CompileError;
    }

    String validation(spirv.GetAllocator());
    if (!ValidateSpirv(tools.SpirvTools(), compiled.object.View(), validation)) {
        Note(diagnostics, "spirv-val rejected the SPIR-V", validation.View());
        return ShaderToolOutcome::ToolError;
    }

    spirv = Move(compiled.object);
    return ShaderToolOutcome::Success;
}

namespace cross_binding_detail {

// 정점 입력 시맨틱 수·버퍼 크기("ATTRIBUTE15" NUL 포함보다 넉넉하다)다.
constexpr usize MaxVertexSemantics = 16;
constexpr usize VertexSemanticBytes = 16;
// D3D11 루트 상수 register(b13)와 그 cbuffer 이름 접미다(Platform.rsh, 16.1절 SSD32).
constexpr unsigned RootConstantsRegister = 13;
constexpr StringView RootConstantsSuffix("RootConstants");
// SPIR-V 장식 값(spirv.h)이다.
constexpr unsigned DecorationDescriptorSet = 34;
constexpr unsigned DecorationBinding = 33;
constexpr unsigned DecorationLocation = 30;
constexpr unsigned DecorationNonWritable = 24;
// ShaderStageBits 값(ShaderCore — 이 모듈은 의존하지 않는다)이다.
constexpr uint32 StageVertex = 1u << 0;
constexpr uint32 StagePixel = 1u << 1;
// 셰이더 하나에서 register를 정하는 자원 수 상한이다.
constexpr usize MaxHlslResources = 256;

SpvExecutionModel ExecutionModel(uint32 stage) noexcept {
    switch (stage) {
    case StageVertex:
        return SpvExecutionModelVertex;
    case StagePixel:
        return SpvExecutionModelFragment;
    default:
        return SpvExecutionModelGLCompute;
    }
}

// HLSL 5.0 register 배정(SSD38)이다.
bool AssignHlslRegisters(const SpirvCrossFunctions& cross, spvc_compiler compiler, uint32 stage,
                         char (&semantics)[cross_binding_detail::MaxVertexSemantics]
                                          [cross_binding_detail::VertexSemanticBytes]) noexcept {
    using namespace tool_calls_detail;
    using namespace cross_binding_detail;
    using Index = SpirvCrossFunctions;
    const auto createResources = ToolFunction<decltype(&spvc_compiler_create_shader_resources)>(
        cross, Index::CompilerCreateShaderResources);
    const auto listFor = ToolFunction<decltype(&spvc_resources_get_resource_list_for_type)>(
        cross, Index::ResourcesGetResourceListForType);
    const auto decoration =
        ToolFunction<decltype(&spvc_compiler_get_decoration)>(cross, Index::CompilerGetDecoration);
    const auto memberDecoration = ToolFunction<decltype(&spvc_compiler_get_member_decoration)>(
        cross, Index::CompilerGetMemberDecoration);
    const auto bindingInit = ToolFunction<decltype(&spvc_hlsl_resource_binding_init)>(
        cross, Index::HlslResourceBindingInit);
    const auto addBinding = ToolFunction<decltype(&spvc_compiler_hlsl_add_resource_binding)>(
        cross, Index::CompilerHlslAddResourceBinding);
    const auto addRemap = ToolFunction<decltype(&spvc_compiler_hlsl_add_vertex_attribute_remap)>(
        cross, Index::CompilerHlslAddVertexAttributeRemap);
    if (createResources == nullptr || listFor == nullptr || addBinding == nullptr ||
        addRemap == nullptr) {
        return false;
    }

    spvc_resources resources = nullptr;
    if (createResources(compiler, &resources) != SPVC_SUCCESS) {
        return false;
    }

    // register 공간: 0 b, 1 t, 2 u, 3 s. 공간마다 (set, binding) 순으로 0부터다.
    constexpr usize Spaces = 4;
    constexpr usize ConstantSpace = 0;
    constexpr usize ResourceSpace = 1;
    constexpr usize UnorderedSpace = 2;
    constexpr usize SamplerSpace = 3;
    struct Item {
        unsigned space;
        unsigned set;
        unsigned binding;
        bool root;
    };

    Item items[MaxHlslResources] = {};
    usize count = 0;
    const auto collect = [&](spvc_resource_type type, usize space) noexcept {
        const spvc_reflected_resource* list = nullptr;
        size_t size = 0;
        if (listFor(resources, type, &list, &size) != SPVC_SUCCESS) {
            return false;
        }

        for (size_t index = 0; index < size && count < MaxHlslResources; ++index) {
            usize chosen = space;
            if (type == SPVC_RESOURCE_TYPE_STORAGE_BUFFER) {
                chosen = memberDecoration(compiler, list[index].base_type_id, 0,
                                          static_cast<SpvDecoration>(DecorationNonWritable)) != 0
                             ? ResourceSpace
                             : UnorderedSpace;
            }

            const StringView name =
                list[index].name != nullptr ? StringView(list[index].name) : StringView();
            items[count++] = Item{
                static_cast<unsigned>(chosen),
                decoration(compiler, list[index].id,
                           static_cast<SpvDecoration>(DecorationDescriptorSet)),
                decoration(compiler, list[index].id, static_cast<SpvDecoration>(DecorationBinding)),
                chosen == ConstantSpace &&
                    name.EndsWith(RootConstantsSuffix, StringComparison::CaseSensitive)};
        }

        return true;
    };
    if (!collect(SPVC_RESOURCE_TYPE_UNIFORM_BUFFER, ConstantSpace) ||
        !collect(SPVC_RESOURCE_TYPE_STORAGE_BUFFER, UnorderedSpace) ||
        !collect(SPVC_RESOURCE_TYPE_SEPARATE_IMAGE, ResourceSpace) ||
        !collect(SPVC_RESOURCE_TYPE_STORAGE_IMAGE, UnorderedSpace) ||
        !collect(SPVC_RESOURCE_TYPE_SEPARATE_SAMPLERS, SamplerSpace)) {
        return false;
    }

    // 공간 안 순서는 (set, binding)이다.
    for (usize outer = 1; outer < count; ++outer) {
        for (usize inner = outer; inner > 0; --inner) {
            const Item& a = items[inner - 1];
            const Item& b = items[inner];
            const bool later =
                a.space > b.space || (a.space == b.space &&
                                      (a.set > b.set || (a.set == b.set && a.binding > b.binding)));
            if (!later) {
                break;
            }

            const Item swap = items[inner - 1];
            items[inner - 1] = items[inner];
            items[inner] = swap;
        }
    }

    unsigned next[Spaces] = {};
    const SpvExecutionModel model = ExecutionModel(stage);
    for (usize index = 0; index < count; ++index) {
        const Item& item = items[index];
        const unsigned reg = item.root ? RootConstantsRegister : next[item.space]++;
        spvc_hlsl_resource_binding binding;
        bindingInit(&binding);
        binding.stage = model;
        binding.desc_set = item.set;
        binding.binding = item.binding;
        binding.cbv = {0, reg};
        binding.srv = {0, reg};
        binding.uav = {0, reg};
        binding.sampler = {0, reg};
        if (addBinding(compiler, &binding) != SPVC_SUCCESS) {
            return false;
        }
    }

    if (model != SpvExecutionModelVertex) {
        return true;
    }

    // 정점 입력: 고쳐 쓰기가 location = n(ATTRIBUTEn)으로 맞췄으므로 location n → 시맨틱
    // ATTRIBUTEn이다.
    const spvc_reflected_resource* inputs = nullptr;
    size_t inputCount = 0;
    if (listFor(resources, SPVC_RESOURCE_TYPE_STAGE_INPUT, &inputs, &inputCount) != SPVC_SUCCESS) {
        return false;
    }

    for (size_t index = 0; index < inputCount && index < MaxVertexSemantics; ++index) {
        const unsigned location =
            decoration(compiler, inputs[index].id, static_cast<SpvDecoration>(DecorationLocation));
        (void)snprintf(semantics[index], VertexSemanticBytes, "ATTRIBUTE%u", location);
        spvc_hlsl_vertex_attribute_remap remap;
        remap.location = location;
        remap.semantic = semantics[index];
        if (addRemap(compiler, &remap, 1) != SPVC_SUCCESS) {
            return false;
        }
    }

    return true;
}

// MSL 자원 배정(SSD39)이다.
bool AssignMslBindings(const SpirvCrossFunctions& cross, spvc_compiler compiler, uint32 stage,
                       Span<const SpirvMslBinding> bindings) noexcept {
    using namespace tool_calls_detail;
    using namespace cross_binding_detail;
    using Index = SpirvCrossFunctions;
    const auto init = ToolFunction<decltype(&spvc_msl_resource_binding_init_2)>(
        cross, Index::MslResourceBindingInit2);
    const auto add = ToolFunction<decltype(&spvc_compiler_msl_add_resource_binding_2)>(
        cross, Index::CompilerMslAddResourceBinding2);
    if (init == nullptr || add == nullptr) {
        return false;
    }

    for (const SpirvMslBinding& entry : bindings) {
        spvc_msl_resource_binding_2 binding;
        init(&binding);
        binding.stage = ExecutionModel(stage);
        binding.desc_set =
            entry.set == SpirvMslPushConstantSet ? SPVC_MSL_PUSH_CONSTANT_DESC_SET : entry.set;
        binding.binding =
            entry.set == SpirvMslPushConstantSet ? SPVC_MSL_PUSH_CONSTANT_BINDING : entry.binding;
        binding.count = 1;
        binding.msl_buffer = entry.buffer;
        binding.msl_texture = entry.texture;
        binding.msl_sampler = entry.sampler;
        if (add(compiler, &binding) != SPVC_SUCCESS) {
            return false;
        }
    }

    return true;
}

// ESSL binding·결합 sampler 이름(SSD42)이다. 결합 sampler를 만든 뒤 부른다.
bool AssignGlslBindings(const SpirvCrossFunctions& cross, spvc_compiler compiler,
                        const SpirvCrossRequest& request) noexcept {
    using namespace tool_calls_detail;
    using Index = SpirvCrossFunctions;
    const auto createResources = ToolFunction<decltype(&spvc_compiler_create_shader_resources)>(
        cross, Index::CompilerCreateShaderResources);
    const auto listFor = ToolFunction<decltype(&spvc_resources_get_resource_list_for_type)>(
        cross, Index::ResourcesGetResourceListForType);
    const auto decoration =
        ToolFunction<decltype(&spvc_compiler_get_decoration)>(cross, Index::CompilerGetDecoration);
    const auto setDecoration =
        ToolFunction<decltype(&spvc_compiler_set_decoration)>(cross, Index::CompilerSetDecoration);
    const auto setName =
        ToolFunction<decltype(&spvc_compiler_set_name)>(cross, Index::CompilerSetName);
    const auto combined = ToolFunction<decltype(&spvc_compiler_get_combined_image_samplers)>(
        cross, Index::CompilerGetCombinedImageSamplers);
    const auto nameOf =
        ToolFunction<decltype(&spvc_compiler_get_name)>(cross, Index::CompilerGetName);
    if (createResources == nullptr || listFor == nullptr || setDecoration == nullptr ||
        setName == nullptr || combined == nullptr || nameOf == nullptr) {
        return false;
    }

    if (!request.glslBindings.IsEmpty()) {
        spvc_resources resources = nullptr;
        if (createResources(compiler, &resources) != SPVC_SUCCESS) {
            return false;
        }

        for (const spvc_resource_type type :
             {SPVC_RESOURCE_TYPE_STORAGE_BUFFER, SPVC_RESOURCE_TYPE_STORAGE_IMAGE,
              SPVC_RESOURCE_TYPE_SEPARATE_IMAGE, SPVC_RESOURCE_TYPE_SAMPLED_IMAGE}) {
            const spvc_reflected_resource* list = nullptr;
            size_t size = 0;
            if (listFor(resources, type, &list, &size) != SPVC_SUCCESS) {
                return false;
            }

            for (size_t index = 0; index < size; ++index) {
                const unsigned set = decoration(
                    compiler, list[index].id,
                    static_cast<SpvDecoration>(cross_binding_detail::DecorationDescriptorSet));
                const unsigned binding =
                    decoration(compiler, list[index].id,
                               static_cast<SpvDecoration>(cross_binding_detail::DecorationBinding));
                for (const SpirvGlslBinding& entry : request.glslBindings) {
                    if (entry.set == set && entry.binding == binding) {
                        setDecoration(
                            compiler, list[index].id,
                            static_cast<SpvDecoration>(cross_binding_detail::DecorationBinding),
                            entry.glBinding);
                    }
                }
            }
        }
    }

    const spvc_combined_image_sampler* pairs = nullptr;
    size_t count = 0;
    if (combined(compiler, &pairs, &count) != SPVC_SUCCESS) {
        return false;
    }

    for (size_t index = 0; index < count; ++index) {
        const char* texture = nameOf(compiler, pairs[index].image_id);
        const char* sampler = nameOf(compiler, pairs[index].sampler_id);
        for (const SpirvGlslCombined& entry : request.glslCombined) {
            if (texture != nullptr && sampler != nullptr && entry.texture == StringView(texture) &&
                entry.sampler == StringView(sampler)) {
                // set_name은 NUL 종료 문자열을 받는다(이름 버퍼는 호출자가 NUL로 끝낸다).
                setName(compiler, pairs[index].combined_id, entry.name.Data());
                setDecoration(compiler, pairs[index].combined_id,
                              static_cast<SpvDecoration>(cross_binding_detail::DecorationBinding),
                              entry.unit);
            }
        }
    }

    return true;
}

} // namespace cross_binding_detail

ShaderToolOutcome QuerySpirvSamplerUses(const SpirvCrossFunctions& cross, Span<const uint8> spirv,
                                        Array<SpirvSamplerUse>& out, String& diagnostics) noexcept {
    using namespace tool_calls_detail;
    using Index = SpirvCrossFunctions;
    out.Clear();
    const auto contextCreate =
        ToolFunction<decltype(&spvc_context_create)>(cross, Index::ContextCreate);
    const auto contextDestroy =
        ToolFunction<decltype(&spvc_context_destroy)>(cross, Index::ContextDestroy);
    const auto parse =
        ToolFunction<decltype(&spvc_context_parse_spirv)>(cross, Index::ContextParseSpirv);
    const auto createCompiler =
        ToolFunction<decltype(&spvc_context_create_compiler)>(cross, Index::ContextCreateCompiler);
    const auto combine = ToolFunction<decltype(&spvc_compiler_build_combined_image_samplers)>(
        cross, Index::CompilerBuildCombinedImageSamplers);
    const auto combined = ToolFunction<decltype(&spvc_compiler_get_combined_image_samplers)>(
        cross, Index::CompilerGetCombinedImageSamplers);
    const auto nameOf =
        ToolFunction<decltype(&spvc_compiler_get_name)>(cross, Index::CompilerGetName);
    if (contextCreate == nullptr || combined == nullptr || nameOf == nullptr ||
        spirv.Size() % SpirvWordBytes != 0 || spirv.IsEmpty()) {
        Note(diagnostics, "SPIRV-Cross is not available or the module is not word aligned");
        return ShaderToolOutcome::ToolError;
    }

    spvc_context context = nullptr;
    if (contextCreate(&context) != SPVC_SUCCESS) {
        Note(diagnostics, "spvc_context_create failed");
        return ShaderToolOutcome::ToolError;
    }

    spvc_parsed_ir ir = nullptr;
    spvc_compiler compiler = nullptr;
    const spvc_combined_image_sampler* pairs = nullptr;
    size_t pairCount = 0;
    bool ok = parse(context, reinterpret_cast<const SpvId*>(spirv.Data()),
                    spirv.Size() / SpirvWordBytes, &ir) == SPVC_SUCCESS &&
              createCompiler(context, SPVC_BACKEND_GLSL, ir, SPVC_CAPTURE_MODE_TAKE_OWNERSHIP,
                             &compiler) == SPVC_SUCCESS &&
              combine(compiler) == SPVC_SUCCESS &&
              combined(compiler, &pairs, &pairCount) == SPVC_SUCCESS;
    for (size_t index = 0; ok && index < pairCount; ++index) {
        const char* texture = nameOf(compiler, pairs[index].image_id);
        const char* sampler = nameOf(compiler, pairs[index].sampler_id);
        auto use = out.TryEmplaceBack(out.GetAllocator());
        ok = use && use.Value()->texture.TryAssign(StringView(texture != nullptr ? texture : "")) &&
             use.Value()->sampler.TryAssign(StringView(sampler != nullptr ? sampler : ""));
    }

    if (!ok) {
        Note(diagnostics, "SPIRV-Cross could not list combined image samplers");
    }

    contextDestroy(context);
    return ok ? ShaderToolOutcome::Success : ShaderToolOutcome::ToolError;
}

ShaderToolOutcome CrossCompileSpirv(const SpirvCrossFunctions& cross,
                                    const SpirvCrossRequest& request, String& text,
                                    String& diagnostics) noexcept {
    using namespace tool_calls_detail;
    using namespace cross_binding_detail;
    using Index = SpirvCrossFunctions;
    text.Clear();
    diagnostics.Clear();
    const auto contextCreate =
        ToolFunction<decltype(&spvc_context_create)>(cross, Index::ContextCreate);
    const auto contextDestroy =
        ToolFunction<decltype(&spvc_context_destroy)>(cross, Index::ContextDestroy);
    const auto lastError = ToolFunction<decltype(&spvc_context_get_last_error_string)>(
        cross, Index::ContextGetLastErrorString);
    const auto parse =
        ToolFunction<decltype(&spvc_context_parse_spirv)>(cross, Index::ContextParseSpirv);
    const auto createCompiler =
        ToolFunction<decltype(&spvc_context_create_compiler)>(cross, Index::ContextCreateCompiler);
    const auto createOptions = ToolFunction<decltype(&spvc_compiler_create_compiler_options)>(
        cross, Index::CompilerCreateCompilerOptions);
    const auto setBool = ToolFunction<decltype(&spvc_compiler_options_set_bool)>(
        cross, Index::CompilerOptionsSetBool);
    const auto setUint = ToolFunction<decltype(&spvc_compiler_options_set_uint)>(
        cross, Index::CompilerOptionsSetUint);
    const auto install = ToolFunction<decltype(&spvc_compiler_install_compiler_options)>(
        cross, Index::CompilerInstallCompilerOptions);
    const auto combine = ToolFunction<decltype(&spvc_compiler_build_combined_image_samplers)>(
        cross, Index::CompilerBuildCombinedImageSamplers);
    const auto compile =
        ToolFunction<decltype(&spvc_compiler_compile)>(cross, Index::CompilerCompile);
    if (contextCreate == nullptr || compile == nullptr) {
        Note(diagnostics, "SPIRV-Cross is not available");
        return ShaderToolOutcome::ToolError;
    }

    if (request.spirv.Size() % SpirvWordBytes != 0 || request.spirv.IsEmpty()) {
        Note(diagnostics, "the SPIR-V module is not word aligned");
        return ShaderToolOutcome::ToolError;
    }

    spvc_context context = nullptr;
    if (contextCreate(&context) != SPVC_SUCCESS) {
        Note(diagnostics, "spvc_context_create failed");
        return ShaderToolOutcome::ToolError;
    }

    spvc_backend backend = SPVC_BACKEND_GLSL;
    if (request.target == SpirvCrossTarget::Msl) {
        backend = SPVC_BACKEND_MSL;
    } else if (request.target == SpirvCrossTarget::Hlsl50) {
        backend = SPVC_BACKEND_HLSL;
    }

    spvc_parsed_ir ir = nullptr;
    spvc_compiler compiler = nullptr;
    spvc_compiler_options options = nullptr;
    const auto* words = reinterpret_cast<const SpvId*>(request.spirv.Data());
    bool ok = parse(context, words, request.spirv.Size() / SpirvWordBytes, &ir) == SPVC_SUCCESS &&
              createCompiler(context, backend, ir, SPVC_CAPTURE_MODE_TAKE_OWNERSHIP, &compiler) ==
                  SPVC_SUCCESS &&
              createOptions(compiler, &options) == SPVC_SUCCESS;
    if (ok) {
        switch (request.target) {
        case SpirvCrossTarget::Essl:
            ok = setUint(options, SPVC_COMPILER_OPTION_GLSL_VERSION, request.esslVersion) ==
                     SPVC_SUCCESS &&
                 setBool(options, SPVC_COMPILER_OPTION_GLSL_ES, SPVC_TRUE) == SPVC_SUCCESS &&
                 setBool(options, SPVC_COMPILER_OPTION_GLSL_ES_DEFAULT_FLOAT_PRECISION_HIGHP,
                         SPVC_TRUE) == SPVC_SUCCESS &&
                 setBool(options, SPVC_COMPILER_OPTION_FIXUP_DEPTH_CONVENTION,
                         request.fixupClipSpace ? SPVC_TRUE : SPVC_FALSE) == SPVC_SUCCESS &&
                 setBool(options, SPVC_COMPILER_OPTION_FLIP_VERTEX_Y,
                         request.flipVertexY ? SPVC_TRUE : SPVC_FALSE) == SPVC_SUCCESS;
            break;
        case SpirvCrossTarget::Msl:
            ok = setUint(options, SPVC_COMPILER_OPTION_MSL_VERSION,
                         SPVC_MAKE_MSL_VERSION(request.mslMajor, request.mslMinor, 0)) ==
                     SPVC_SUCCESS &&
                 setUint(options, SPVC_COMPILER_OPTION_MSL_PLATFORM, SPVC_MSL_PLATFORM_MACOS) ==
                     SPVC_SUCCESS &&
                 setBool(options, SPVC_COMPILER_OPTION_MSL_TEXTURE_BUFFER_NATIVE, SPVC_TRUE) ==
                     SPVC_SUCCESS;
            break;
        case SpirvCrossTarget::Hlsl50:
            ok = setUint(options, SPVC_COMPILER_OPTION_HLSL_SHADER_MODEL, Hlsl50ShaderModel) ==
                 SPVC_SUCCESS;
            break;
        }
    }

    if (ok && request.target == SpirvCrossTarget::Essl && request.glslPlainUniforms) {
        ok = setBool(options, SPVC_COMPILER_OPTION_GLSL_EMIT_UNIFORM_BUFFER_AS_PLAIN_UNIFORMS,
                     SPVC_TRUE) == SPVC_SUCCESS;
    }

    if (ok && request.target == SpirvCrossTarget::Msl &&
        request.mslBufferSizeIndex != SpirvMslNoIndex) {
        ok = setUint(options, SPVC_COMPILER_OPTION_MSL_BUFFER_SIZE_BUFFER_INDEX,
                     request.mslBufferSizeIndex) == SPVC_SUCCESS;
    }

    ok = ok && install(compiler, options) == SPVC_SUCCESS;
    // char 배열은 compile이 끝날 때까지 살아 있어야 한다(SPIRV-Cross가 시맨틱 포인터를 들고 있다).
    char semantics[MaxVertexSemantics][VertexSemanticBytes] = {};
    if (ok && request.target == SpirvCrossTarget::Hlsl50 && request.hlslAssignRegisters) {
        ok = AssignHlslRegisters(cross, compiler, request.stage, semantics);
    }

    if (ok && request.target == SpirvCrossTarget::Msl && !request.mslBindings.IsEmpty()) {
        ok = AssignMslBindings(cross, compiler, request.stage, request.mslBindings);
    }
    // ESSL에는 분리 texture·sampler가 없으므로 결합 sampler를 만든다(UE GL 경로와 같다).
    if (ok && backend == SPVC_BACKEND_GLSL) {
        ok = combine(compiler) == SPVC_SUCCESS;
    }

    if (ok && backend == SPVC_BACKEND_GLSL &&
        (!request.glslBindings.IsEmpty() || !request.glslCombined.IsEmpty())) {
        ok = AssignGlslBindings(cross, compiler, request);
    }

    const char* source = nullptr;
    ok = ok && compile(compiler, &source) == SPVC_SUCCESS && source != nullptr;
    ShaderToolOutcome outcome = ShaderToolOutcome::Success;
    if (ok) {
        if (!text.TryAppend(StringView(source))) {
            Note(diagnostics, "out of memory");
            outcome = ShaderToolOutcome::ToolError;
        }
    } else {
        const char* message = lastError(context);
        Note(diagnostics, "SPIRV-Cross failed",
             StringView(message != nullptr ? message : "unknown error"));
        outcome = ShaderToolOutcome::CompileError;
    }

    contextDestroy(context);
    return outcome;
}

} // namespace rocks::shader
