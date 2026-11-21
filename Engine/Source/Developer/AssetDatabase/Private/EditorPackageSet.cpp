#include <AssetDatabase/EditorPackageSet.h>
#include <AssetDatabase/ImportRecord.h>
#include <AssetRuntime/Package.generated.h>
#include <AssetRuntime/Package.h>
#include <AssetRuntime/PackageRangeReader.h>
#include <AssetRuntime/PackageWriter.h>
#include <Core/Algorithms.h>
#include <Core/Build/BuildInfo.h>
#include <Core/FileError.h>
#include <Object/ConstructionContext.h>

// 에디터 패키지 집합의 열기·만들기·저장이다(Docs/Design/AssetSystem.ko.md 7.1절 15 — A2c2a·A2c2b).
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

// 진단을 받는 호출이 실패했을 때 진단을 마저 채운다: 호출이 진단을 채우지 않은 실패(sink·할당·IO —
// 진단 None)도 그 오류를 남기고, 에셋이 비었으면 이 패키지로 둔다.
Result<void> EditorComplete(AssetDiagnostic& diagnostic, ErrorCode error, AssetId asset) noexcept {
    if (diagnostic.error == ErrorCode::None) {
        diagnostic.code = AssetDiagnosticCode::None;
        diagnostic.error = error;
    }

    if (diagnostic.asset.IsNull()) {
        diagnostic.asset = asset;
    }

    return Result<void>::Failure(error);
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

// guid 문자열(36자 소문자 하이픈 형식)의 길이와 하이픈 위치다.
constexpr usize EditorGuidTextBytes = 36;
constexpr usize EditorGuidHyphens[] = {8, 13, 18, 23};
constexpr StringView EditorTempSuffix = ".tmp";
static_assert(1 + EditorGuidTextBytes + 4 == TemporaryFileSuffixBytes,
              "Save temporary suffix must match the layout path limit");

bool EditorIsGuidHyphen(usize index) noexcept {
    for (usize hyphen : EditorGuidHyphens) {
        if (index == hyphen) {
            return true;
        }
    }

    return false;
}

// value를 36자 소문자 하이픈 형식으로 붙인다(high 16자리 다음 low 16자리).
Result<void> EditorAppendGuidText(String& out, Guid value) noexcept {
    constexpr char Digits[] = "0123456789abcdef";
    constexpr usize NibbleBits = 4;
    constexpr usize WordNibbles = 16;
    char text[EditorGuidTextBytes];
    usize nibble = 0;
    for (usize at = 0; at < EditorGuidTextBytes; ++at) {
        if (EditorIsGuidHyphen(at)) {
            text[at] = '-';
            continue;
        }

        const uint64 word = nibble < WordNibbles ? value.high : value.low;
        const usize shift = (WordNibbles - 1 - nibble % WordNibbles) * NibbleBits;
        text[at] = Digits[(word >> shift) & 0xf];
        ++nibble;
    }

    return out.TryAppend(StringView(text, EditorGuidTextBytes));
}

// 저장 임시 파일 이름(정확히 "<이름>.rasset.<36자 소문자 guid>.tmp", 이름은 비지 않음)인지다.
bool EditorIsOrphanTempName(StringView name) noexcept {
    const usize tail = 1 + EditorGuidTextBytes + EditorTempSuffix.ByteCount();
    const usize minimum = 1 + EditorPackageExtension.ByteCount() + tail;
    if (name.ByteCount() < minimum ||
        !name.EndsWith(EditorTempSuffix, StringComparison::CaseSensitive)) {
        return false;
    }

    const usize guidStart = name.ByteCount() - EditorTempSuffix.ByteCount() - EditorGuidTextBytes;
    if (name.Data()[guidStart - 1] != '.') {
        return false;
    }

    for (usize at = 0; at < EditorGuidTextBytes; ++at) {
        const char c = name.Data()[guidStart + at];
        const bool valid =
            EditorIsGuidHyphen(at) ? c == '-' : ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
        if (!valid) {
            return false;
        }
    }

    return StringView(name.Data(), guidStart - 1).EndsWith(EditorPackageExtension);
}

// 디렉터리 열거에서 고아 임시 파일 이름을 모은다.
struct EditorOrphanContext {
    Array<String>* names;
    ErrorCode error = ErrorCode::None;
};

bool EditorCollectOrphan(const platform::DirectoryEntry& entry, void* raw) noexcept {
    auto* context = static_cast<EditorOrphanContext*>(raw);
    const StringView name(entry.name, entry.nameBytes);
    if (entry.info.type != platform::FileType::File || entry.info.isLink || !name.IsValidUtf8() ||
        !EditorIsOrphanTempName(name)) {
        return true;
    }

    String copy(context->names->GetAllocator());
    if (auto assigned = copy.TryAssign(name); !assigned) {
        context->error = assigned.Error();
        return false;
    }

    if (auto pushed = context->names->TryPushBack(Move(copy)); !pushed) {
        context->error = pushed.Error();
        return false;
    }

    return true;
}

// 저장 대상 절대 경로(언제나 Content 아래 — '/'가 있다)의 부모 디렉터리 길이다(마지막 '/' 앞까지).
usize EditorParentBytes(StringView path) noexcept {
    usize slash = path.ByteCount();
    while (path.Data()[slash - 1] != '/') {
        --slash;
    }

    return slash - 1;
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
    // Package로 옮겨 간 파일 저장소다(정보에 남긴다).
    FilePackageBulkStore* store = nullptr;
    bool readOnly = false;
    bool excluded = false;
};

// 저장 판정의 결과다(교체 전 — 경로·쓰기 입력).
struct EditorPackageSet::SavePlan {
    explicit SavePlan(Allocator& allocator) noexcept
        : target(allocator), fileName(allocator), tags(allocator), extra(allocator) {
    }

    // 대상 절대 경로와 Content 상대 이름이다.
    String target;
    String fileName;
    Array<uint8> tags;
    Array<PackageDependencyEntry> extra;
    uint64 startGeneration = 0;
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

Result<ObjectHandle> EditorPackageSet::Open(AssetId id, Array<EditorWarning>* warnings,
                                            AssetDiagnostic* failure) noexcept {
    if (registry_.Get() == nullptr || id.IsNull()) {
        return EditorFail<ObjectHandle>(
            failure, EditorDiagnostic(AssetDiagnosticCode::None, ErrorCode::InvalidArgument, id));
    }

    return OpenEntry(id, warnings, failure);
}

Result<ObjectHandle> EditorPackageSet::Open(const LogicalPath& path, Array<EditorWarning>* warnings,
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
        return EditorComplete(diagnostic, prepared.Error(), id);
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

    pending.store = store.Value().Get();
    UniquePtr<PackageBulkStore> attachedStore(Move(store.Value()));
    if (auto set = loaded.SetBulkStore(attachedStore); !set) {
        return fail(AssetDiagnosticCode::None, set.Error());
    }

    return Result<void>::Success();
}

Result<ObjectHandle> EditorPackageSet::OpenEntry(AssetId id, Array<EditorWarning>* warnings,
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
    Array<EditorWarning> excluded(allocator);
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
                    EditorWarning{EditorWarningKind::ExcludedDependency, diagnostic});
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
        const PackageContents& contents = source.state->Contents();
        info.fingerprint = contents.fingerprint;
        info.readOnly = source.readOnly;
        info.fileName = Move(source.fileName);
        info.store = source.store;
        if (auto copied = info.importRecord.TryAppend(contents.importRecord); !copied) {
            return oom(copied.Error());
        }

        for (const PackageDependencyEntry& dependency : contents.dependencies) {
            if (dependency.kind == PackageDependencyKind::ImportOwner) {
                if (auto pushed = info.importOwner.TryPushBack(dependency); !pushed) {
                    return oom(pushed.Error());
                }
            }
        }

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
        for (const EditorWarning& warning : excluded) {
            (void)warnings->TryPushBack(warning);
        }

        for (const PackageUnresolvedImport& one : unresolved) {
            EditorWarning warning;
            warning.kind = EditorWarningKind::UnresolvedImport;
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

Result<ObjectHandle> EditorPackageSet::Create(const LogicalPath& path, TypeId type, AssetId id,
                                              AssetDiagnostic* failure,
                                              EditorAssetInitializer* initializer) noexcept {
    using Out = Result<ObjectHandle>;
    auto fail = [&](AssetDiagnosticCode code, ErrorCode error) noexcept {
        return EditorFail<ObjectHandle>(failure, EditorDiagnostic(code, error, id));
    };
    if (registry_.Get() == nullptr || type == TypeId{}) {
        return fail(AssetDiagnosticCode::None, ErrorCode::InvalidArgument);
    }

    // 경로: 색인에 같은 경로 키가 있거나(둘 이상이면 AlreadyExists) 저장하지 않은 새 패키지와
    // 겹친다.
    auto indexed = config_.index->FindByPath(path);
    if (indexed || indexed.Error() == ErrorCode::AlreadyExists) {
        return fail(AssetDiagnosticCode::DuplicatePath, ErrorCode::AlreadyExists);
    }

    for (const EditorPackageInfo& info : open_) {
        if (info.unsaved) {
            auto other = ParseLogicalPath(*allocator_, info.logicalPath.View());
            if (!other) {
                return fail(AssetDiagnosticCode::None, other.Error());
            }

            if (other.Value() == path) {
                return fail(AssetDiagnosticCode::DuplicatePath, ErrorCode::AlreadyExists);
            }
        }
    }

    // 파일 절대 경로(임시 접미사 포함)가 OS 상한 안이어야 한다(배치는 바뀌지 않으므로 여기서 판정).
    {
        String file(*allocator_);
        if (auto made = config_.layout->TryMakeFilePath(path, file); !made) {
            return fail(made.Error() == ErrorCode::CapacityExceeded
                            ? AssetDiagnosticCode::InvalidPath
                            : AssetDiagnosticCode::None,
                        made.Error());
        }
    }

    // id: 새로 발급하거나 준 id가 열려 있지도 색인에 있지도 않아야 한다.
    if (id.IsNull()) {
        auto made = AssetId::TryNew();
        if (!made) {
            return fail(AssetDiagnosticCode::None, made.Error());
        }

        id = made.Value();
    } else {
        auto existing = config_.index->FindById(id);
        if (Find(id) != nullptr || existing || existing.Error() == ErrorCode::AlreadyExists) {
            return fail(AssetDiagnosticCode::DuplicateAssetId, ErrorCode::AlreadyExists);
        }
    }

    // 공개 뒤에는 실패할 수 없게 정보·표·저장소를 먼저 마련한다.
    Allocator& allocator = *allocator_;
    EditorPackageInfo info(allocator);
    info.id = id;
    info.unsaved = true;
    info.dirty = true;
    info.generation = 1;
    if (auto assigned = info.logicalPath.TryAssign(path.Text()); !assigned) {
        return fail(AssetDiagnosticCode::None, assigned.Error());
    }

    if (auto reserved = open_.TryReserve(open_.Size() + 1); !reserved) {
        return fail(AssetDiagnosticCode::None, reserved.Error());
    }

    if (auto reserved = roots_.TryReserve(roots_.Size() + 1); !reserved) {
        return fail(AssetDiagnosticCode::None, reserved.Error());
    }

    auto store = UniquePtr<FilePackageBulkStore>::TryMake(allocator, allocator, *cache_);
    if (!store) {
        return fail(AssetDiagnosticCode::None, store.Error());
    }

    // 패키지·대표 에셋을 한 트랜잭션으로 만들고 초기화한 뒤 함께 공개한다(실패하면 소멸자가 되돌려
    // 아무것도 남지 않는다).
    const ObjectAccessContext access = Access();
    ConstructionContext transaction(*registry_, access);
    auto package = NewPackage(transaction, id, PackageKind::Editor);
    if (!package) {
        return fail(AssetDiagnosticCode::None, package.Error());
    }

    const ObjectHandle handle = package.Value();
    auto representative = NewRepresentativeAsset(transaction, handle, type);
    if (!representative) {
        return fail(AssetDiagnosticCode::None, representative.Error());
    }

    if (initializer != nullptr) {
        if (auto initialized = initializer->Initialize(transaction, representative.Value());
            !initialized) {
            return fail(AssetDiagnosticCode::None, initialized.Error());
        }
    }

    auto root = transaction.Commit();
    if (!root) {
        return fail(AssetDiagnosticCode::None, root.Error());
    }

    // 방금 공개한 Package다(Commit의 root가 쥔다): 접근은 실패하지 않고(소유 스레드 — GC·종료 중이
    // 아니다), 새 Package에는 저장소가 없다.
    auto borrow = registry_->Resolve<Package>(access, handle);
    ROCKS_ASSERT(borrow.IsOk());
    auto pointer = borrow.Value().Get();
    ROCKS_ASSERT(pointer.IsOk());
    info.package = handle;
    info.store = store.Value().Get();
    UniquePtr<PackageBulkStore> attached(Move(store.Value()));
    const bool joined = static_cast<bool>(AttachBulkStore(*pointer.Value(), attached));
    ROCKS_ASSERT(joined);
    (void)joined;
    const usize at = LowerBound(id);
    (void)open_.TryInsert(Move(info), at);
    (void)roots_.TryInsert(Move(root.Value()), at);
    return Out::Success(handle);
}

Result<void> EditorPackageSet::MarkDirty(AssetId id) noexcept {
    const usize at = LowerBound(id);
    if (at == open_.Size() || !(open_[at].id == id)) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    ++open_[at].generation;
    open_[at].dirty = true;
    return Result<void>::Success();
}

Result<void> EditorPackageSet::Save(AssetId id, const AssetDatabaseLock& lock,
                                    const EditorSaveOptions& options,
                                    Array<EditorWarning>* warnings,
                                    AssetDiagnostic* failure) noexcept {
    auto fail = [&](AssetDiagnosticCode code, ErrorCode error) noexcept {
        return EditorFail<void>(failure, EditorDiagnostic(code, error, id));
    };
    // hook 표는 필수다(Tags를 저장마다 다시 만든다 — 모르는 host가 태그를 지우지 않게). 잠금은 이
    // 프로젝트의 것이어야 한다.
    if (registry_.Get() == nullptr || config_.hooks == nullptr ||
        !lock.IsHeldFor(*config_.layout)) {
        return fail(AssetDiagnosticCode::None, ErrorCode::InvalidArgument);
    }

    const usize at = LowerBound(id);
    if (at == open_.Size() || !(open_[at].id == id)) {
        return fail(AssetDiagnosticCode::None, ErrorCode::NotFound);
    }

    EditorPackageInfo& info = open_[at];
    SavePlan plan(*allocator_);
    AssetDiagnostic diagnostic;
    if (auto planned = PlanSave(info, options, plan, diagnostic); !planned) {
        return EditorFail<void>(failure, diagnostic);
    }

    if (auto replaced = WriteAndReplace(info, plan, warnings, diagnostic); !replaced) {
        return EditorFail<void>(failure, diagnostic);
    }

    return Result<void>::Success();
}

Result<void> EditorPackageSet::PlanSave(EditorPackageInfo& info, const EditorSaveOptions& options,
                                        SavePlan& plan, AssetDiagnostic& diagnostic) noexcept {
    Allocator& allocator = *allocator_;
    const AssetId id = info.id;
    auto fail = [&](AssetDiagnosticCode code, ErrorCode error, uint32 section = 0) noexcept {
        diagnostic = EditorDiagnostic(code, error, id, section);
        return Result<void>::Failure(error);
    };
    if (info.readOnly) {
        return fail(AssetDiagnosticCode::NewerVersion, ErrorCode::Unsupported);
    }

    if (!info.unresolved.IsEmpty() && !options.dropUnresolved) {
        diagnostic = EditorDiagnostic(AssetDiagnosticCode::MissingDependency,
                                      ErrorCode::InvalidArgument, id);
        diagnostic.related = info.unresolved[0].target;
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    plan.startGeneration = info.generation;
    platform::FileSystem& files = *config_.files;
    const StringView content = config_.layout->ContentDirectory();
    if (info.unsaved) {
        // 새 패키지: 논리 경로의 NFC 이름. 대상이 이미 있거나 색인에 같은 경로 키가 있으면 거부한다
        // (Linux에서 대소문자만 다른 두 파일이 생기지 않게).
        auto path = ParseLogicalPath(allocator, info.logicalPath.View());
        if (!path) {
            return fail(AssetDiagnosticCode::None, path.Error());
        }

        // 경로 상한은 Create가 판정했다(배치는 바뀌지 않는다) — 여기 실패는 할당뿐이다.
        if (auto made = config_.layout->TryMakeFilePath(path.Value(), plan.target); !made) {
            return fail(AssetDiagnosticCode::None, made.Error());
        }

        platform::FileInfo existing;
        const platform::FileStatus stat =
            files.Stat(plan.target.View().Data(), plan.target.View().ByteCount(), existing);
        if (stat) {
            return fail(AssetDiagnosticCode::DuplicatePath, ErrorCode::AlreadyExists);
        }

        if (stat.code != platform::FileCode::NotFound) {
            return fail(AssetDiagnosticCode::None, FileErrorCode(stat));
        }

        auto indexed = config_.index->FindByPath(path.Value());
        if (indexed || indexed.Error() == ErrorCode::AlreadyExists) {
            return fail(AssetDiagnosticCode::DuplicatePath, ErrorCode::AlreadyExists);
        }

        const StringView target = plan.target.View();
        if (auto assigned =
                plan.fileName.TryAssign(StringView(target.Data() + content.ByteCount() + 1,
                                                   target.ByteCount() - content.ByteCount() - 1));
            !assigned) {
            return fail(AssetDiagnosticCode::None, assigned.Error());
        }
    } else {
        // 기존 패키지: 대상이 열 때의 파일인지 지문으로 본다(5.5절 1).
        Result<void> step = plan.fileName.TryAssign(info.fileName.View());
        if (step) {
            step = plan.target.TryAssign(content);
        }

        if (step) {
            step = plan.target.TryAppend("/");
        }

        if (step) {
            step = plan.target.TryAppend(info.fileName.View());
        }

        if (!step) {
            return fail(AssetDiagnosticCode::None, step.Error());
        }

        FilePackageRangeReader reader;
        if (auto opened = reader.Open(files, plan.target.View()); !opened) {
            return opened.Error() == ErrorCode::NotFound
                       ? fail(AssetDiagnosticCode::ExternalModification, ErrorCode::StaleHandle)
                       : fail(AssetDiagnosticCode::None, opened.Error());
        }

        AssetDiagnostic read;
        auto fingerprint = ReadPackageFingerprint(allocator, reader, &read);
        if (!fingerprint && (fingerprint.Error() == ErrorCode::OutOfMemory ||
                             read.code == AssetDiagnosticCode::None)) {
            return fail(AssetDiagnosticCode::None, fingerprint.Error());
        }

        if (!fingerprint || !(fingerprint.Value() == info.fingerprint)) {
            return fail(AssetDiagnosticCode::ExternalModification, ErrorCode::StaleHandle);
        }
    }

    // Tags·Build 의존은 대표 에셋 타입의 hook으로 만든다(Tags는 저장마다 다시 — 7.1절 15). 대표
    // 에셋이 없으면(편집이 Discard했다) 그 오류다(hook 없이 쓰면 writer가 같은 패키지를 거부한다).
    AssetDependencySink dependencies(allocator);
    {
        const ObjectAccessContext access = Access();
        auto representative = registry_->FindChild(access, info.package, StringView());
        if (!representative) {
            return fail(AssetDiagnosticCode::None, representative.Error());
        }

        // 타입 조회는 TypeRegistry가 닫혔으면(ShuttingDown) 실패할 수 있다 — 진단 None과 그
        // 오류다(로드의 같은 조회와 같다, 5.7절 19).
        auto type = registry_->AcquireType(access, representative.Value());
        if (!type) {
            return fail(AssetDiagnosticCode::None, type.Error());
        }

        const TypeId typeId = type.Value().Id();
        AssetTagSink tags(allocator);
        Result<void> step =
            config_.hooks->CollectTags(*registry_, access, representative.Value(), typeId, tags);
        if (step) {
            step = config_.hooks->CollectBuildDependencies(
                *registry_, access, representative.Value(), typeId, dependencies);
        }

        if (!step) {
            return fail(AssetDiagnosticCode::None, step.Error());
        }

        auto written = tags.Write(allocator);
        if (!written) {
            return fail(AssetDiagnosticCode::None, written.Error());
        }

        plan.tags = Move(written.Value());
    }

    // extra 의존 = hook의 Build 의존 + 열 때 읽은 ImportOwner(자리를 한 번에 잡는다).
    if (auto reserved =
            plan.extra.TryReserve(dependencies.Dependencies().Size() + info.importOwner.Size());
        !reserved) {
        return fail(AssetDiagnosticCode::None, reserved.Error());
    }

    (void)plan.extra.TryAppend(dependencies.Dependencies());
    (void)plan.extra.TryAppend(info.importOwner.View());

    return Result<void>::Success();
}

Result<void> EditorPackageSet::WriteAndReplace(EditorPackageInfo& info, SavePlan& plan,
                                               Array<EditorWarning>* warnings,
                                               AssetDiagnostic& diagnostic) noexcept {
    Allocator& allocator = *allocator_;
    platform::FileSystem& files = *config_.files;
    const AssetId id = info.id;
    auto fail = [&](AssetDiagnosticCode code, ErrorCode error) noexcept {
        diagnostic = EditorDiagnostic(code, error, id);
        return Result<void>::Failure(error);
    };

    // 경고 자리(미해결 null 저장 + NotDurable + 고아 정리 하나)를 먼저 잡는다(교체 뒤에 실패하지
    // 않게 — 고아 정리는 실패를 경고 하나로 모은다).
    if (warnings != nullptr) {
        if (auto reserved = warnings->TryReserve(warnings->Size() + info.unresolved.Size() + 2);
            !reserved) {
            return fail(AssetDiagnosticCode::None, reserved.Error());
        }
    }

    // 임시 파일 "<대상>.<guid>.tmp"(5.5절 2).
    String temp(allocator);
    auto guid = TryNewGuid();
    if (!guid) {
        return fail(AssetDiagnosticCode::None, guid.Error());
    }

    Result<void> step = temp.TryAssign(plan.target.View());
    if (step) {
        step = temp.TryAppend(".");
    }

    if (step) {
        step = EditorAppendGuidText(temp, guid.Value());
    }

    if (step) {
        step = temp.TryAppend(EditorTempSuffix);
    }

    if (!step) {
        return fail(AssetDiagnosticCode::None, step.Error());
    }

    // 새 패키지는 부모 디렉터리를 만든다.
    const StringView target = plan.target.View();
    if (info.unsaved) {
        if (auto made =
                platform::MakeDirectoryTree(files, target.Data(), EditorParentBytes(target));
            !made) {
            return fail(AssetDiagnosticCode::None, FileErrorCode(made));
        }
    }

    // 실패하면 임시 파일을 지운다(지우기 실패는 무시 — 다음 저장의 고아 정리가 지운다).
    auto discard = [&]() noexcept {
        (void)files.Delete(temp.View().Data(), temp.View().ByteCount());
    };
    PackageWriteOptions write;
    write.kind = PackageKind::Editor;
    write.engineVersion = config_.writeEngineVersion.IsEmpty() ? build::CurrentEngineVersion()
                                                               : config_.writeEngineVersion;
    write.bulkSource = info.store;
    write.tags = plan.tags.View();
    write.extraDependencies = plan.extra.View();
    write.importRecord = info.importRecord.View();
    PackageWriteResult result(allocator);
    {
        platform::File file;
        if (auto opened = file.Open(files, temp.View().Data(), temp.View().ByteCount(),
                                    platform::FileOpenMode::CreateNew);
            !opened) {
            return fail(AssetDiagnosticCode::None, FileErrorCode(opened));
        }

        FilePackageSink sink(file);
        auto written =
            WritePackage(allocator, *registry_, Access(), info.package, write, sink, &diagnostic);
        if (!written) {
            (void)file.Close();
            discard();
            return EditorComplete(diagnostic, written.Error(), id);
        }

        result = Move(written.Value());
        if (auto flushed = file.Flush(); !flushed) {
            (void)file.Close();
            discard();
            return fail(AssetDiagnosticCode::None, FileErrorCode(flushed));
        }

        if (auto closed = file.Close(); !closed) {
            discard();
            return fail(AssetDiagnosticCode::None, FileErrorCode(closed));
        }
    }

    // 다시 읽어 검증하고(5.5절 5) 새 파일의 bulk entry·섹션 offset을 얻는다(Rebind).
    PackageReadOptions edit;
    edit.expectedKind = PackageKind::Editor;
    PackageIndexView reread(allocator);
    {
        FilePackageRangeReader reader;
        if (auto opened = reader.Open(files, temp.View()); !opened) {
            discard();
            return fail(AssetDiagnosticCode::None, opened.Error());
        }

        Array<uint8> scratch(allocator);
        if (auto sized = scratch.TryResize(EditorSaveVerifyScratchBytes); !sized) {
            discard();
            return fail(AssetDiagnosticCode::None, sized.Error());
        }

        if (auto verified = VerifyPackageFile(allocator, reader, scratch.View(), &diagnostic);
            !verified) {
            discard();
            return EditorComplete(diagnostic, verified.Error(), id);
        }

        auto read = ReadPackageForEdit(allocator, reader, edit, &diagnostic);
        if (!read) {
            discard();
            return EditorComplete(diagnostic, read.Error(), id);
        }

        reread = Move(read.Value());
    }

    // 교체 뒤의 일을 모두 준비한다(실패하면 교체 전이라 아무것도 바뀌지 않는다): 저장소를 새 파일에
    // 다시 잇는 표, 임시 파일을 대상 이름으로 읽은 새 색인.
    const PackageContents& contents = reread.contents;
    uint64 bulkOffset = 0;
    for (const PackageSectionEntry& section : contents.sections) {
        if (section.kind == PackageSectionKind::Bulk) {
            bulkOffset = section.offset;
        }
    }

    BulkRebindPlan rebind(allocator);
    if (auto prepared =
            info.store->PrepareRebind(rebind, target, result.fingerprint, bulkOffset,
                                      contents.bulkEntries.View(), result.bulkRemap.View());
        !prepared) {
        discard();
        return fail(AssetDiagnosticCode::None, prepared.Error());
    }

    AssetIndexRefresh refresh(allocator);
    if (auto prepared = config_.index->PrepareRefresh(files, plan.fileName.View(), temp.View(),
                                                      config_.read, refresh);
        !prepared) {
        discard();
        return fail(AssetDiagnosticCode::None, prepared.Error());
    }

    // 교체(5.5절 6): 캐시가 쥔 대상 핸들을 먼저 닫는다.
    cache_->Close(target);
    const platform::FileStatus replaced = files.Replace(temp.View().Data(), temp.View().ByteCount(),
                                                        target.Data(), target.ByteCount());
    const bool notDurable = replaced.code == platform::FileCode::NotDurable;
    if (!replaced && !notDurable) {
        discard();
        return fail(AssetDiagnosticCode::None, FileErrorCode(replaced));
    }

    // 교체 뒤(5.5절 7 — 실패하지 않는다): 정보를 새 파일로 바꾸고 준비한 것을 확정한다.
    info.fingerprint = result.fingerprint;
    info.fileName = Move(plan.fileName);
    info.unsaved = false;
    info.logicalPath.Clear();
    if (warnings != nullptr) {
        if (notDurable) {
            EditorWarning warning;
            warning.kind = EditorWarningKind::NotDurable;
            warning.diagnostic =
                EditorDiagnostic(AssetDiagnosticCode::NotDurable, FileErrorCode(replaced), id);
            (void)warnings->TryPushBack(warning);
        }

        for (const PackageUnresolvedImport& one : info.unresolved) {
            EditorWarning warning;
            warning.kind = EditorWarningKind::DroppedUnresolvedImport;
            warning.diagnostic = EditorDiagnostic(
                one.code, one.error, id, static_cast<uint32>(PackageSectionKind::ObjectImports));
            warning.diagnostic.related = one.target;
            warning.diagnostic.detail = one.import;
            (void)warnings->TryPushBack(warning);
        }
    }

    info.unresolved.Clear();
    // 파일이 메모리와 같아졌으므로 dirty를 지운다(시작한 세대 그대로일 때 — 저장 중 편집은 남긴다).
    if (info.generation == plan.startGeneration) {
        info.dirty = false;
    }

    info.store->CommitRebind(rebind);
    config_.index->CommitRefresh(refresh);
    DeleteOrphans(info, target, warnings);
    return Result<void>::Success();
}

void EditorPackageSet::DeleteOrphans(const EditorPackageInfo& info, StringView target,
                                     Array<EditorWarning>* warnings) noexcept {
    platform::FileSystem& files = *config_.files;
    auto warn = [&](ErrorCode error) noexcept {
        if (warnings != nullptr) {
            EditorWarning warning;
            warning.kind = EditorWarningKind::OrphanNotDeleted;
            warning.diagnostic = EditorDiagnostic(AssetDiagnosticCode::None, error, info.id);
            (void)warnings->TryPushBack(warning);
        }
    };

    // 같은 디렉터리의 고아 임시 파일(정확한 모양만 — 5.5절 8)을 지운다. 정리는 저장의 일부가 아니라
    // 실패는 경고다(첫 실패 하나 — 자리는 WriteAndReplace가 잡았다). 지우기 실패 뒤에도 나머지를
    // 지운다.
    const usize parentBytes = EditorParentBytes(target);
    Array<String> names(*allocator_);
    EditorOrphanContext context{&names};
    if (auto listed =
            files.EnumerateDirectory(target.Data(), parentBytes, &EditorCollectOrphan, &context);
        !listed) {
        warn(FileErrorCode(listed));
        return;
    }

    if (context.error != ErrorCode::None) {
        warn(context.error);
        return;
    }

    ErrorCode first = ErrorCode::None;
    for (const String& name : names) {
        String path(*allocator_);
        Result<void> step = path.TryAssign(StringView(target.Data(), parentBytes));
        if (step) {
            step = path.TryAppend("/");
        }

        if (step) {
            step = path.TryAppend(name.View());
        }

        if (!step) {
            warn(step.Error());
            return;
        }

        if (auto removed = files.Delete(path.View().Data(), path.View().ByteCount());
            !removed && first == ErrorCode::None) {
            first = FileErrorCode(removed);
        }
    }

    if (first != ErrorCode::None) {
        warn(first);
    }
}

} // namespace rocks
