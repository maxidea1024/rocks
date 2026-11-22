#include "AssetDatabaseInternal.h"

#include <AssetDatabase/AssetService.h>
#include <AssetDatabase/AssetSourceRoots.h>
#include <AssetDatabase/ImportRecord.h>
#include <AssetDatabase/LogicalPath.h>
#include <AssetRuntime/PackageRangeReader.h>
#include <AssetRuntime/PackageReader.h>
#include <Core/Text/StringBuilder.h>

// 임포트 상태다(Docs/Design/AssetSystem.ko.md 7.1절 18 A2f2b — asset-info --import). 패키지를
// 열지 않고 파일의 ImportRecord 칸만 읽는다.
namespace rocks {

namespace {

template <typename T>
Result<T> FailStatus(AssetDiagnostic* failure, AssetDiagnosticCode code, ErrorCode error,
                     AssetId asset = {}) noexcept {
    if (failure != nullptr) {
        *failure = AssetDiagnostic{};
        failure->code = error == ErrorCode::OutOfMemory ? AssetDiagnosticCode::None : code;
        failure->error = error;
        failure->asset = asset;
    }

    return Result<T>::Failure(error);
}

// 색인 조회의 오류를 진단으로 옮긴다(같은 경로 키 둘·같은 id 둘은 AlreadyExists).
Result<void> FailLookup(AssetDiagnostic* failure, ErrorCode error, bool byId,
                        AssetId asset = {}) noexcept {
    const AssetDiagnosticCode code = error != ErrorCode::AlreadyExists ? AssetDiagnosticCode::None
                                     : byId ? AssetDiagnosticCode::DuplicateAssetId
                                            : AssetDiagnosticCode::DuplicatePath;
    return FailStatus<void>(failure, code, error, asset);
}

} // namespace

Result<void> AssetService::ImportStatus(StringView target, AssetImportStatus& out,
                                        Array<EditorWarning>* warnings,
                                        AssetDiagnostic* failure) noexcept {
    Allocator& allocator = *allocator_;
    if (!initialized_) {
        return FailStatus<void>(failure, AssetDiagnosticCode::None, ErrorCode::InvalidArgument);
    }

    // 대상: 논리 경로(색인에 없으면 별칭) 또는 AssetId.
    const AssetIndexEntry* entry = nullptr;
    if (target.StartsWith("/", StringComparison::CaseSensitive)) {
        auto path = ParseLogicalPath(allocator, target);
        if (!path) {
            return FailStatus<void>(failure,
                                    path.Error() == ErrorCode::InvalidArgument
                                        ? AssetDiagnosticCode::InvalidPath
                                        : AssetDiagnosticCode::None,
                                    path.Error());
        }

        auto found = index_.FindByPath(path.Value());
        if (!found && found.Error() == ErrorCode::NotFound) {
            auto aliased = ResolveAliasPath(path.Value(), warnings, failure);
            if (!aliased) {
                return Result<void>::Failure(aliased.Error());
            }

            found = index_.FindById(aliased.Value());
            if (!found) {
                return FailLookup(failure, found.Error(), true, aliased.Value());
            }
        } else if (!found) {
            return FailLookup(failure, found.Error(), false);
        }

        entry = found.Value();
    } else {
        auto parsed = AssetId::TryParse(target);
        if (!parsed || parsed.Value().IsNull()) {
            return FailStatus<void>(failure, AssetDiagnosticCode::InvalidPath,
                                    ErrorCode::InvalidArgument);
        }

        auto found = index_.FindById(parsed.Value());
        if (!found) {
            return FailLookup(failure, found.Error(), true, parsed.Value());
        }

        entry = found.Value();
    }

    const AssetId id = entry->id;
    // 파일의 ImportRecord 칸만 읽는다(부분 읽기 — 헤더·섹션 표·고른 섹션).
    String file(allocator);
    Result<void> step = file.TryAppend(layout_->ContentDirectory());
    if (step) {
        step = file.TryAppend("/");
    }

    if (step) {
        step = file.TryAppend(entry->fileName.View());
    }

    if (!step) {
        return FailStatus<void>(failure, AssetDiagnosticCode::None, step.Error(), id);
    }

    FilePackageRangeReader reader;
    if (auto opened = reader.Open(*files_, file.View()); !opened) {
        return FailStatus<void>(failure,
                                opened.Error() == ErrorCode::NotFound
                                    ? AssetDiagnosticCode::MissingFile
                                    : AssetDiagnosticCode::None,
                                opened.Error(), id);
    }

    AssetDiagnostic readFailure;
    auto view = ReadPackageIndex(allocator, reader, read_, &readFailure);
    if (!view) {
        if (failure != nullptr) {
            *failure = readFailure;
            failure->error = view.Error();
            failure->asset = id;
        }

        return Result<void>::Failure(view.Error());
    }

    const PackageContents& contents = view.Value().contents;
    // 색인 뒤 파일이 바뀌어 다른 패키지가 그 자리에 있다(EditorPackageSet::Open과 같은 진단).
    if (!(contents.header.assetId == id)) {
        return FailStatus<void>(failure, AssetDiagnosticCode::ExternalModification,
                                ErrorCode::StaleHandle, id);
    }

    if (contents.importRecord.IsEmpty()) {
        return FailStatus<void>(failure, AssetDiagnosticCode::None, ErrorCode::InvalidArgument, id);
    }

    auto record = ReadImportRecord(allocator, id, contents.importRecord);
    if (!record) {
        if (record.Error() == ErrorCode::Unsupported) {
            return FailStatus<void>(failure, AssetDiagnosticCode::NewerVersion,
                                    ErrorCode::Unsupported, id);
        }

        auto failed =
            FailStatus<void>(failure, AssetDiagnosticCode::CorruptPackage, record.Error(), id);
        if (failure != nullptr && record.Error() != ErrorCode::OutOfMemory) {
            failure->section = static_cast<uint32>(PackageSectionKind::ImportRecord);
        }

        return failed;
    }

    const ImportRecord& recorded = record.Value();
    out.id = id;
    out.owner = recorded.owner;
    out.ownerPath.Clear();
    out.importerId = recorded.importerId;
    out.importerVersion = recorded.importerVersion;
    out.importer = AssetDiagnosticCode::None;
    out.sources.Clear();
    if (auto named = out.importerName.TryAssign(recorded.importerName.View()); !named) {
        return FailStatus<void>(failure, AssetDiagnosticCode::None, named.Error(), id);
    }

    // 참조 기록(출력 패키지): owner와 그 색인 경로만(FindById는 할당하지 않는다 — 없음·같은 id
    // 둘이면 경로를 비운다).
    if (!out.Owned()) {
        if (auto owner = index_.FindById(recorded.owner); owner) {
            if (auto assigned = out.ownerPath.TryAssign(owner.Value()->path.View()); !assigned) {
                return FailStatus<void>(failure, AssetDiagnosticCode::None, assigned.Error(), id);
            }
        }

        return Result<void>::Success();
    }

    // 임포터 상태.
    const AssetImporter* importer =
        importers_ != nullptr ? importers_->FindById(recorded.importerId) : nullptr;
    if (importer == nullptr) {
        out.importer = AssetDiagnosticCode::UnsupportedSourceFormat;
    } else if (importer->Info().version > recorded.importerVersion) {
        out.importer = AssetDiagnosticCode::ImporterOutdated;
    } else if (importer->Info().version < recorded.importerVersion) {
        out.importer = AssetDiagnosticCode::NewerVersion;
    }

    // 원본 상태: 지금 루트로 풀고 크기 → SHA-256(고정 버퍼로 흘려 읽기).
    AssetSourceRoots roots(allocator, *layout_);
    if (auto loaded = roots.Load(*files_, failure); !loaded) {
        return loaded;
    }

    Array<uint8> buffer(allocator);
    step = buffer.TryResize(ImportStatusReadBytes);
    if (step) {
        step = out.sources.TryReserve(recorded.sources.Size());
    }

    if (!step) {
        return FailStatus<void>(failure, AssetDiagnosticCode::None, step.Error(), id);
    }

    String path(allocator);
    for (const ImportSource& source : recorded.sources) {
        AssetImportSourceStatus status(allocator);
        step = status.role.TryAssign(source.role.View());
        if (step) {
            step = status.root.TryAssign(source.root.View());
        }

        if (step) {
            step = status.relative.TryAssign(source.relativePath.View());
        }

        if (!step) {
            return FailStatus<void>(failure, AssetDiagnosticCode::None, step.Error(), id);
        }

        path.Clear();
        if (auto resolved = roots.Resolve(source.root.View(), source.relativePath.View(), path);
            !resolved) {
            if (resolved.Error() == ErrorCode::OutOfMemory) {
                return FailStatus<void>(failure, AssetDiagnosticCode::None, resolved.Error(), id);
            }

            // 모르는 루트는 원본 없음(재임포트와 같다), 위치 규칙 위반은 잘못된 원본이다(읽은
            // 기록은 같은 규칙을 이미 통과했다 — Resolve 계약대로 둔다).
            status.state = resolved.Error() == ErrorCode::NotFound
                               ? AssetDiagnosticCode::MissingSource
                               : AssetDiagnosticCode::InvalidSource;
            status.error = resolved.Error();
        } else {
            status.state = CheckImportSource(*files_, path.View(), source.bytes, source.sha256,
                                             buffer.View(), status.error);
        }

        // 자리는 위에서 잡았다.
        (void)out.sources.TryPushBack(rocks::Move(status));
    }

    return Result<void>::Success();
}

} // namespace rocks
