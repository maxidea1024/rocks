#include "ExchangeInternal.h"

#include <Core/Text/StringBuilder.h>
#include <ReflectionText/TextAssignments.h>
#include <ReflectionText/TextDiagnostics.h>
#include <ReflectionText/TextValue.h>
#include <UnrealExchange/ExchangeServiceKind.h>
#include <UnrealExchange/UnrealExchangeFormat.h>
#include <UnrealExchange/UnrealSkeletalMeshImporter.h>
#include <UnrealExchange/UnrealSkeletonImporter.h>
#include <UnrealExchange/UnrealStaticMeshImporter.h>
#include <UnrealExchange/UnrealTextureImporter.h>

// 교환 에셋의 종류별 임포터다(Docs/Design/UnrealExchange 10.6절 1–3번, 5.5절 11번).

namespace rocks {

namespace exchange_service_kind {

// 옵션 필드 이름이다(UnrealExchangeImportOptions — 10.6절 3번).
constexpr StringView ObjectPathOption = "objectPath";
constexpr StringView FormerPathsOption = "formerPaths";
constexpr StringView ReferencesOption = "references";
constexpr StringView MeshNormalsOption = "meshNormals";
constexpr char EscapeCharacter = '\\';
constexpr char ListSeparator = ',';
constexpr char MapSeparator = ':';
// 대입 수다(공통 셋 + 메시의 meshNormals).
constexpr usize CommonOptionCount = 3;
constexpr usize MaxOptionCount = CommonOptionCount + 1;

// meshNormals 대입 값이다. 리플렉션 enum ExchangeMeshNormals의 열거자 도구 이름과 같아야
// 한다(이름을 바꾸면 여기도 — 명령 시험 UnrealExchangeStaticMesh_ImportCommand가 두 값 모두
// 대입한다).
constexpr StringView MeshNormalsUnrealText = "Unreal";
constexpr StringView MeshNormalsRocksText = "Rocks";

StringView MeshNormalsText(ExchangeMeshNormals normals) noexcept {
    return normals == ExchangeMeshNormals::Rocks ? MeshNormalsRocksText : MeshNormalsUnrealText;
}

// 메시 원본 모드(5.3절)를 받는 클래스다.
bool UsesMeshNormals(ExchangeClass type) noexcept {
    return type == ExchangeClass::StaticMesh;
}

// 목록 원소 하나를 escape해 붙인다(ReflectionText 목록 escape 표 — `,`·`;`, map 키는 `:`도). escape
// 글자 사이 구간을 통째로 붙인다(UTF-8 다중 byte 글자를 쪼개지 않는다 — escape 글자는 ASCII다).
Result<void> AppendEscaped(StringBuilderBase& out, StringView text, bool key) noexcept {
    usize start = 0;
    for (usize index = 0; index < text.ByteCount(); ++index) {
        const char at = text.Data()[index];
        const bool special = reflection_text::TextListEscapeCharacters.Contains(
                                 StringView(&at, 1), StringComparison::CaseSensitive) &&
                             (key || at != MapSeparator);
        if (!special) {
            continue;
        }

        if (auto appended = out.TryAppend(text.SliceBytes(start, index - start)); !appended) {
            return appended;
        }

        if (auto escaped = out.TryAppendAscii(EscapeCharacter); !escaped) {
            return escaped;
        }

        if (auto escaped = out.TryAppendAscii(at); !escaped) {
            return escaped;
        }

        start = index + 1;
    }

    return out.TryAppend(text.SliceBytes(start, text.ByteCount() - start));
}

// 옵션 대입 텍스트다(10.6절 3번). 값 텍스트는 이 객체가 가진다.
struct OptionTexts {
    explicit OptionTexts(Allocator& allocator) noexcept
        : formerPaths(allocator), references(allocator) {
    }

