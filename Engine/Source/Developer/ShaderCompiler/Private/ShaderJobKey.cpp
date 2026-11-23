#include <ShaderCompiler/ShaderCompileOptions.generated.h>
#include <ShaderCompiler/ShaderJobKey.h>

#include <AssetNugget/NuggetReflected.h>
#include <Core/Containers/Array.h>
#include <Object/ObjectRegistry.h>
#include <Object/TypeRegistry.h>
#include <ShaderToolTable/ShaderToolTable.h>

// 셰이더 job 키(Docs/Design/ShaderSystem 10절)다. 기록 순서는 공개 헤더 주석의 1–7행이다.
namespace rocks::shader {

namespace job_key_detail {

Result<void> Fail(NuggetKeyWriter& writer, ErrorCode error) noexcept {
    writer.Fail(error);
    return Result<void>::Failure(error);
}

bool IsSingleStage(uint32 stage) noexcept {
    return stage != 0 && (stage & (stage - 1)) == 0;
}

// 출력에 관여하는 role이다(Symbols·Notice는 PDB·라이선스만 바뀌어도 모든 키가 바뀌지 않게 뺀다).
bool AffectsOutput(ShaderToolFileRole role) noexcept {
    return role == ShaderToolFileRole::Runtime || role == ShaderToolFileRole::Redist ||
           role == ShaderToolFileRole::Tool;
}

const ShaderSystemToolFingerprint* FindFingerprint(Span<const ShaderSystemToolFingerprint> tools,
                                                   StringView name) noexcept {
    for (const ShaderSystemToolFingerprint& tool : tools) {
        if (tool.name == name) {
            return &tool;
        }
    }

    return nullptr;
}

// 2행: 도구 목록(형식 정의 tools 순서).
Result<void> WriteTools(NuggetKeyWriter& writer, const ShaderJobKeyInput& input) noexcept {
    const ShaderFormatDefinition& format = *input.format;
    writer.WriteList(format.toolCount);
    Array<uint8> settings(*input.reflection.allocator);
    for (usize index = 0; index < format.toolCount; ++index) {
        const ShaderTool* tool = format.tools[index];
        if (tool == nullptr || tool->name == nullptr || tool->version == nullptr) {
            return Fail(writer, ErrorCode::InvalidArgument);
        }

        settings.Clear();
        StringView version(tool->version);
        if (tool->origin == ShaderToolOrigin::System) {
            const ShaderSystemToolFingerprint* fingerprint =
                FindFingerprint(input.systemTools, StringView(tool->name));
            if (fingerprint == nullptr) {
                return Fail(writer, ErrorCode::UnsupportedCapability);
            }

            version = fingerprint->version;
            for (const Sha256Digest& digest : fingerprint->executables) {
                if (!settings.TryAppend(Span<const uint8>(digest.bytes, Sha256Digest::Bytes))) {
                    return Fail(writer, ErrorCode::OutOfMemory);
                }
            }
        } else {
            // 이 호스트 파일이 없는 도구는 이 호스트에서 그 형식을 만들 수 없다.
            if (tool->fileCount == 0) {
                return Fail(writer, ErrorCode::UnsupportedCapability);
            }

            for (usize file = 0; file < tool->fileCount; ++file) {
                const ShaderToolFile& entry = tool->files[file];
                if (!AffectsOutput(entry.role) || !entry.hasDigest) {
                    continue;
                }

                if (!settings.TryAppend(
                        Span<const uint8>(entry.sha256.bytes, Sha256Digest::Bytes))) {
                    return Fail(writer, ErrorCode::OutOfMemory);
                }
            }
        }

        WriteNuggetEncoder(writer, StringView(tool->name), version, settings.View());
    }

    return writer.Error();
}

} // namespace job_key_detail

Result<void> WriteShaderJobKeyBody(NuggetKeyWriter& writer,
                                   const ShaderJobKeyInput& input) noexcept {
    using namespace job_key_detail;
    const ShaderKeyReflection& reflection = input.reflection;
    if (input.format == nullptr || input.format->state != ShaderFormatState::Supported ||
        !IsSingleStage(input.stage) || (input.format->stages & input.stage) == 0 ||
        input.entry.IsEmpty() || input.profileId.IsEmpty() || input.options == nullptr ||
        reflection.registry == nullptr || reflection.types == nullptr ||
        reflection.allocator == nullptr) {
        return Fail(writer, ErrorCode::InvalidArgument);
    }

    if (!ValidateShaderCompileOptions(*input.options)) {
        return Fail(writer, ErrorCode::Unsupported);
    }

    // 2행.
    if (auto tools = WriteTools(writer, input); !tools) {
        return tools;
    }

    // 3–5행.
    writer.WriteString(input.format->Id());
    writer.WriteDigest(input.format->sha256);
    writer.WriteU32(input.stage);
    writer.WriteString(input.entry);
    writer.WriteDigest(input.preprocessedSha256);

    // 6행: 옵션(리플렉션 기록 — 기본값 포함 전체)과 레이아웃 서명.
    auto lease = reflection.types->AcquireValueType(GeneratedType<ShaderCompileOptions>::Id(),
                                                    TypeKind::Struct);
    if (!lease) {
        return Fail(writer, lease.Error());
    }

    const reflection_text::AssignmentTarget target = reflection_text::AssignmentTarget::Value(
        *reflection.registry, reflection.access, lease.Value(),
        const_cast<ShaderCompileOptions*>(input.options), *reflection.allocator);
    if (auto written = WriteReflected(writer, target); !written) {
        return written;
    }

    if (input.layoutSignature != nullptr) {
        writer.WriteDigest(*input.layoutSignature);
    } else {
        writer.WriteNone();
    }

    // 7행.
    writer.WriteString(input.profileId);
    writer.WriteDigest(input.profileSha256);
    return writer.Error();
}

Result<Sha256Digest> BuildShaderJobKey(const ShaderJobKeyInput& input,
                                       NuggetKeyTrace* trace) noexcept {
    NuggetKeyWriter writer;
    writer.SetTrace(trace);
    WriteNuggetKeyHeader(writer, ShaderJobProcessor, input.projectGeneration);
    if (auto body = WriteShaderJobKeyBody(writer, input); !body) {
        return Result<Sha256Digest>::Failure(body.Error());
    }

    return writer.Finish();
}

} // namespace rocks::shader
