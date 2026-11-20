#include "SpikeTools.h"

#include <spirv-tools/libspirv.h>
#include <spirv_cross_c.h>
#include <stdio.h>
#include <string.h>

// SPIRV-Tools(검증·최적화)와 SPIRV-Cross(MSL·ESSL·HLSL 5.0) 호출이다. 두 도구는 C API라 OS 헤더
// 없이 공통 코드에 둔다. 함수 주소는 ShaderToolSet의 함수 표에서 공개 헤더 타입으로 바꿔 부른다.
namespace rocks::spike {

namespace {

constexpr usize SpirvWordBytes = 4;
// SPIR-V 머리는 5 word다(magic·version·generator·bound·schema).
constexpr usize SpirvHeaderWords = 5;
// OpExtInst opcode다(SPIR-V 사양 3.49.10).
constexpr uint32 OpExtInstCode = 12;
// OpExtension·OpCapability opcode다(SPIR-V 사양 3.49.10·3.49.19).
constexpr uint32 OpExtensionCode = 10;
constexpr uint32 OpCapabilityCode = 17;
// uint32 10진 표기 버퍼 크기다(10자리 + NUL).
constexpr usize UInt32DecimalBytes = 11;
constexpr uint32 OpcodeMask = 0xffffu;
constexpr uint32 WordCountShift = 16;

// ESSL 버전 320(`#version 320 es`, 2절 GLES 형식).
constexpr uint32 EsslVersion = 320;
// D3D11 대체 경로의 HLSL 셰이더 모델 5.0.
constexpr uint32 HlslShaderModel50 = 50;

template <typename Function>
Function Get(const shader::SpirvToolsFunctions& tools, shader::SpirvToolsFunctions::Index index) {
    return reinterpret_cast<Function>(tools.address[index]);
}

template <typename Function>
Function Get(const shader::SpirvCrossFunctions& cross, shader::SpirvCrossFunctions::Index index) {
    return reinterpret_cast<Function>(cross.address[index]);
}

bool AsWords(Span<const uint8> code, const uint32*& words, usize& count) noexcept {
    if (code.Size() % SpirvWordBytes != 0 || code.Size() < SpirvHeaderWords * SpirvWordBytes) {
        return false;
    }

    words = reinterpret_cast<const uint32*>(code.Data());
    count = code.Size() / SpirvWordBytes;
    return true;
}

} // namespace

bool ValidateSpirv(const shader::SpirvToolsFunctions& tools, Span<const uint8> code,
                   StringBuilderBase& diagnostics) noexcept {
    using Index = shader::SpirvToolsFunctions;
    const uint32* words = nullptr;
    usize count = 0;
    if (!AsWords(code, words, count)) {
        diagnostics.Append(StringView("not a SPIR-V word stream"));
        return false;
    }

    const auto contextCreate = Get<decltype(&spvContextCreate)>(tools, Index::ContextCreate);
    const auto contextDestroy = Get<decltype(&spvContextDestroy)>(tools, Index::ContextDestroy);
    const auto optionsCreate =
        Get<decltype(&spvValidatorOptionsCreate)>(tools, Index::ValidatorOptionsCreate);
    const auto optionsDestroy =
        Get<decltype(&spvValidatorOptionsDestroy)>(tools, Index::ValidatorOptionsDestroy);
    const auto validate = Get<decltype(&spvValidateWithOptions)>(tools, Index::ValidateWithOptions);
    const auto diagnosticDestroy =
        Get<decltype(&spvDiagnosticDestroy)>(tools, Index::DiagnosticDestroy);

    spv_context context = contextCreate(SPV_ENV_VULKAN_1_1);
    spv_validator_options options = optionsCreate();
    spv_const_binary_t binary{words, count};
    spv_diagnostic diagnostic = nullptr;
    const spv_result_t result = validate(context, options, &binary, &diagnostic);
    if (diagnostic != nullptr && diagnostic->error != nullptr) {
        diagnostics.Append(StringView(diagnostic->error));
    }

    diagnosticDestroy(diagnostic);
    optionsDestroy(options);
    contextDestroy(context);
    return result == SPV_SUCCESS;
}

bool OptimizeSpirv(const shader::SpirvToolsFunctions& tools, Span<const uint8> code, ByteArray& out,
                   StringBuilderBase& diagnostics) noexcept {
    using Index = shader::SpirvToolsFunctions;
    const uint32* words = nullptr;
    usize count = 0;
    if (!AsWords(code, words, count)) {
        diagnostics.Append(StringView("not a SPIR-V word stream"));
        return false;
    }

    const auto create = Get<decltype(&spvOptimizerCreate)>(tools, Index::OptimizerCreate);
    const auto destroy = Get<decltype(&spvOptimizerDestroy)>(tools, Index::OptimizerDestroy);
    const auto registerPasses = Get<decltype(&spvOptimizerRegisterPerformancePasses)>(
        tools, Index::OptimizerRegisterPerformancePasses);
    const auto optionsCreate =
        Get<decltype(&spvOptimizerOptionsCreate)>(tools, Index::OptimizerOptionsCreate);
    const auto optionsDestroy =
        Get<decltype(&spvOptimizerOptionsDestroy)>(tools, Index::OptimizerOptionsDestroy);
    const auto run = Get<decltype(&spvOptimizerRun)>(tools, Index::OptimizerRun);
    const auto binaryDestroy = Get<decltype(&spvBinaryDestroy)>(tools, Index::BinaryDestroy);

    spv_optimizer_t* optimizer = create(SPV_ENV_VULKAN_1_1);
    registerPasses(optimizer);
    spv_optimizer_options options = optionsCreate();
    spv_binary binary = nullptr;
    const spv_result_t result = run(optimizer, words, count, &binary, options);
    bool copied = false;
    if (result == SPV_SUCCESS && binary != nullptr) {
        copied = static_cast<bool>(out.TryResize(binary->wordCount * SpirvWordBytes));
        if (copied) {
            memcpy(out.Data(), binary->code, out.Size());
        }
    } else {
        diagnostics.Append(StringView("spvOptimizerRun failed"));
    }

    binaryDestroy(binary);
    optionsDestroy(options);
    destroy(optimizer);
    return copied;
}

const char* CrossTargetName(CrossTarget target) noexcept {
    switch (target) {
    case CrossTarget::EsslAndroid:
        return "gles31_android";
    case CrossTarget::EsslPc:
        return "gles31_pc";
    case CrossTarget::Msl:
        return "metal_sm5";
    case CrossTarget::Hlsl50:
        return "hlsl50";
    }

    return "unknown";
}

bool CrossCompile(const shader::SpirvCrossFunctions& cross, Span<const uint8> code,
                  CrossTarget target, StringBuilderBase& text, StringBuilderBase& diagnostics,
                  Report* combinedSamplers) noexcept {
    using Index = shader::SpirvCrossFunctions;
    text.Clear();
    const uint32* words = nullptr;
    usize count = 0;
    if (!AsWords(code, words, count)) {
        diagnostics.Append(StringView("not a SPIR-V word stream"));
        return false;
    }

    const auto contextCreate = Get<decltype(&spvc_context_create)>(cross, Index::ContextCreate);
    const auto contextDestroy = Get<decltype(&spvc_context_destroy)>(cross, Index::ContextDestroy);
    const auto lastError =
        Get<decltype(&spvc_context_get_last_error_string)>(cross, Index::ContextGetLastErrorString);
    const auto parse = Get<decltype(&spvc_context_parse_spirv)>(cross, Index::ContextParseSpirv);
    const auto createCompiler =
        Get<decltype(&spvc_context_create_compiler)>(cross, Index::ContextCreateCompiler);
    const auto createOptions = Get<decltype(&spvc_compiler_create_compiler_options)>(
        cross, Index::CompilerCreateCompilerOptions);
    const auto setBool =
        Get<decltype(&spvc_compiler_options_set_bool)>(cross, Index::CompilerOptionsSetBool);
    const auto setUint =
        Get<decltype(&spvc_compiler_options_set_uint)>(cross, Index::CompilerOptionsSetUint);
    const auto install = Get<decltype(&spvc_compiler_install_compiler_options)>(
        cross, Index::CompilerInstallCompilerOptions);
    const auto combine = Get<decltype(&spvc_compiler_build_combined_image_samplers)>(
        cross, Index::CompilerBuildCombinedImageSamplers);
    const auto compile = Get<decltype(&spvc_compiler_compile)>(cross, Index::CompilerCompile);

    spvc_context context = nullptr;
    if (contextCreate(&context) != SPVC_SUCCESS) {
        diagnostics.Append(StringView("spvc_context_create failed"));
        return false;
    }

    spvc_backend backend = SPVC_BACKEND_GLSL;
    if (target == CrossTarget::Msl) {
        backend = SPVC_BACKEND_MSL;
    } else if (target == CrossTarget::Hlsl50) {
        backend = SPVC_BACKEND_HLSL;
    }

    spvc_parsed_ir ir = nullptr;
    spvc_compiler compiler = nullptr;
    spvc_compiler_options options = nullptr;
    bool ok = parse(context, words, count, &ir) == SPVC_SUCCESS &&
              createCompiler(context, backend, ir, SPVC_CAPTURE_MODE_TAKE_OWNERSHIP, &compiler) ==
                  SPVC_SUCCESS &&
              createOptions(compiler, &options) == SPVC_SUCCESS;
    if (ok) {
        switch (target) {
        case CrossTarget::EsslAndroid:
        case CrossTarget::EsslPc: {
            const bool android = target == CrossTarget::EsslAndroid;
            ok = setUint(options, SPVC_COMPILER_OPTION_GLSL_VERSION, EsslVersion) == SPVC_SUCCESS &&
                 setBool(options, SPVC_COMPILER_OPTION_GLSL_ES, SPVC_TRUE) == SPVC_SUCCESS &&
                 setBool(options, SPVC_COMPILER_OPTION_GLSL_ES_DEFAULT_FLOAT_PRECISION_HIGHP,
                         SPVC_TRUE) == SPVC_SUCCESS &&
                 setBool(options, SPVC_COMPILER_OPTION_FIXUP_DEPTH_CONVENTION,
                         android ? SPVC_TRUE : SPVC_FALSE) == SPVC_SUCCESS &&
                 setBool(options, SPVC_COMPILER_OPTION_FLIP_VERTEX_Y,
                         android ? SPVC_TRUE : SPVC_FALSE) == SPVC_SUCCESS;
            break;
        }

        case CrossTarget::Msl:
            // typed buffer는 MSL texture_buffer로 낸다. 끄면(SPIRV-Cross 기본값) texture2d와
            // 좌표 변환 함수로 흉내 낸다(측정 S13, 7.2절 4항 Metal, UE 5.8도 켠다).
            ok = setUint(options, SPVC_COMPILER_OPTION_MSL_VERSION,
                         SPVC_MAKE_MSL_VERSION(2, 1, 0)) == SPVC_SUCCESS &&
                 setUint(options, SPVC_COMPILER_OPTION_MSL_PLATFORM, SPVC_MSL_PLATFORM_MACOS) ==
                     SPVC_SUCCESS &&
                 setBool(options, SPVC_COMPILER_OPTION_MSL_TEXTURE_BUFFER_NATIVE, SPVC_TRUE) ==
                     SPVC_SUCCESS;
            break;
        case CrossTarget::Hlsl50:
            ok = setUint(options, SPVC_COMPILER_OPTION_HLSL_SHADER_MODEL, HlslShaderModel50) ==
                 SPVC_SUCCESS;
            break;
        }
    }

    ok = ok && install(compiler, options) == SPVC_SUCCESS;
    // ESSL에는 분리 texture·sampler가 없으므로 결합 sampler를 만든다(UE도 GL 경로에서 같은 처리를
    // 한다).
    if (ok && backend == SPVC_BACKEND_GLSL) {
        ok = combine(compiler) == SPVC_SUCCESS;
    }

    const char* source = nullptr;
    ok = ok && compile(compiler, &source) == SPVC_SUCCESS && source != nullptr;
    if (ok) {
        text.Append(StringView(source));
        ok = !text.HasFailed();
    } else {
        const char* message = lastError(context);
        diagnostics.Append(StringView(message != nullptr ? message : "SPIRV-Cross failed"));
    }

    // 결합 목록은 compile 뒤에 읽는다(결합 변수 이름은 compile이 정한다).
    if (ok && combinedSamplers != nullptr && backend == SPVC_BACKEND_GLSL) {
        const auto getCombined = Get<decltype(&spvc_compiler_get_combined_image_samplers)>(
            cross, Index::CompilerGetCombinedImageSamplers);
        const auto getName = Get<decltype(&spvc_compiler_get_name)>(cross, Index::CompilerGetName);
        const spvc_combined_image_sampler* samplers = nullptr;
        size_t samplerCount = 0;
        ok = getCombined(compiler, &samplers, &samplerCount) == SPVC_SUCCESS;
        combinedSamplers->BeginArray("combinedSamplers");
        for (size_t index = 0; ok && index < samplerCount; ++index) {
            combinedSamplers->BeginObject();
            combinedSamplers->String("combined",
                                     StringView(getName(compiler, samplers[index].combined_id)));
            combinedSamplers->String("image",
                                     StringView(getName(compiler, samplers[index].image_id)));
            combinedSamplers->String("sampler",
                                     StringView(getName(compiler, samplers[index].sampler_id)));
            combinedSamplers->EndObject();
        }

        combinedSamplers->EndArray();
    }

    contextDestroy(context);
    return ok;
}

bool ReportSpirvFeatures(Span<const uint8> code, uint32 capability, StringView extension,
                         Report& report, SpirvFeatureMatch& match) noexcept {
    match = SpirvFeatureMatch{};
    const uint32* words = nullptr;
    usize count = 0;
    if (!AsWords(code, words, count)) {
        return false;
    }

    report.BeginArray("capabilities");
    char number[UInt32DecimalBytes];
    usize index = SpirvHeaderWords;
    while (index < count) {
        const uint32 instruction = words[index];
        const uint32 length = instruction >> WordCountShift;
        if (length == 0 || index + length > count) {
            break;
        }

        const uint32 opcode = instruction & OpcodeMask;
        if (opcode == OpCapabilityCode && length > 1) {
            snprintf(number, sizeof(number), "%u", words[index + 1]);
            report.Element(StringView(number));
            match.capability = match.capability || words[index + 1] == capability;
        }

        index += length;
    }

    report.EndArray();
    report.BeginArray("extensions");
    index = SpirvHeaderWords;
    while (index < count) {
        const uint32 instruction = words[index];
        const uint32 length = instruction >> WordCountShift;
        if (length == 0 || index + length > count) {
            break;
        }

        if ((instruction & OpcodeMask) == OpExtensionCode && length > 1) {
            // 이름은 NUL 종료 literal string이고 word 안에 little-endian으로 담긴다.
            const char* name = reinterpret_cast<const char*>(words + index + 1);
            const usize limit = (length - 1) * SpirvWordBytes;
            const StringView text(name, strnlen(name, limit));
            report.Element(text);
            match.extension = match.extension || text == extension;
        }

        index += length;
    }

    report.EndArray();
    return true;
}

usize CountExtInst(Span<const uint8> code) noexcept {
    const uint32* words = nullptr;
    usize count = 0;
    if (!AsWords(code, words, count)) {
        return 0;
    }

    usize found = 0;
    usize index = SpirvHeaderWords;
    while (index < count) {
        const uint32 instruction = words[index];
        const uint32 length = instruction >> WordCountShift;
        if ((instruction & OpcodeMask) == OpExtInstCode) {
            ++found;
        }

        if (length == 0) {
            break;
        }

        index += length;
    }

    return found;
}

} // namespace rocks::spike
