#include <TargetProfiles/TargetProfile.generated.h>

#include <AssetRuntime/CookedManifest.h>
#include <Core/FileRead.h>
#include <Core/Text/Json.h>
#include <HostConfig/HostSettings.h>
#include <Meta/GeneratedType.h>
#include <Object/TypeRegistry.h>
#include <ReflectionText/TextJson.h>
#include <TargetProfiles/TargetProfile.h>

namespace rocks {

namespace {

using reflection_text::AssignmentTarget;
using reflection_text::TextCodecOptions;
using reflection_text::TextDiagnostics;

// bulk 정렬 범위다(쿠킹 패키지 writer와 같다 — PackageWriteOptions).
constexpr uint32 ProfileBulkAlignmentMin = 8;
constexpr uint32 ProfileBulkAlignmentMax = 65536;
constexpr StringView ProfileJsonExtension = ".json";

// backend 이름 어휘다(10.3절 1항).
constexpr StringView GraphicsBackendNames[] = {"D3D12", "D3D11", "Vulkan", "Metal", "OpenGL"};

Result<void> ProfileFail(TargetProfileFailure* failure, TargetProfileFile file,
                         TargetProfileField field, ErrorCode error, uint32 index = 0) noexcept {
    if (failure != nullptr) {
        failure->code = error == ErrorCode::InvalidArgument ? AssetDiagnosticCode::InvalidSetting
                                                            : AssetDiagnosticCode::None;
        failure->error = error;
        failure->file = file;
        failure->field = field;
        failure->index = index;
    }

    return Result<void>::Failure(error);
}

bool IsProfileIdCharacter(char c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

bool IsValidProfileId(StringView id) noexcept {
    if (id.IsEmpty() || id.ByteCount() > TargetProfileIdMaxBytes) {
        return false;
    }

    for (usize index = 0; index < id.ByteCount(); ++index) {
        if (!IsProfileIdCharacter(id.Data()[index])) {
            return false;
        }
    }

    return true;
}

bool IsValidProfileBulkAlignment(uint32 alignment) noexcept {
    return alignment == 0 ||
           (alignment >= ProfileBulkAlignmentMin && alignment <= ProfileBulkAlignmentMax &&
            (alignment & (alignment - 1)) == 0);
}

// 텍스처 LOD 그룹 검증이다(12.14절).
Result<void> CheckLodGroups(const TargetProfile& profile, TargetProfileFailure* failure) noexcept {
    for (const auto& entry : profile.textureLodGroups) {
        const TextureLodGroup& group = entry.value;
        const TextureMipGenSettings mipGen = group.mipGenSettings;
        const bool valid = group.minLodSize >= 0 && group.maxLodSize >= 0 && group.maxAniso >= 0 &&
                           group.minLodSize <= group.maxLodSize &&
                           (mipGen == TextureMipGenSettings::SimpleAverage ||
                            mipGen == TextureMipGenSettings::NoMipmaps ||
                            mipGen == TextureMipGenSettings::Unfiltered) &&
                           group.mipLoadOptions != TextureMipLoadOptions::OnlyFirstMip;
        if (!valid) {
            return ProfileFail(failure, TargetProfileFile::None,
                               TargetProfileField::TextureLodGroup, ErrorCode::InvalidArgument,
                               static_cast<uint32>(entry.key));
        }
    }

    return Result<void>::Success();
}

// 셰이더 칸 구조 검증이다(10.3절 1항 — 형식 정의 대조는 비에셋 산출물 단계가 한다).
Result<void> CheckShaderFields(const TargetProfile& profile,
                               TargetProfileFailure* failure) noexcept {
    const Array<String>& backends = profile.graphicsBackends;
    auto fail = [&](TargetProfileField field, usize index) noexcept {
        return ProfileFail(failure, TargetProfileFile::None, field, ErrorCode::InvalidArgument,
                           static_cast<uint32>(index));
    };
    // 셰이더 없는 프로파일은 형식 Map도 빈다.
    if (backends.IsEmpty()) {
        return profile.shaderFormats.IsEmpty() ? Result<void>::Success()
                                               : fail(TargetProfileField::ShaderFormats, 0);
    }

    if (backends.Size() > CookedManifestMaxBackends) {
        return fail(TargetProfileField::GraphicsBackends, CookedManifestMaxBackends);
    }

    for (usize index = 0; index < backends.Size(); ++index) {
        const StringView name = backends[index].View();
        if (!IsKnownGraphicsBackend(name)) {
            return fail(TargetProfileField::GraphicsBackends, index);
        }

        for (usize other = 0; other < index; ++other) {
            if (backends[other].View() == name) {
                return fail(TargetProfileField::GraphicsBackends, index);
            }
        }
    }

    // 목록에 없는 backend 키(Map 순서의 번호).
    usize key = 0;
    for (const auto& entry : profile.shaderFormats) {
        bool listed = false;
        for (const String& backend : backends) {
            listed = listed || backend.View() == entry.key.View();
        }

        if (!listed) {
            return fail(TargetProfileField::ShaderFormats, key);
        }

        ++key;
    }

    // backend마다 형식 목록(있음·비지 않음·상한·문법·전역 중복 — 전역 중복은 앞 backend까지 본다).
    for (usize index = 0; index < backends.Size(); ++index) {
        const ShaderFormatList* list = profile.shaderFormats.Find(backends[index]);
        if (list == nullptr || list->formats.IsEmpty() ||
            list->formats.Size() > CookedManifestMaxFormatsPerBackend) {
            return fail(TargetProfileField::ShaderFormats, index);
        }

        for (usize format = 0; format < list->formats.Size(); ++format) {
            const StringView id = list->formats[format].View();
            if (!IsValidShaderFormatId(id)) {
                return fail(TargetProfileField::ShaderFormats, index);
            }

            for (usize previous = 0; previous <= index; ++previous) {
                const ShaderFormatList* earlier = profile.shaderFormats.Find(backends[previous]);
                const usize end = previous == index ? format : earlier->formats.Size();
                for (usize at = 0; at < end; ++at) {
                    if (earlier->formats[at].View() == id) {
                        return fail(TargetProfileField::ShaderFormats, index);
                    }
                }
            }
        }
    }

    return Result<void>::Success();
}

} // namespace

bool IsKnownGraphicsBackend(StringView name) noexcept {
    for (StringView known : GraphicsBackendNames) {
        if (known == name) {
            return true;
        }
    }

    return false;
}

bool IsValidShaderFormatId(StringView id) noexcept {
    if (id.IsEmpty() || id.ByteCount() > ShaderFormatIdMaxBytes) {
        return false;
    }

    for (usize index = 0; index < id.ByteCount(); ++index) {
        const char c = id.Data()[index];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
    }

    return true;
}

Result<void> ApplyConfigJsonFile(Allocator& allocator, platform::FileSystem& files, StringView path,
                                 const AssignmentTarget& target, TextDiagnostics& diagnostics,
                                 ConfigJsonOutcome& outcome) noexcept {
    outcome = ConfigJsonOutcome{};
    Array<uint8> bytes(allocator);
    if (auto read = ReadWholeFile(files, path, ConfigJsonMaxFileBytes, bytes, outcome.existed);
        !read) {
        outcome.stage = ConfigJsonStage::File;
        return read;
    }

    if (!outcome.existed) {
        return Result<void>::Success();
    }

    JsonDocument document(allocator);
    const JsonError parsed =
        document.Parse(StringView(reinterpret_cast<const char*>(bytes.Data()), bytes.Size()));
    if (!parsed.Succeeded()) {
        if (parsed.code == JsonErrorCode::OutOfMemory) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }

        outcome.stage = ConfigJsonStage::Json;
        outcome.line = parsed.position.line;
        outcome.column = parsed.position.column;
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 기본 옵션(Map 통째 교체)이다 — 키별 병합은 필드 플래그 TextMapMergeByKey가 고른다(10.3.1절).
    const TextCodecOptions options;
    auto prepared = reflection_text::ImportJson(document.Root(), target, options, diagnostics);
    if (!prepared) {
        if (prepared.Error() == ErrorCode::InvalidArgument) {
            outcome.stage = ConfigJsonStage::Text;
        }

        return Result<void>::Failure(prepared.Error());
    }

    return prepared.Value().Publish();
}

Result<void> ReadTargetProfile(const TargetProfileContext& context, StringView id,
                               TargetProfile& out, TextDiagnostics& diagnostics,
                               TargetProfileFailure* failure) noexcept {
    if (failure != nullptr) {
        *failure = TargetProfileFailure{};
    }

    if (!IsValidProfileId(id)) {
        return ProfileFail(failure, TargetProfileFile::None, TargetProfileField::Id,
                           ErrorCode::InvalidArgument);
    }

    Allocator& allocator = *context.allocator;
    out = TargetProfile(allocator);
    if (auto assigned = out.id.TryAssign(id); !assigned) {
        return ProfileFail(failure, TargetProfileFile::None, TargetProfileField::None,
                           assigned.Error());
    }

    auto lease =
        context.types->AcquireValueType(GeneratedType<TargetProfile>::Id(), TypeKind::Struct);
    if (!lease) {
        return ProfileFail(failure, TargetProfileFile::None, TargetProfileField::None,
                           lease.Error());
    }

    const AssignmentTarget target =
        AssignmentTarget::Value(*context.registry, context.access, lease.Value(), &out, allocator);
    struct Layer {
        StringView directory;
        TargetProfileFile file;
    };
    const Layer layers[] = {{context.engineConfigDirectory, TargetProfileFile::EngineProfile},
                            {context.projectConfigDirectory, TargetProfileFile::ProjectProfile}};
    String path(allocator);
    for (const Layer& layer : layers) {
        if (layer.file == TargetProfileFile::ProjectProfile && layer.directory.IsEmpty()) {
            continue;
        }

        path.Clear();
        const StringView pieces[] = {layer.directory,     "/", TargetProfileDirectory, "/", id,
                                     ProfileJsonExtension};
        for (StringView piece : pieces) {
            if (auto appended = path.TryAppend(piece); !appended) {
                return ProfileFail(failure, layer.file, TargetProfileField::None, appended.Error());
            }
        }

        ConfigJsonOutcome outcome;
        if (auto applied = ApplyConfigJsonFile(allocator, *context.files, path.View(), target,
                                               diagnostics, outcome);
            !applied) {
            const ErrorCode error = applied.Error();
            TargetProfileField field = TargetProfileField::None;
            if (outcome.stage == ConfigJsonStage::File && error == ErrorCode::CapacityExceeded) {
                field = TargetProfileField::File;
            } else if (outcome.stage == ConfigJsonStage::Json) {
                field = TargetProfileField::Json;
                if (failure != nullptr) {
                    failure->line = outcome.line;
                    failure->column = outcome.column;
                }
            } else if (outcome.stage == ConfigJsonStage::Text) {
                field = TargetProfileField::Text;
            }

            return ProfileFail(failure, layer.file, field, error);
        }

        if (!outcome.existed && layer.file == TargetProfileFile::EngineProfile) {
            // 엔진 플랫폼 목록이 기준이다 — 프로젝트만의 프로파일은 받지 않는다.
            return ProfileFail(failure, layer.file, TargetProfileField::None,
                               ErrorCode::InvalidArgument);
        }

        if (out.id.View() != id) {
            return ProfileFail(failure, layer.file, TargetProfileField::Id,
                               ErrorCode::InvalidArgument);
        }
    }

    if (!IsValidProfileBulkAlignment(out.bulkAlignment)) {
        return ProfileFail(failure, TargetProfileFile::None, TargetProfileField::BulkAlignment,
                           ErrorCode::InvalidArgument);
    }

    if (auto checked = CheckLodGroups(out, failure); !checked) {
        return checked;
    }

    return CheckShaderFields(out, failure);
}

Result<void> ReadHostTargetProfile(const TargetProfileContext& context, StringView platform,
                                   TargetProfile& out, TextDiagnostics& diagnostics,
                                   TargetProfileFailure* failure) noexcept {
    const StringView name = platform.IsEmpty() ? HostPlatformName() : platform;
    return ReadTargetProfile(context, name, out, diagnostics, failure);
}

TextureLodGroup FindTextureLodGroup(const TargetProfile& profile, TextureGroup group) noexcept {
    const TextureLodGroup* found = profile.textureLodGroups.Find(group);
    return found != nullptr ? *found : TextureLodGroup{};
}

} // namespace rocks
