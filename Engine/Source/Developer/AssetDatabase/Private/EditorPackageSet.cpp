#include <AssetDatabase/EditorPackageSet.h>
#include <AssetDatabase/ImportRecord.h>
#include <AssetRuntime/Package.generated.h>
#include <AssetRuntime/Package.h>
#include <AssetRuntime/PackageRangeReader.h>
#include <Core/Algorithms.h>
#include <Object/ConstructionContext.h>

// 에디터 패키지 집합의 열기다(Docs/Design/AssetSystem.ko.md 7.1절 15 — A2c2a).
namespace rocks {

namespace {

// 진단 하나를 채운다(Open 실패·경고 공통).
AssetDiagnostic EditorDiagnostic(AssetDiagnosticCode code, ErrorCode error, AssetId asset,
                                 uint32 section = 0) noexcept {
    AssetDiagnostic diagnostic;
    diagnostic.code = code;
    diagnostic.error = error;
    diagnostic.asset = asset;
    diagnostic.section = section;
    return diagnostic;
}

// 실패를 out에 적고 error로 돌려준다.
template <class T>
Result<T> EditorFail(AssetDiagnostic* out, const AssetDiagnostic& diagnostic) noexcept {
    if (out != nullptr) {
        *out = diagnostic;
    }

    return Result<T>::Failure(diagnostic.error);
}

// object import를 만드는 의존 종류인지다(5.7절 5 — 닫힘을 넓히는 종류).
bool OpensWith(PackageDependencyKind kind) noexcept {
    return kind == PackageDependencyKind::Hard || kind == PackageDependencyKind::EditorOnly;
}

} // namespace

// 닫힘에 넣은 패키지 하나다. state는 Prepare를 마친 상태(빼낸 패키지는 excluded)다.
struct EditorPackageSet::Pending {
    explicit Pending(Allocator& allocator) noexcept : path(allocator), fileName(allocator) {
    }

