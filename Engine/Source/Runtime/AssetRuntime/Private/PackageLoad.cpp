#include "PackageAccess.h"
#include "PackageInternal.h"

#include <AssetRuntime/BulkData.generated.h>
#include <AssetRuntime/Package.generated.h>
#include <AssetRuntime/PackageLoad.h>

namespace rocks {

namespace {

using namespace package_detail;

constexpr uint32 EditorOnlyFlag = static_cast<uint32>(FieldFlags::EditorOnly);
constexpr uint32 RequiredFlag = static_cast<uint32>(FieldFlags::Required);

ObjectGraphLoadOptions StageOptions(const PackageLoadOptions& options,
                                    detail::PackageLoadHooks& hooks) noexcept {
    ObjectGraphLoadOptions load;
    load.limits = options.limits;
    load.hooks = &hooks;
    if (options.read.expectedKind == PackageKind::Cooked) {
        // 쿠킹 데이터는 정확히 일치해야 한다(AS 5.3절): 모르는 필드·더 새 버전 거부, EditorOnly
        // 필드 없음.
        load.policy.unknownField = UnknownFieldPolicy::Reject;
        load.policy.newerVersion = NewerVersionPolicy::Reject;
        load.policy.excludedFlags = EditorOnlyFlag;
    } else {
        // 에디터 패키지는 더 새 데이터를 읽기 전용으로 연다(모르는 필드는 진단 후 건너뛰기).
        load.policy.unknownField = UnknownFieldPolicy::SkipAndReport;
        load.policy.newerVersion = NewerVersionPolicy::LoadNewerSkippingUnknown;
    }

    return load;
}

// 그래프 단계 오류를 진단으로 분류한다.
template <class T>
Result<T> ClassifyGraphFailure(AssetDiagnostic* failure, ErrorCode error, AssetId asset) noexcept {
    switch (error) {
    case ErrorCode::OutOfMemory:
    case ErrorCode::ShuttingDown:
    case ErrorCode::WrongThread:
    case ErrorCode::WrongContext:
    case ErrorCode::Busy:
        return FailAsset<T>(failure, AssetDiagnosticCode::None, error, asset,
                            static_cast<uint32>(PackageSectionKind::Objects), 0);
    case ErrorCode::NotFound:
    case ErrorCode::Unsupported:
        return FailAsset<T>(failure, AssetDiagnosticCode::UnsupportedVersion, error, asset,
                            static_cast<uint32>(PackageSectionKind::Objects), 0);
    default:
        return FailAsset<T>(failure, AssetDiagnosticCode::CorruptPackage, error, asset,
                            static_cast<uint32>(PackageSectionKind::Objects), 0);
    }
}

} // namespace

namespace detail {

PackageLoadHooks::PackageLoadHooks(Allocator& allocator, bool cooked) noexcept
    : softUses(allocator), bulkUses(allocator), cooked_(cooked) {
    observed_[0] = GeneratedType<BulkDataRef>::Id();
}

Result<void> PackageLoadHooks::Report(const FieldDiagnostic& diagnostic) noexcept {
    switch (diagnostic.kind) {
    case FieldDiagnosticKind::UnknownField:
    case FieldDiagnosticKind::NewerVersion:
        sawNewer = true;
        return Result<void>::Success();
    case FieldDiagnosticKind::RequiredMissing:
        ++missingRequired;
        // 쿠킹 패키지는 writer가 Required null을 거부하므로 누락은 손상이다.
        return cooked_ ? Result<void>::Failure(ErrorCode::InvalidArgument)
                       : Result<void>::Success();
    default:
        return Result<void>::Success();
    }
}

Result<void> PackageLoadHooks::CollectSoftReference(const SoftReferenceInfo& info) noexcept {
    SoftUse use;
    use.asset = info.asset;
    use.editorOnly = (info.effectiveFlags & EditorOnlyFlag) != 0;
    use.required = (info.effectiveFlags & RequiredFlag) != 0;
    return softUses.TryPushBack(use);
}

Span<const TypeId> PackageLoadHooks::ObservedStructs() noexcept {
    return Span<const TypeId>(observed_, 1);
}

Result<void> PackageLoadHooks::ObserveStruct(const FieldCodecFrame& frame, TypeId type,
                                             const void* nativeKey, const void* value) noexcept {
    (void)frame;
    (void)nativeKey;
    if (!(type == observed_[0])) {
        return Result<void>::Success();
    }

    const BulkDataRef& ref = *static_cast<const BulkDataRef*>(value);
    if (ref.IsEmpty()) {
        return Result<void>::Success();
    }

    return bulkUses.TryPushBack(BulkUse{ref.bulkId, ref.rawBytes, ref.sha256});
}

} // namespace detail

PackageLoadState::PackageLoadState(Allocator& allocator, ObjectRegistry& registry,
                                   TypeRegistry& types, const ObjectAccessContext& access,
                                   const PackageLoadOptions& options) noexcept
    : allocator_(allocator), registry_(registry), types_(types), options_(options),
      hooks_(allocator, options.read.expectedKind == PackageKind::Cooked),
      stage_(allocator, registry, types, access, StageOptions(options, hooks_)), file_(allocator),
      contents_(allocator), nodes_(allocator), importTypes_(allocator), importPaths_(allocator),
      bulkBytes_(registry.ObjectAllocator()), bulkSlots_(registry.ObjectAllocator()) {
    options_.read.types = &types;
}

PackageLoadState::~PackageLoadState() = default;

StringView PackageLoadState::ImportPath(usize index) const noexcept {
    return index < importPaths_.Size() ? importPaths_[index].View() : StringView();
}

Result<void> PackageLoadState::Prepare(Array<uint8>&& file, AssetDiagnostic* failure) noexcept {
    if (attempted_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    attempted_ = true;
    file_ = Move(file);
    auto read = ReadPackage(allocator_, file_.View(), options_.read, failure);
    if (!read) {
        return Result<void>::Failure(read.Error());
    }

    contents_ = Move(read.Value());
    const AssetId asset = contents_.header.assetId;
    readOnly_ = contents_.readOnly;

    // 그래프 노드 표·import 타입·import 경로.
    if (!nodes_.TryReserve(contents_.exports.Size()) ||
        !importTypes_.TryReserve(contents_.imports.Size()) ||
        !importPaths_.TryReserve(contents_.imports.Size())) {
        return FailAsset<void>(failure, AssetDiagnosticCode::None, ErrorCode::OutOfMemory, asset);
    }

    for (const PackageExportEntry& entry : contents_.exports) {
        GraphNodeDesc node;
        node.type = entry.type;
        node.outer = entry.outerIndex;
        node.name = entry.name;
        (void)nodes_.TryPushBack(node);
    }

    for (usize index = 0; index < contents_.imports.Size(); ++index) {
        (void)importTypes_.TryPushBack(contents_.imports[index].type);
        auto path = importPaths_.TryEmplaceBack(allocator_);
        if (!path) {
            return FailAsset<void>(failure, AssetDiagnosticCode::None, path.Error(), asset);
        }

        if (auto built = ImportPathOf(contents_.imports.View(), index, *path.Value()); !built) {
            return FailAsset<void>(failure, AssetDiagnosticCode::None, built.Error(), asset);
        }
    }

    // bulk 압축 해제·SHA-256 확인.
    if (auto decoded =
            DecodePackageBulk(contents_, bulkBytes_, bulkSlots_, options_.maxBulkRawBytes, failure);
        !decoded) {
        return decoded;
    }

    // 그래프 1단계(Registry에 닿지 않는다 — worker에서 해도 된다).
    ObjectGraphInput input;
    input.bytes = contents_.objects;
    input.nodes = nodes_.View();
    input.hasBase = true;
    input.base = OuterSummary::ForNewOutermost(true);
    input.importTypes = importTypes_.View();
    if (auto valid = stage_.Validate(input); !valid) {
        return ClassifyGraphFailure<void>(failure, valid.Error(), asset);
    }

    readOnly_ = readOnly_ || hooks_.sawNewer;
    if (contents_.header.kind == PackageKind::Cooked && readOnly_) {
        return FailAsset<void>(failure, AssetDiagnosticCode::UnsupportedVersion,
                               ErrorCode::Unsupported, asset,
                               static_cast<uint32>(PackageSectionKind::Objects), 0);
    }

    if (auto checked = CrossCheck(failure); !checked) {
        return checked;
    }

    prepared_ = true;
    return Result<void>::Success();
}

Result<void> PackageLoadState::CrossCheck(AssetDiagnostic* failure) noexcept {
    const AssetId asset = contents_.header.assetId;
    auto corrupt = [&](PackageSectionKind section, uint64 detail) noexcept {
        return FailAsset<void>(failure, AssetDiagnosticCode::CorruptPackage,
                               ErrorCode::InvalidArgument, asset, static_cast<uint32>(section),
                               detail);
    };

    // export 1(대표 에셋)은 Asset 파생이어야 한다.
    auto isAsset = types_.IsA(contents_.exports[0].type, GeneratedType<Asset>::Id());
    if (!isAsset || !isAsset.Value()) {
        return corrupt(PackageSectionKind::Exports, 1);
    }

    // 노드 저장 버전 = Summary 타입 표 버전(에디터의 더 새 버전은 읽기 전용으로 이미 통과).
    for (usize node = 0; node < contents_.exports.Size(); ++node) {
        auto stored = stage_.StoredVersion(node);
        if (!stored) {
            return corrupt(PackageSectionKind::Objects, node);
        }

        bool matched = false;
        for (const PackageTypeVersion& row : contents_.summary.types) {
            if (row.type == contents_.exports[node].type) {
                matched = row.version == stored.Value();
                break;
            }
        }

        if (!matched) {
            return corrupt(PackageSectionKind::Summary, node);
        }
    }

    // BulkDataRef ↔ BulkIndex.
    for (const detail::PackageLoadHooks::BulkUse& use : hooks_.bulkUses) {
        if (use.bulkId > contents_.bulkEntries.Size()) {
            return corrupt(PackageSectionKind::BulkIndex, use.bulkId);
        }

        const PackageBulkEntry& entry = contents_.bulkEntries[use.bulkId - 1];
        if (entry.rawBytes != use.rawBytes || !(entry.sha256 == use.sha256)) {
            return corrupt(PackageSectionKind::BulkIndex, use.bulkId);
        }
    }

    // Dependencies ↔ 디코딩 결과(writer와 같은 합성 규칙).
    const Span<const GraphExternalUse> externals = stage_.ExternalUses();
    Array<DependencyUse> uses(allocator_);
    for (usize index = 0; index < externals.Size(); ++index) {
        if (externals[index].occurrences == 0) {
            continue;
        }

        DependencyUse use;
        use.asset = contents_.imports[index].asset;
        use.hard = true;
        use.editorOnly = (externals[index].commonFlags & EditorOnlyFlag) != 0;
        use.required = externals[index].required;
        if (auto pushed = uses.TryPushBack(use); !pushed) {
            return FailAsset<void>(failure, AssetDiagnosticCode::None, pushed.Error(), asset);
        }
    }

    for (const detail::PackageLoadHooks::SoftUse& soft : hooks_.softUses) {
        DependencyUse use;
        use.asset = soft.asset;
        use.hard = false;
        use.editorOnly = soft.editorOnly;
        use.required = soft.required;
        if (auto pushed = uses.TryPushBack(use); !pushed) {
            return FailAsset<void>(failure, AssetDiagnosticCode::None, pushed.Error(), asset);
        }
    }

    Array<PackageDependencyEntry> decoded(allocator_);
    if (auto merged = MergeDependencyUses(allocator_, uses.View(), asset, decoded); !merged) {
        return FailAsset<void>(failure, AssetDiagnosticCode::None, merged.Error(), asset);
    }

    decoded.Sort(DependencyLess);
    // 파일의 Hard·Soft·EditorOnly 항목만 비교한다(Build·ImportOwner는 대조하지 않는다 —
    // ImportOwner의 ImportRecord 대조는 A2).
    usize cursor = 0;
    usize compared = 0;
    for (const PackageDependencyEntry& entry : contents_.dependencies) {
        if (entry.kind == PackageDependencyKind::Build ||
            entry.kind == PackageDependencyKind::ImportOwner) {
            continue;
        }

        ++compared;
        if (readOnly_) {
            continue;
        }

        if (cursor >= decoded.Size() || !(decoded[cursor].asset == entry.asset) ||
            decoded[cursor].kind != entry.kind || decoded[cursor].required != entry.required) {
            return corrupt(PackageSectionKind::Dependencies, cursor);
        }

        ++cursor;
    }

    if (readOnly_) {
        // 읽기 전용: 디코딩 결과 ⊆ Dependencies((asset, kind) 기준).
        for (const PackageDependencyEntry& want : decoded) {
            bool found = false;
            for (const PackageDependencyEntry& have : contents_.dependencies) {
                found = found || (have.asset == want.asset && have.kind == want.kind);
            }

            if (!found) {
                return corrupt(PackageSectionKind::Dependencies, 0);
            }
        }
    } else if (cursor != decoded.Size() || compared != decoded.Size()) {
        return corrupt(PackageSectionKind::Dependencies, cursor);
    }

    // 모든 import 에셋은 Hard 또는 EditorOnly 의존이다.
    for (usize index = 0; index < contents_.imports.Size(); ++index) {
        bool found = false;
        for (const PackageDependencyEntry& have : contents_.dependencies) {
            found = found || (have.asset == contents_.imports[index].asset &&
                              (have.kind == PackageDependencyKind::Hard ||
                               have.kind == PackageDependencyKind::EditorOnly));
        }

        if (!found) {
            return corrupt(PackageSectionKind::ObjectImports, index);
        }
    }

    return Result<void>::Success();
}

Result<void> InstantiatePackages(ConstructionContext& transaction,
                                 Span<PackageLoadState* const> packages,
                                 PackageImportResolver* resolver, Span<ObjectHandle> outPackages,
                                 AssetDiagnostic* failure) noexcept {
    if (packages.IsEmpty() || outPackages.Size() != packages.Size()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    for (usize index = 0; index < packages.Size(); ++index) {
        if (packages[index] == nullptr || !packages[index]->IsPrepared() ||
            &packages[index]->registry_ != &transaction.Registry()) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        for (usize other = 0; other < index; ++other) {
            if (packages[other]->Id() == packages[index]->Id()) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }
        }
    }

    // allocate-all: Package와 그래프 노드.
    for (usize index = 0; index < packages.Size(); ++index) {
        PackageLoadState& state = *packages[index];
        auto package = transaction.Create<Package>(GeneratedType<Package>::Id());
        if (!package) {
            return ClassifyGraphFailure<void>(failure, package.Error(), state.Id());
        }

        outPackages[index] = package.Value();
        if (auto created = state.stage_.CreateObjects(transaction, package.Value()); !created) {
            return ClassifyGraphFailure<void>(failure, created.Error(), state.Id());
        }
    }

    // fixup: import 해석 → 적용.
    for (usize index = 0; index < packages.Size(); ++index) {
        PackageLoadState& state = *packages[index];
        const Span<const PackageImportEntry> imports = state.contents_.imports.View();
        Array<ObjectHandle> targets(state.allocator_);
        if (auto reserved = targets.TryReserve(imports.Size()); !reserved) {
            return FailAsset<void>(failure, AssetDiagnosticCode::None, reserved.Error(),
                                   state.Id());
        }

        for (usize position = 0; position < imports.Size(); ++position) {
            const PackageImportEntry& entry = imports[position];
            ObjectHandle package{};
            for (usize other = 0; other < packages.Size(); ++other) {
                if (packages[other]->Id() == entry.asset) {
                    package = outPackages[other];
                }
            }

            if (package.IsNull()) {
                auto found = resolver != nullptr
                                 ? resolver->FindPackage(entry.asset)
                                 : Result<ObjectHandle>::Failure(ErrorCode::NotFound);
                if (!found) {
                    return FailAsset<void>(failure,
                                           found.Error() == ErrorCode::NotFound
                                               ? AssetDiagnosticCode::MissingDependency
                                               : AssetDiagnosticCode::None,
                                           found.Error(), state.Id(),
                                           static_cast<uint32>(PackageSectionKind::ObjectImports),
                                           position, entry.asset);
                }

                package = found.Value();
            }

            auto target = transaction.FindObject(package, state.ImportPath(position));
            if (!target) {
                return FailAsset<void>(
                    failure,
                    target.Error() == ErrorCode::NotFound ? AssetDiagnosticCode::MissingDependency
                                                          : AssetDiagnosticCode::None,
                    target.Error(), state.Id(),
                    static_cast<uint32>(PackageSectionKind::ObjectImports), position, entry.asset);
            }

            auto type = transaction.AcquireType(target.Value());
            if (!type) {
                return FailAsset<void>(failure, AssetDiagnosticCode::None, type.Error(), state.Id(),
                                       static_cast<uint32>(PackageSectionKind::ObjectImports),
                                       position, entry.asset);
            }

            auto derived = state.types_.IsA(type.Value().Id(), entry.type);
            if (!derived || !derived.Value()) {
                return FailAsset<void>(
                    failure, AssetDiagnosticCode::TypeMismatch, ErrorCode::TypeMismatch, state.Id(),
                    static_cast<uint32>(PackageSectionKind::ObjectImports), position, entry.asset);
            }

            (void)targets.TryPushBack(target.Value());
        }

        if (auto applied = state.stage_.ApplyFields(transaction, targets.View()); !applied) {
            if (applied.Error() == ErrorCode::TypeMismatch) {
                return FailAsset<void>(failure, AssetDiagnosticCode::TypeMismatch, applied.Error(),
                                       state.Id(), static_cast<uint32>(PackageSectionKind::Objects),
                                       0);
            }

            return ClassifyGraphFailure<void>(failure, applied.Error(), state.Id());
        }
    }

    for (usize index = 0; index < packages.Size(); ++index) {
        PackageLoadState& state = *packages[index];
        if (auto checked = state.stage_.CheckRequired(transaction); !checked) {
            return ClassifyGraphFailure<void>(failure, checked.Error(), state.Id());
        }
    }

    // prepare: Package 정체성·bulk, 대표 에셋 id(Commit 전 — AS 6.1절).
    for (usize index = 0; index < packages.Size(); ++index) {
        PackageLoadState& state = *packages[index];
        auto package = transaction.GetForWrite<Package>(outPackages[index]);
        if (!package) {
            return FailAsset<void>(failure, AssetDiagnosticCode::None, package.Error(), state.Id());
        }

        detail::PackageAccess::SetPackage(*package.Value(), state.Id(), state.contents_.header.kind,
                                          Move(state.bulkBytes_), Move(state.bulkSlots_));
        auto representative = transaction.GetForWrite<Asset>(state.stage_.Nodes()[0]);
        if (!representative) {
            return FailAsset<void>(failure, AssetDiagnosticCode::None, representative.Error(),
                                   state.Id());
        }

        detail::PackageAccess::SetAssetId(*representative.Value(), state.Id());
    }

    return Result<void>::Success();
}

} // namespace rocks
