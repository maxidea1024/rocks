#include <AssetCooker/CookConfig.generated.h>

#include <AssetCooker/CookConfig.h>
#include <AssetDatabase/LogicalPath.h>
#include <AssetNugget/NuggetReflected.h>
#include <AssetRuntime/Package.generated.h>
#include <Core/FileError.h>
#include <Core/Text/Json.h>
#include <Object/TypeRegistry.h>
#include <ReflectionText/TextJson.h>

namespace rocks {

namespace {

using reflection_text::AssignmentTarget;
using reflection_text::TextCodecOptions;
using reflection_text::TextDiagnostics;

// bulk 정렬 범위다(쿠킹 패키지 writer와 같다 — PackageWriteOptions).
constexpr uint32 CookBulkAlignmentMin = 8;
constexpr uint32 CookBulkAlignmentMax = 65536;
constexpr StringView CookJsonExtension = ".json";

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

// path 파일 전체를 읽는다. 없으면 existed = false로 성공한다. 상한을 넘으면 CapacityExceeded다.
Result<void> ReadWholeFile(platform::FileSystem& files, StringView path, Array<uint8>& bytes,
                           bool& existed) noexcept {
    existed = false;
    platform::File file;
    if (auto opened =
            file.Open(files, path.Data(), path.ByteCount(), platform::FileOpenMode::ReadExisting);
        !opened) {
        if (opened.code == platform::FileCode::NotFound) {
            return Result<void>::Success();
        }

        return Result<void>::Failure(FileErrorCode(opened));
    }

    existed = true;
    uint64 size = 0;
    if (auto sized = file.Size(size); !sized) {
        return Result<void>::Failure(FileErrorCode(sized));
    }

    if (size > CookConfigMaxFileBytes) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    if (auto resized = bytes.TryResize(static_cast<usize>(size)); !resized) {
        return resized;
    }

    const platform::FileTransfer read = file.ReadExact(bytes.Data(), bytes.Size());
    return read.status ? Result<void>::Success()
                       : Result<void>::Failure(FileErrorCode(read.status));
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

// path의 JSON을 target에 대입한다(파일이 없으면 existed = false로 아무것도 하지 않는다).
Result<void> ApplyJsonFile(const CookConfigContext& context, StringView path,
                           const AssignmentTarget& target, TextDiagnostics& diagnostics,
                           CookConfigFile file, CookConfigFailure* failure,
                           bool& existed) noexcept {
    Allocator& allocator = *context.allocator;
    Array<uint8> bytes(allocator);
    if (auto read = ReadWholeFile(*context.files, path, bytes, existed); !read) {
        const CookConfigField field = read.Error() == ErrorCode::CapacityExceeded
                                          ? CookConfigField::File
                                          : CookConfigField::None;
        return Fail(failure, file, field, read.Error());
    }

    if (!existed) {
        return Result<void>::Success();
    }

    JsonDocument document(allocator);
    const JsonError parsed =
        document.Parse(StringView(reinterpret_cast<const char*>(bytes.Data()), bytes.Size()));
    if (!parsed.Succeeded()) {
        if (parsed.code == JsonErrorCode::OutOfMemory) {
            return Fail(failure, file, CookConfigField::None, ErrorCode::OutOfMemory);
        }

        if (failure != nullptr) {
            failure->line = parsed.position.line;
            failure->column = parsed.position.column;
        }

        return Fail(failure, file, CookConfigField::Json, ErrorCode::InvalidArgument);
    }

    TextCodecOptions options;
    auto prepared = reflection_text::ImportJson(document.Root(), target, options, diagnostics);
    if (!prepared) {
        const ErrorCode error = prepared.Error();
        return Fail(failure, file,
                    error == ErrorCode::InvalidArgument ? CookConfigField::Text
                                                        : CookConfigField::None,
                    error);
    }

    if (auto published = prepared.Value().Publish(); !published) {
        return Fail(failure, file, CookConfigField::None, published.Error());
    }

    return Result<void>::Success();
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

bool IsValidBulkAlignment(uint32 alignment) noexcept {
    return alignment == 0 ||
           (alignment >= CookBulkAlignmentMin && alignment <= CookBulkAlignmentMax &&
            (alignment & (alignment - 1)) == 0);
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

Result<void> ReadTargetProfile(const CookConfigContext& context, StringView id, TargetProfile& out,
                               TextDiagnostics& diagnostics, CookConfigFailure* failure) noexcept {
    if (failure != nullptr) {
        *failure = CookConfigFailure{};
    }

    if (!IsValidProfileId(id)) {
        return Fail(failure, CookConfigFile::None, CookConfigField::Id, ErrorCode::InvalidArgument);
    }

    out = TargetProfile(*context.allocator);
    if (auto assigned = out.id.TryAssign(id); !assigned) {
        return Fail(failure, CookConfigFile::None, CookConfigField::None, assigned.Error());
    }

    auto lease =
        context.types->AcquireValueType(GeneratedType<TargetProfile>::Id(), TypeKind::Struct);
    if (!lease) {
        return Fail(failure, CookConfigFile::None, CookConfigField::None, lease.Error());
    }

    const AssignmentTarget target = AssignmentTarget::Value(
        *context.registry, context.access, lease.Value(), &out, *context.allocator);
    String path(*context.allocator);
    struct Layer {
        StringView directory;
        CookConfigFile file;
    };
    const Layer layers[] = {{context.engineConfigDirectory, CookConfigFile::EngineProfile},
                            {context.projectConfigDirectory, CookConfigFile::ProjectProfile}};
    for (const Layer& layer : layers) {
        if (auto joined =
                JoinPath(layer.directory, TargetProfileDirectory, id, CookJsonExtension, path);
            !joined) {
            return Fail(failure, layer.file, CookConfigField::None, joined.Error());
        }

        bool existed = false;
        if (auto applied = ApplyJsonFile(context, path.View(), target, diagnostics, layer.file,
                                         failure, existed);
            !applied) {
            return applied;
        }

        if (!existed && layer.file == CookConfigFile::EngineProfile) {
            // 엔진 플랫폼 목록이 기준이다 — 프로젝트만의 프로파일은 받지 않는다.
            return Fail(failure, layer.file, CookConfigField::None, ErrorCode::InvalidArgument);
        }

        if (out.id.View() != id) {
            return Fail(failure, layer.file, CookConfigField::Id, ErrorCode::InvalidArgument);
        }
    }

    if (!IsValidBulkAlignment(out.bulkAlignment)) {
        return Fail(failure, CookConfigFile::None, CookConfigField::BulkAlignment,
                    ErrorCode::InvalidArgument);
    }

    return Result<void>::Success();
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
