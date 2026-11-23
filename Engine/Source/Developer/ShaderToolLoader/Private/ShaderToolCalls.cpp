#include <ShaderToolLoader/ShaderToolCalls.h>

#include <spirv-tools/libspirv.h>
#include <spirv_cross_c.h>

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

ShaderToolOutcome CrossCompileSpirv(const SpirvCrossFunctions& cross,
                                    const SpirvCrossRequest& request, String& text,
                                    String& diagnostics) noexcept {
    using namespace tool_calls_detail;
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

    ok = ok && install(compiler, options) == SPVC_SUCCESS;
    // ESSL에는 분리 texture·sampler가 없으므로 결합 sampler를 만든다(UE GL 경로와 같다).
    if (ok && backend == SPVC_BACKEND_GLSL) {
        ok = combine(compiler) == SPVC_SUCCESS;
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