    AssetId id{};
    // 처음 가리킨 패키지다(요청한 패키지는 null).
    AssetId referrer{};
    // Content 경로를 포함한 파일 경로와 Content 상대 이름이다.
    String path;
    String fileName;
    UniquePtr<PackageLoadState> state;
    bool readOnly = false;
    bool excluded = false;
};

EditorPackageSet::EditorPackageSet(Allocator& allocator,
                                   const EditorPackageSetConfig& config) noexcept
    : allocator_(&allocator), config_(config), open_(allocator), roots_(allocator) {
}

EditorPackageSet::~EditorPackageSet() {
    // 패키지 정책 Registry의 객체는 Discard로만 죽는다(GC가 Discard하지 않은 객체를 거두지
    // 않게 — 로더 종료와 같은 순서). 소멸 중에는 Borrow·트랜잭션이 없어 Discard가 실패할 일이 없고,
    // 실패해도 Registry 파괴가 객체를 거둔다.
    if (registry_.Get() != nullptr) {
        for (const EditorPackageInfo& info : open_) {
            (void)registry_->Discard(registry_->OwnerAccess(), info.package);
        }
    }

    roots_.Clear();
    open_.Clear();
    registry_.Reset();
    cache_.Reset();
    observer_.Reset();
}

Result<void> EditorPackageSet::Initialize() noexcept {
    if (registry_.Get() != nullptr || config_.types == nullptr || config_.files == nullptr ||
        config_.layout == nullptr || config_.index == nullptr || config_.fileCacheCapacity == 0 ||
        config_.observerMaxKeys == 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    Allocator& allocator = *allocator_;
    auto observer =
        UniquePtr<ReferenceObserver>::TryMake(allocator, allocator, config_.observerMaxKeys);
    if (!observer) {
        return Result<void>::Failure(observer.Error());
    }

    auto cache = UniquePtr<PackageFileCache>::TryMake(allocator, allocator, *config_.files,
                                                      config_.fileCacheCapacity);
    if (!cache) {
        return Result<void>::Failure(cache.Error());
    }

    RegistryOptions options = config_.registryOptions;
    options.policy.packageNativeType = NativeTypeKey<Package>();
    options.policy.rejectWorldOnly = true;
    options.referenceObserver = observer.Value().Get();
    auto registry = UniquePtr<ObjectRegistry>::TryMake(allocator, allocator, *config_.types,
                                                       config_.registryContext, options);
    if (!registry) {
        return Result<void>::Failure(registry.Error());
    }

    observer_ = Move(observer.Value());
    cache_ = Move(cache.Value());
    registry_ = Move(registry.Value());
    return Result<void>::Success();
}

usize EditorPackageSet::LowerBound(AssetId id) const noexcept {
    return algo::LowerBound(open_.View(), id,
                            [](const EditorPackageInfo& info, AssetId key) noexcept {
                                return GuidLess(info.id.value, key.value);
                            });
}

const EditorPackageInfo* EditorPackageSet::Find(AssetId id) const noexcept {
    const usize at = LowerBound(id);
    return at < open_.Size() && open_[at].id == id ? &open_[at] : nullptr;
}

Result<ObjectHandle> EditorPackageSet::FindPackage(AssetId asset) noexcept {
    const EditorPackageInfo* info = Find(asset);
    if (info == nullptr) {
        return Result<ObjectHandle>::Failure(ErrorCode::NotFound);
    }

    return Result<ObjectHandle>::Success(info->package);
}

Result<ObjectHandle> EditorPackageSet::Open(AssetId id, Array<EditorOpenWarning>* warnings,
                                            AssetDiagnostic* failure) noexcept {
    if (registry_.Get() == nullptr || id.IsNull()) {
        return EditorFail<ObjectHandle>(
            failure, EditorDiagnostic(AssetDiagnosticCode::None, ErrorCode::InvalidArgument, id));
    }

    return OpenEntry(id, warnings, failure);
}

Result<ObjectHandle> EditorPackageSet::Open(const LogicalPath& path,
                                            Array<EditorOpenWarning>* warnings,
                                            AssetDiagnostic* failure) noexcept {
    if (registry_.Get() == nullptr) {
        return EditorFail<ObjectHandle>(
            failure, EditorDiagnostic(AssetDiagnosticCode::None, ErrorCode::InvalidArgument, {}));
    }

    auto found = config_.index->FindByPath(path);
    if (!found) {
        const AssetDiagnosticCode code = found.Error() == ErrorCode::AlreadyExists
                                             ? AssetDiagnosticCode::DuplicatePath
                                             : AssetDiagnosticCode::None;
        return EditorFail<ObjectHandle>(failure, EditorDiagnostic(code, found.Error(), {}));
    }

    return OpenEntry(found.Value()->id, warnings, failure);
}

Result<void> EditorPackageSet::PreparePending(Pending& pending,
                                              AssetDiagnostic& diagnostic) noexcept {
    Allocator& allocator = *allocator_;
    const AssetId id = pending.id;
    auto fail = [&](AssetDiagnosticCode code, ErrorCode error, uint32 section = 0) noexcept {
        diagnostic = EditorDiagnostic(code, error, id, section);
        return Result<void>::Failure(error);
    };

    // 색인에서 파일을 찾는다(요청한 패키지가 없으면 진단 None, 의존이면 MissingDependency).
    auto found = config_.index->FindById(id);
    if (!found) {
        if (found.Error() == ErrorCode::AlreadyExists) {
            return fail(AssetDiagnosticCode::DuplicateAssetId, found.Error());
        }

        return fail(pending.referrer.IsNull() ? AssetDiagnosticCode::None
                                              : AssetDiagnosticCode::MissingDependency,
                    found.Error());
    }

    const StringView content = config_.layout->ContentDirectory();
    const StringView fileName = found.Value()->fileName.View();
    Result<void> step = pending.fileName.TryAssign(fileName);
    if (step) {
        step = pending.path.TryAssign(content);
    }

    if (step) {
        step = pending.path.TryAppend("/");
    }

    if (step) {
        step = pending.path.TryAppend(fileName);
    }

    if (!step) {
        return fail(AssetDiagnosticCode::None, step.Error());
    }

    // 파일을 열어 Defer로 Prepare한다(Bulk 내용은 읽지 않는다 — 지연 읽기 저장소가 읽는다).
    FilePackageRangeReader reader;
    if (auto opened = reader.Open(*config_.files, pending.path.View()); !opened) {
        return fail(opened.Error() == ErrorCode::NotFound ? AssetDiagnosticCode::MissingFile
                                                          : AssetDiagnosticCode::None,
                    opened.Error());
    }

    PackageLoadOptions options;
    options.bulk = PackageBulkMode::Defer;
    options.read = config_.read;
    options.read.expectedKind = PackageKind::Editor;
    auto state = UniquePtr<PackageLoadState>::TryMake(allocator, allocator, *registry_,
                                                      *config_.types, Access(), options);
    if (!state) {
        return fail(AssetDiagnosticCode::None, state.Error());
    }

    pending.state = Move(state.Value());
    PackageLoadState& loaded = *pending.state;
    if (auto prepared = loaded.Prepare(reader, &diagnostic); !prepared) {
        if (diagnostic.asset.IsNull()) {
            diagnostic.asset = id;
        }

        return prepared;
    }

    // 색인 뒤 파일이 바뀌어 다른 패키지가 그 자리에 있다.
    if (!(loaded.Id() == id)) {
        return fail(AssetDiagnosticCode::ExternalModification, ErrorCode::StaleHandle);
    }

    // ImportRecord와 ImportOwner 대조(다른 스키마·더 새 버전은 대조하지 않고 읽기 전용).
    const PackageContents& contents = loaded.Contents();
    ImportRecord record(allocator);
    const ImportRecord* checked = nullptr;
    bool unknownRecord = false;
    if (!contents.importRecord.IsEmpty()) {
        auto read = ReadImportRecord(allocator, id, contents.importRecord);
        if (read) {
            record = Move(read.Value());
            checked = &record;
        } else if (read.Error() == ErrorCode::Unsupported) {
            unknownRecord = true;
        } else if (read.Error() == ErrorCode::OutOfMemory) {
            return fail(AssetDiagnosticCode::None, read.Error());
        } else {
            return fail(AssetDiagnosticCode::CorruptPackage, read.Error(),
                        static_cast<uint32>(PackageSectionKind::ImportRecord));
        }
    }

    if (!unknownRecord) {
        if (auto owner = CheckImportOwner(id, checked, contents.dependencies.View()); !owner) {
            return fail(AssetDiagnosticCode::CorruptPackage, owner.Error(),
                        static_cast<uint32>(PackageSectionKind::Dependencies));
        }
    }

    pending.readOnly = loaded.IsReadOnly() || unknownRecord;

    // 파일 bulk 저장소를 건다.
    uint64 bulkOffset = 0;
    for (const PackageSectionEntry& section : contents.sections) {
        if (section.kind == PackageSectionKind::Bulk) {
            bulkOffset = section.offset;
        }
    }

    auto store = UniquePtr<FilePackageBulkStore>::TryMake(allocator, allocator, *cache_);
    if (!store) {
        return fail(AssetDiagnosticCode::None, store.Error());
    }

    if (auto attached = store.Value()->Attach(pending.path.View(), contents.fingerprint, id,
                                              bulkOffset, contents.bulkEntries.View());
        !attached) {
        return fail(AssetDiagnosticCode::None, attached.Error());
    }

    UniquePtr<PackageBulkStore> attachedStore(Move(store.Value()));
    if (auto set = loaded.SetBulkStore(attachedStore); !set) {
        return fail(AssetDiagnosticCode::None, set.Error());
    }

    return Result<void>::Success();
}

Result<ObjectHandle> EditorPackageSet::OpenEntry(AssetId id, Array<EditorOpenWarning>* warnings,
                                                 AssetDiagnostic* failure) noexcept {
    using Out = Result<ObjectHandle>;
    if (const EditorPackageInfo* open = Find(id); open != nullptr) {
        return Out::Success(open->package);
    }

    Allocator& allocator = *allocator_;
    auto oom = [&](ErrorCode error) noexcept {
        return EditorFail<ObjectHandle>(failure,
                                        EditorDiagnostic(AssetDiagnosticCode::None, error, id));
    };

    // 닫힘을 너비 우선으로 넓힌다. seen은 넣은 id(id 순)다.
    Array<Pending> pending(allocator);
    Array<AssetId> seen(allocator);
    Array<EditorOpenWarning> excluded(allocator);
    if (auto pushed = pending.TryPushBack(Pending(allocator)); !pushed) {
        return oom(pushed.Error());
    }

    pending[0].id = id;
    if (auto pushed = seen.TryPushBack(id); !pushed) {
        return oom(pushed.Error());
    }

    for (usize index = 0; index < pending.Size(); ++index) {
        AssetDiagnostic diagnostic;
        if (auto prepared = PreparePending(pending[index], diagnostic); !prepared) {
            if (index == 0 || prepared.Error() == ErrorCode::OutOfMemory) {
                if (index != 0) {
                    diagnostic = EditorDiagnostic(AssetDiagnosticCode::None, prepared.Error(), id);
                }

                return EditorFail<ObjectHandle>(failure, diagnostic);
            }

            pending[index].excluded = true;
            pending[index].state.Reset();
            diagnostic.asset = pending[index].id;
            diagnostic.related = pending[index].referrer;
            if (auto pushed = excluded.TryPushBack(
                    EditorOpenWarning{EditorOpenWarningKind::ExcludedDependency, diagnostic});
                !pushed) {
                return oom(pushed.Error());
            }

            continue;
        }

        const AssetId referrer = pending[index].id;
        for (const PackageDependencyEntry& dependency :
             pending[index].state->Contents().dependencies) {
            if (!OpensWith(dependency.kind) || Find(dependency.asset) != nullptr) {
                continue;
            }

            const usize at = algo::LowerBound(seen.View(), dependency.asset,
                                              [](AssetId left, AssetId right) noexcept {
                                                  return GuidLess(left.value, right.value);
                                              });
            if (at < seen.Size() && seen[at] == dependency.asset) {
                continue;
            }

            if (auto inserted = seen.TryInsert(dependency.asset, at); !inserted) {
                return oom(inserted.Error());
            }

            if (auto pushed = pending.TryPushBack(Pending(allocator)); !pushed) {
                return oom(pushed.Error());
            }

            pending[pending.Size() - 1].id = dependency.asset;
            pending[pending.Size() - 1].referrer = referrer;
        }
    }

    // 남은 패키지를 한 트랜잭션으로 만든다(이미 열린 패키지는 resolver로 잇고 누락은 null).
    Array<PackageLoadState*> states(allocator);
    Array<usize> origin(allocator);
    for (usize index = 0; index < pending.Size(); ++index) {
        if (pending[index].excluded) {
            continue;
        }

        if (auto pushed = states.TryPushBack(pending[index].state.Get()); !pushed) {
            return oom(pushed.Error());
        }

        if (auto pushed = origin.TryPushBack(index); !pushed) {
            return oom(pushed.Error());
        }
    }

    const usize count = states.Size();
    Array<ObjectHandle> handles(allocator);
    Array<RootRef<Object>> roots(allocator);
    Array<PackageUnresolvedImport> unresolved(allocator);
    Array<EditorPackageInfo> infos(allocator);
    if (auto sized = handles.TryResize(count); !sized) {
        return oom(sized.Error());
    }

    if (auto sized = roots.TryResize(count); !sized) {
        return oom(sized.Error());
    }

    if (auto reserved = infos.TryReserve(count); !reserved) {
        return oom(reserved.Error());
    }

    ConstructionContext transaction(*registry_, Access());
    PackageInstantiateOptions options;
    options.resolver = this;
    options.imports = PackageImportPolicy::NullMissing;
    options.unresolved = &unresolved;
    AssetDiagnostic diagnostic;
    if (auto made =
            InstantiatePackages(transaction, states.View(), options, handles.View(), &diagnostic);
        !made) {
        return EditorFail<ObjectHandle>(failure, diagnostic);
    }

    // 공개 뒤에는 실패할 수 없게 표·경고 자리를 먼저 마련한다(공개한 패키지를 놓치지 않게).
    for (usize slot = 0; slot < count; ++slot) {
        Pending& source = pending[origin[slot]];
        EditorPackageInfo info(allocator);
        info.id = source.id;
        info.package = handles[slot];
        info.fingerprint = source.state->Contents().fingerprint;
        info.readOnly = source.readOnly;
        info.fileName = Move(source.fileName);
        for (const PackageUnresolvedImport& one : unresolved) {
            if (one.package == source.id) {
                if (auto pushed = info.unresolved.TryPushBack(one); !pushed) {
                    return oom(pushed.Error());
                }
            }
        }

        (void)infos.TryPushBack(Move(info));
    }

    if (auto reserved = open_.TryReserve(open_.Size() + count); !reserved) {
        return oom(reserved.Error());
    }

    if (auto reserved = roots_.TryReserve(roots_.Size() + count); !reserved) {
        return oom(reserved.Error());
    }

    if (warnings != nullptr) {
        if (auto reserved =
                warnings->TryReserve(warnings->Size() + excluded.Size() + unresolved.Size());
            !reserved) {
            return oom(reserved.Error());
        }
    }

    if (auto committed = transaction.Commit(handles.View(), roots.View()); !committed) {
        return oom(committed.Error());
    }

    for (usize slot = 0; slot < count; ++slot) {
        const usize at = LowerBound(infos[slot].id);
        (void)open_.TryInsert(Move(infos[slot]), at);
        (void)roots_.TryInsert(Move(roots[slot]), at);
    }

    if (warnings != nullptr) {
        for (const EditorOpenWarning& warning : excluded) {
            (void)warnings->TryPushBack(warning);
        }

        for (const PackageUnresolvedImport& one : unresolved) {
            EditorOpenWarning warning;
            warning.kind = EditorOpenWarningKind::UnresolvedImport;
            warning.diagnostic =
                EditorDiagnostic(one.code, one.error, one.package,
                                 static_cast<uint32>(PackageSectionKind::ObjectImports));
            warning.diagnostic.related = one.target;
            warning.diagnostic.detail = one.import;
            (void)warnings->TryPushBack(warning);
        }
    }

    return Out::Success(handles[0]);
}

} // namespace rocks