    StringBuilder<512> formerPaths;
    StringBuilder<1024> references;
};

Result<void> BuildOptions(const ExchangeKindRequest& request, const ExchangeAsset& asset,
                          OptionTexts& out) noexcept {
    bool first = true;
    for (const ExchangeRedirect& redirect : request.bundle->Redirects()) {
        if (redirect.to != asset.objectPath) {
            continue;
        }

        if (!first) {
            if (auto separated = out.formerPaths.TryAppendAscii(ListSeparator); !separated) {
                return separated;
            }
        }

        if (auto appended = AppendEscaped(out.formerPaths, redirect.from, false); !appended) {
            return appended;
        }

        first = false;
    }

    first = true;
    for (usize index = 0; index < asset.dependencies.Size(); ++index) {
        const ExchangeDependency& dependency = asset.dependencies[index];
        const AssetId id =
            dependency.target != ExchangeNoIndex
                ? request.resolved[dependency.target]
                : request.plan->ExternalTarget(request.asset, static_cast<uint32>(index));
        if (id.IsNull()) {
            continue;
        }

        if (!first) {
            if (auto separated = out.references.TryAppendAscii(ListSeparator); !separated) {
                return separated;
            }
        }

        if (auto appended = AppendEscaped(out.references, dependency.objectPath, true); !appended) {
            return appended;
        }

        if (auto appended = out.references.TryAppendFormat(":{}", id.value); !appended) {
            return appended;
        }

        first = false;
    }

    return Result<void>::Success();
}

// 서비스 실패를 묶음 진단으로 옮긴다(10.6절 2번).
Result<void> ReportFailure(const ExchangeKindRequest& request, const AssetDiagnostic& failure,
                           ErrorCode error) noexcept {
    ExchangeDiagnosticInput input;
    input.code = failure.code == AssetDiagnosticCode::None ? AssetDiagnosticCode::InvalidSource
                                                           : failure.code;
    input.error = error;
    input.issue = ExchangeIssue::ServiceFailure;
    input.file = ExchangeFile::AssetJson;
    input.asset = request.asset;
    input.text = request.path;
    return request.diagnostics->TryAdd(input);
}

} // namespace exchange_service_kind

StringView ExchangeServiceImporterName(ExchangeClass type) noexcept {
    switch (type) {
    case ExchangeClass::StaticMesh:
        return UnrealStaticMeshImporterName;
    case ExchangeClass::Skeleton:
        return UnrealSkeletonImporterName;
    case ExchangeClass::SkeletalMesh:
        return UnrealSkeletalMeshImporterName;
    default:
        break;
    }

    return UnrealTextureImporterName(type);
}

Result<AssetId> ExchangeServiceKind::Import(const ExchangeKindRequest& request) noexcept {
    using namespace exchange_service_kind;
    using Out = Result<AssetId>;
    const ExchangeAsset& asset = request.bundle->Assets()[request.asset];
    const StringView importer = ExchangeServiceImporterName(asset.exchangeClass);
    if (importer.IsEmpty()) {
        return Out::Failure(ErrorCode::InvalidArgument);
    }

    Allocator& allocator = asset.blobs.GetAllocator();
    // 서비스를 부르기 전의 실패도 묶음 진단을 남긴다(종류별 임포터 계약 — 할당 실패만 진단 없이).
    const auto fail = [&](ErrorCode error) noexcept -> Out {
        if (error != ErrorCode::OutOfMemory) {
            if (auto reported = ReportFailure(request, AssetDiagnostic{}, error); !reported) {
                return Out::Failure(reported.Error());
            }
        }

        return Out::Failure(error);
    };

    // 원본: 에셋 JSON과 blob(asset.blobs 순).
    StringBuilder<512> documentPath(allocator);
    if (auto made = exchange_detail::AppendBundlePath(request.bundle->Directory(), asset.file,
                                                      documentPath);
        !made) {
        return fail(made.Error());
    }

    Array<String> roles(allocator);
    Array<String> paths(allocator);
    Array<AssetImportSourceInput> sources(allocator);
    if (auto reserved = roles.TryReserve(asset.blobs.Size()); !reserved) {
        return fail(reserved.Error());
    }

    if (auto reserved = paths.TryReserve(asset.blobs.Size()); !reserved) {
        return fail(reserved.Error());
    }

    if (auto reserved = sources.TryReserve(asset.blobs.Size() + 1); !reserved) {
        return fail(reserved.Error());
    }

    (void)sources.TryPushBack(AssetImportSourceInput{UnrealExchangeAssetRole, documentPath.View()});
    for (usize index = 0; index < asset.blobs.Size(); ++index) {
        // 원본 역할은 digest마다 하나다(blob 파일 이름이 digest다). 같은 byte를 다른 배치로
        // 가리키는 참조(정육면체의 둘레 목록과 삼각형 코너 — 5.5절 10번)는 한 원본을 같이 쓴다.
        const ExchangeBlobRef& blob = asset.blobs[index];
        bool repeated = false;
        for (usize earlier = 0; earlier < index && !repeated; ++earlier) {
            repeated = asset.blobs[earlier].digest == blob.digest;
        }

        if (repeated) {
            continue;
        }

        char hex[Sha256Digest::HexBytes + 1];
        blob.digest.ToHex(hex);
        String role(allocator);
        StringBuilder<512> path(allocator);
        if (auto built = role.TryAppend(UnrealExchangeBlobRolePrefix); !built) {
            return fail(built.Error());
        }

        if (auto built = role.TryAppend(StringView(hex, Sha256Digest::HexBytes)); !built) {
            return fail(built.Error());
        }

        if (auto built = AppendExchangeBlobPath(request.bundle->Directory(), blob.digest, path);
            !built) {
            return fail(built.Error());
        }

        String copy(allocator);
        if (auto assigned = copy.TryAssign(path.View()); !assigned) {
            return fail(assigned.Error());
        }

        (void)roles.TryPushBack(rocks::Move(role));
        (void)paths.TryPushBack(rocks::Move(copy));
        (void)sources.TryPushBack(
            AssetImportSourceInput{roles[roles.Size() - 1].View(), paths[paths.Size() - 1].View()});
    }

    // 옵션(대입 텍스트).
    OptionTexts texts(allocator);
    if (auto built = BuildOptions(request, asset, texts); !built) {
        return fail(built.Error());
    }

    const ExchangeMeshNormals normals =
        request.options != nullptr ? request.options->meshNormals : ExchangeMeshNormals::Unreal;
    const reflection_text::TextAssignment assignments[MaxOptionCount] = {
        {reflection_text::AssignmentKind::Set, ObjectPathOption, asset.objectPath, 0},
        {reflection_text::AssignmentKind::Set, FormerPathsOption, texts.formerPaths.View(), 1},
        {reflection_text::AssignmentKind::Set, ReferencesOption, texts.references.View(), 2},
        {reflection_text::AssignmentKind::Set, MeshNormalsOption, MeshNormalsText(normals), 3},
    };

    reflection_text::TextDiagnostics diagnostics(allocator);
    AssetImportResult result(allocator);
    AssetDiagnostic failure;
    const Span<const reflection_text::TextAssignment> options(
        assignments, UsesMeshNormals(asset.exchangeClass) ? MaxOptionCount : CommonOptionCount);
    auto imported =
        request.existing.IsNull()
            ? request.service->Import(*request.lock, request.path, sources.View(), importer,
                                      options, diagnostics, result, request.warnings, &failure)
            : request.service->Reimport(*request.lock, request.path, sources.View(), options,
                                        diagnostics, result, request.warnings, &failure, true);
    if (!imported) {
        if (imported.Error() == ErrorCode::OutOfMemory) {
            return Out::Failure(imported.Error());
        }

        if (auto reported = ReportFailure(request, failure, imported.Error()); !reported) {
            return Out::Failure(reported.Error());
        }

        return Out::Failure(imported.Error());
    }

    return Out::Success(result.id);
}

Result<void> RegisterExchangeServiceKinds(void* state, ExchangeKindTable& table) noexcept {
    using namespace exchange_service_kind;
    if (state == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto& kind = *static_cast<ExchangeServiceKind*>(state);
    for (const ExchangeClass type : ExchangeServiceClasses) {
        if (auto registered = table.Register(type, kind); !registered) {
            return registered;
        }
    }

    return Result<void>::Success();
}

} // namespace rocks
