#include <RenderBase/CookedShaderFormat.h>

#include <Core/Move.h>
#include <stdio.h>
#include <string.h>

// 쿠킹 라이브러리의 런타임 형식 선택(Docs/Design/ShaderSystem 16.1절 SSD47)이다. 규칙은 공개 헤더
// 주석을 따른다.
namespace rocks::render {

namespace cooked_format_detail {

// 한도 하나가 장치 값 안인가(장치 값이 NotApplicableLimit면 그 한도로 판단하지 않는다).
bool Within(uint32 required, uint32 device) noexcept {
    return device == graphics::NotApplicableLimit || required <= device;
}

const char* CheckStage(const shader::ShaderFormatLimits& limits, uint32 stageBit,
                       const graphics::StageResourceLimits& stage, uint32 uniformVectors) noexcept {
    if (limits.constantBuffers > stage.constantBuffers) {
        return "constant buffers exceed the device stage limit";
    }

    if (limits.sampledTextures > stage.sampledTextures) {
        return "sampled textures exceed the device stage limit";
    }

    if (limits.samplers > stage.samplers) {
        return "samplers exceed the device stage limit";
    }

    // storage는 쓰기 단계(uavStages)만 형식 상한이 그 단계 요구다(VS·PS 읽기 storage는 형식
    // 거르기가 정한다 — GLES 모바일 상한).
    if ((limits.uavStages & stageBit) != 0) {
        if (limits.storageBuffers > stage.storageBuffers) {
            return "storage buffers exceed the device stage limit";
        }

        if (limits.storageImages > stage.storageImages) {
            return "storage images exceed the device stage limit";
        }
    }

    if (uniformVectors != shader::NoShaderLimit && !Within(uniformVectors, stage.uniformVectors)) {
        return "emulated uniform vectors exceed the device stage limit";
    }

    return nullptr;
}

void AppendProblem(char (&out)[CookedShaderFormatProblemBytes], StringView format,
                   const char* reason) noexcept {
    const usize used = strlen(out);
    if (used + 1 >= sizeof(out)) {
        return;
    }

    (void)snprintf(out + used, sizeof(out) - used, "%s%.*s: %s", used == 0 ? "" : "; ",
                   static_cast<int>(format.ByteCount()), format.Data(), reason);
}

} // namespace cooked_format_detail

const char* CheckShaderFormatFitsDevice(const shader::ShaderFormatFacts& facts,
                                        graphics::Backend backend,
                                        const graphics::DeviceCaps& caps) noexcept {
    using namespace cooked_format_detail;
    const shader::ShaderFormatLimits& limits = facts.limits;
    const graphics::DeviceLimits& device = caps.limits;
    const struct {
        uint32 bit;
        const graphics::StageResourceLimits* stage;
        uint32 uniformVectors;
    } stages[] = {{shader::ShaderStageVertex, &device.vertex, limits.uniformVectors[0]},
                  {shader::ShaderStagePixel, &device.pixel, limits.uniformVectors[1]},
                  {shader::ShaderStageCompute, &device.compute, limits.uniformVectors[2]}};
    for (const auto& stage : stages) {
        if (const char* reason =
                CheckStage(limits, stage.bit, *stage.stage, stage.uniformVectors)) {
            return reason;
        }
    }

    if (limits.renderTargets > device.colorAttachments) {
        return "render targets exceed the device color attachments";
    }

    if (limits.interStageVectors > device.interStageVectors) {
        return "inter-stage vectors exceed the device limit";
    }

    if (limits.rootConstantBytes > device.rootConstantBytes) {
        return "root constant bytes exceed the device limit";
    }

    if (limits.uavTotal != shader::NoShaderLimit && !Within(limits.uavTotal, device.pipelineUavs)) {
        return "pipeline UAVs exceed the device limit";
    }

    if (backend == graphics::Backend::Vulkan) {
        const uint64 pixelResources = static_cast<uint64>(limits.constantBuffers) +
                                      limits.sampledTextures + limits.storageBuffers +
                                      limits.storageImages + limits.renderTargets;
        if (device.perStageResources != graphics::NotApplicableLimit &&
            pixelResources > device.perStageResources) {
            return "pixel stage resources exceed maxPerStageResources";
        }
    }

    if ((facts.deviceRequirements & ~caps.features.shaderDeviceRequirements) != 0) {
        return "the device does not meet the format device requirements";
    }

    return nullptr;
}

Result<void> SelectCookedShaderFormat(CookedShaderLibrarySource& source,
                                      const graphics::Device& device,
                                      shader::ShaderLibraryKind library,
                                      CookedShaderFormatChoice& out) noexcept {
    using namespace cooked_format_detail;
    out.formatId[0] = '\0';
    out.problem[0] = '\0';
    out.bytes.Clear();
    if (source.Backends().IsEmpty()) {
        snprintf(out.problem, sizeof(out.problem), "this cook has no shader formats");
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    const graphics::Backend backend = device.Info().backend;
    bool cooked = false;
    for (const graphics::Backend candidate : source.Backends()) {
        cooked = cooked || candidate == backend;
    }

    if (!cooked) {
        snprintf(out.problem, sizeof(out.problem), "the device backend is not in this cook");
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    const graphics::DeviceCaps& caps = device.Caps();
    for (const StringView format : source.Formats(backend)) {
        Array<uint8> bytes(out.bytes.GetAllocator());
        if (auto read = source.ReadLibrary(format, library, bytes); !read) {
            if (read.Error() == ErrorCode::NotFound) {
                AppendProblem(out.problem, format, "no library of this kind in the cook");
                continue;
            }

            const StringView problem = source.Problem();
            snprintf(out.problem, sizeof(out.problem), "%.*s: %.*s",
                     static_cast<int>(format.ByteCount()), format.Data(),
                     static_cast<int>(problem.ByteCount()), problem.Data());
            return read;
        }

        shader::ShaderLibraryView view;
        if (const shader::ShaderReadStatus opened = view.Open(bytes.View()); !opened) {
            snprintf(out.problem, sizeof(out.problem), "%.*s: the library header is corrupt: %s",
                     static_cast<int>(format.ByteCount()), format.Data(), opened.reason);
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        if (const char* reason = CheckShaderFormatFitsDevice(view.Facts(), backend, caps)) {
            AppendProblem(out.problem, format, reason);
            continue;
        }

        if (format.ByteCount() + 1 > sizeof(out.formatId)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        memcpy(out.formatId, format.Data(), format.ByteCount());
        out.formatId[format.ByteCount()] = '\0';
        out.bytes = Move(bytes);
        out.problem[0] = '\0';
        return Result<void>::Success();
    }

    if (out.problem[0] == '\0') {
        snprintf(out.problem, sizeof(out.problem), "the cook lists no formats for the backend");
    }

    return Result<void>::Failure(ErrorCode::Unsupported);
}

} // namespace rocks::render
