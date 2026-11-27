#include <AssetCooker/CookConfig.generated.h>

#include <AssetCooker/CookConfig.h>
#include <AssetDatabase/LogicalPath.h>
#include <AssetNugget/NuggetReflected.h>
#include <AssetRuntime/Package.generated.h>
#include <Core/FileError.h>
#include <Object/TypeRegistry.h>
#include <TargetProfiles/TargetProfile.generated.h>

namespace rocks {

namespace {

using reflection_text::AssignmentTarget;
using reflection_text::TextDiagnostics;

Result<void> Fail(CookConfigFailure* failure, CookConfigFile file, CookConfigField field,
                  ErrorCode error, uint32 index = 0) noexcept {
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

// directory/name 경로를 만든다.
Result<void> JoinPath(StringView directory, StringView middle, StringView name, StringView suffix,
                      String& out) noexcept {
    out.Clear();
    const StringView pieces[] = {directory, "/", middle, middle.IsEmpty() ? "" : "/", name, suffix};
    for (StringView piece : pieces) {
        if (auto appended = out.TryAppend(piece); !appended) {
            return appended;
        }
    }

    return Result<void>::Success();
}

// path의 JSON을 target에 대입한다(TargetProfiles 공개 도우미 — 실패 단계를 칸으로 옮긴다).
Result<void> ApplyJsonFile(const CookConfigContext& context, StringView path,
                           const AssignmentTarget& target, TextDiagnostics& diagnostics,
                           CookConfigFile file, CookConfigFailure* failure,
                           bool& existed) noexcept {
    ConfigJsonOutcome outcome;
    auto applied =
        ApplyConfigJsonFile(*context.allocator, *context.files, path, target, diagnostics, outcome);
    existed = outcome.existed;
    if (applied) {
        return applied;
    }

    const ErrorCode error = applied.Error();
    CookConfigField field = CookConfigField::None;
    if (outcome.stage == ConfigJsonStage::File && error == ErrorCode::CapacityExceeded) {
        field = CookConfigField::File;
    } else if (outcome.stage == ConfigJsonStage::Json) {
        field = CookConfigField::Json;
        if (failure != nullptr) {
            failure->line = outcome.line;
            failure->column = outcome.column;
        }
    } else if (outcome.stage == ConfigJsonStage::Text) {
        field = CookConfigField::Text;
    }

    return Fail(failure, file, field, error);
}

template <class T>
Result<Sha256Digest> HashReflected(const CookConfigContext& context, const T& value) noexcept {
    auto lease = context.types->AcquireValueType(GeneratedType<T>::Id(), TypeKind::Struct);
    if (!lease) {
        return Result<Sha256Digest>::Failure(lease.Error());
    }

    NuggetKeyWriter writer;
    const AssignmentTarget target =
        AssignmentTarget::Value(*context.registry, context.access, lease.Value(),
                                const_cast<T*>(&value), *context.allocator);
    if (auto written = WriteReflected(writer, target); !written) {
        return Result<Sha256Digest>::Failure(written.Error());
    }

    return writer.Finish();
}

} // namespace

Result<CookPathKind> ClassifyCookPath(Allocator& allocator, StringView text) noexcept {
    const bool directory = text.EndsWith("/", StringComparison::CaseSensitive);
    const StringView path = directory ? StringView(text.Data(), text.ByteCount() - 1) : text;
    if (directory && EqualsIgnoringAsciiCase(path, GameMount)) {
        return Result<CookPathKind>::Success(CookPathKind::Directory);
    }

    auto parsed = ParseLogicalPath(allocator, path, nullptr);
    if (!parsed) {
        return parsed.Error() == ErrorCode::OutOfMemory
                   ? Result<CookPathKind>::Failure(parsed.Error())
                   : Result<CookPathKind>::Success(CookPathKind::Invalid);
    }

    return Result<CookPathKind>::Success(directory ? CookPathKind::Directory : CookPathKind::Asset);
}

CookConfigField ToCookConfigField(TargetProfileField field) noexcept {
    switch (field) {
    case TargetProfileField::None:
        return CookConfigField::None;
    case TargetProfileField::File:
        return CookConfigField::File;
    case TargetProfileField::Json:
        return CookConfigField::Json;
    case TargetProfileField::Text:
        return CookConfigField::Text;
    case TargetProfileField::Id:
        return CookConfigField::Id;
    case TargetProfileField::BulkAlignment:
        return CookConfigField::BulkAlignment;
    case TargetProfileField::TextureLodGroup:
        return CookConfigField::TextureLodGroup;
    case TargetProfileField::GraphicsBackends:
        return CookConfigField::GraphicsBackends;
    case TargetProfileField::ShaderFormats:
        return CookConfigField::ShaderFormats;
    case TargetProfileField::AudioCook:
        return CookConfigField::AudioCook;
    }

    return CookConfigField::None;
}

CookConfigFile ToCookConfigFile(TargetProfileFile file) noexcept {
    switch (file) {
    case TargetProfileFile::None:
        return CookConfigFile::None;
    case TargetProfileFile::EngineProfile:
        return CookConfigFile::EngineProfile;
    case TargetProfileFile::ProjectProfile:
        return CookConfigFile::ProjectProfile;
    }

    return CookConfigFile::None;
}

Result<void> ReadTargetProfile(const CookConfigContext& context, StringView id, TargetProfile& out,
                               TextDiagnostics& diagnostics, CookConfigFailure* failure) noexcept {
    const TargetProfileContext profiles{context.allocator,
                                        context.files,
                                        context.registry,
                                        context.access,
                                        context.types,
                                        context.engineConfigDirectory,
                                        context.projectConfigDirectory};
    TargetProfileFailure read;
    auto done = rocks::ReadTargetProfile(profiles, id, out, diagnostics, &read);
    if (failure != nullptr) {
        *failure = CookConfigFailure{};
        failure->code = read.code;
        failure->error = read.error;
        failure->file = ToCookConfigFile(read.file);
        failure->field = ToCookConfigField(read.field);
        failure->index = read.index;
        failure->line = read.line;
        failure->column = read.column;
    }

    return done;
}

Result<void> ReadCookSettings(const CookConfigContext& context, CookSettings& out,
                              TextDiagnostics& diagnostics, CookConfigFailure* failure) noexcept {
    if (failure != nullptr) {
        *failure = CookConfigFailure{};
    }

    Allocator& allocator = *context.allocator;
    out = CookSettings(allocator);
    auto lease =
        context.types->AcquireValueType(GeneratedType<CookSettings>::Id(), TypeKind::Struct);
    if (!lease) {
        return Fail(failure, CookConfigFile::None, CookConfigField::None, lease.Error());
    }

    const AssignmentTarget target =
        AssignmentTarget::Value(*context.registry, context.access, lease.Value(), &out, allocator);
    String path(allocator);
    if (auto joined = JoinPath(context.projectConfigDirectory, "", CookSettingsFile, "", path);
        !joined) {
        return Fail(failure, CookConfigFile::CookSettings, CookConfigField::None, joined.Error());
    }

    bool existed = false;
    if (auto applied = ApplyJsonFile(context, path.View(), target, diagnostics,
                                     CookConfigFile::CookSettings, failure, existed);
        !applied) {
        return applied;
    }

    struct PathList {
        const Array<String>* list;
        CookConfigField field;
    };
    const PathList lists[] = {{&out.alwaysCook, CookConfigField::AlwaysCook},
                              {&out.neverCook, CookConfigField::NeverCook}};
    for (const PathList& item : lists) {
        for (usize index = 0; index < item.list->Size(); ++index) {
            auto kind = ClassifyCookPath(allocator, (*item.list)[index].View());
            if (!kind) {
                return Fail(failure, CookConfigFile::CookSettings, CookConfigField::None,
                            kind.Error());
            }

            if (kind.Value() == CookPathKind::Invalid) {
                return Fail(failure, CookConfigFile::CookSettings, item.field,
                            ErrorCode::InvalidArgument, static_cast<uint32>(index));
            }
        }
    }

    for (usize index = 0; index < out.primaryAssets.Size(); ++index) {
        const PrimaryAssetRule& rule = out.primaryAssets[index];
        const StringView name = rule.type.View();
        auto type =
            context.types->AcquireByToolName(Span<const char>(name.Data(), name.ByteCount()));
        // 조회는 할당하지 않는다 — 실패(없음·빈 이름)는 규칙 위반이다(종료·lease 계수 포화는 쿠킹
        // 중에 생기지 않는다).
        const bool asset = type && type.Value().Kind() == TypeKind::Class &&
                           type.Value().IsA(GeneratedType<Asset>::Id());
        if (!asset) {
            return Fail(failure, CookConfigFile::CookSettings, CookConfigField::PrimaryAssetType,
                        ErrorCode::InvalidArgument, static_cast<uint32>(index));
        }

        if (rule.directories.IsEmpty()) {
            return Fail(failure, CookConfigFile::CookSettings,
                        CookConfigField::PrimaryAssetDirectory, ErrorCode::InvalidArgument,
                        static_cast<uint32>(index));
        }

        for (usize item = 0; item < rule.directories.Size(); ++item) {
            auto kind = ClassifyCookPath(allocator, rule.directories[item].View());
            if (!kind) {
                return Fail(failure, CookConfigFile::CookSettings, CookConfigField::None,
                            kind.Error());
            }

            if (kind.Value() != CookPathKind::Directory) {
                if (failure != nullptr) {
                    failure->item = static_cast<uint32>(item);
                }

                return Fail(failure, CookConfigFile::CookSettings,
                            CookConfigField::PrimaryAssetDirectory, ErrorCode::InvalidArgument,
                            static_cast<uint32>(index));
            }
        }
    }

    return Result<void>::Success();
}

Result<Sha256Digest> HashTargetProfile(const CookConfigContext& context,
                                       const TargetProfile& profile) noexcept {
    return HashReflected(context, profile);
}

Result<Sha256Digest> HashCookSettings(const CookConfigContext& context,
                                      const CookSettings& settings) noexcept {
    return HashReflected(context, settings);
}

} // namespace rocks
