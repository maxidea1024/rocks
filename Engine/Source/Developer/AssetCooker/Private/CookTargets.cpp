#include <AssetCooker/CookTargets.h>
#include <AssetDatabase/LogicalPath.h>
#include <Core/Algorithms.h>
#include <Core/Containers/HashSet.h>
#include <Object/TypeRegistry.h>

namespace rocks {

namespace {

bool AssetIdLess(AssetId left, AssetId right) noexcept {
    return GuidLess(left.value, right.value);
}

// 경로 항목 하나의 판정이다(비교 키 — 에셋은 일치, 디렉터리는 접두, 마운트는 전체).
struct PathMatcher {
    explicit PathMatcher(Allocator& allocator) noexcept : key(allocator) {
    }

    String key;
    CookPathKind kind = CookPathKind::Invalid;
    bool everything = false;

    bool Matches(StringView entryKey) const noexcept {
        if (everything) {
            return true;
        }

        if (kind == CookPathKind::Asset) {
            return entryKey == key.View();
        }

        return entryKey.ByteCount() > key.View().ByteCount() &&
               entryKey.StartsWith(key.View(), StringComparison::CaseSensitive) &&
               entryKey.Data()[key.View().ByteCount()] == '/';
    }
};

// text의 판정을 만든다. 형식이 틀리면 InvalidArgument다.
Result<void> MakeMatcher(Allocator& allocator, StringView text, PathMatcher& out) noexcept {
    auto kind = ClassifyCookPath(allocator, text);
    if (!kind) {
        return Result<void>::Failure(kind.Error());
    }

    if (kind.Value() == CookPathKind::Invalid) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    out.kind = kind.Value();
    const bool directory = kind.Value() == CookPathKind::Directory;
    const StringView path = directory ? StringView(text.Data(), text.ByteCount() - 1) : text;
    if (directory && EqualsIgnoringAsciiCase(path, GameMount)) {
        out.everything = true;
        return Result<void>::Success();
    }

    auto parsed = ParseLogicalPath(allocator, path, nullptr);
    if (!parsed) {
        return Result<void>::Failure(parsed.Error());
    }

    return out.key.TryAssign(parsed.Value().Key());
}

Result<void> MakeMatchers(Allocator& allocator, Span<const String> texts,
                          Array<PathMatcher>& out) noexcept {
    if (auto reserved = out.TryReserve(texts.Size()); !reserved) {
        return reserved;
    }

    for (const String& text : texts) {
        PathMatcher matcher(allocator);
        if (auto made = MakeMatcher(allocator, text.View(), matcher); !made) {
            return made;
        }

        (void)out.TryPushBack(Move(matcher));
    }

    return Result<void>::Success();
}

bool AnyMatches(Span<const PathMatcher> matchers, StringView key) noexcept {
    for (const PathMatcher& matcher : matchers) {
        if (matcher.Matches(key)) {
            return true;
        }
    }

    return false;
}

using AssetIdSet = HashSet<AssetId, SeededHash<AssetId>>;

// 계산 상태다. 진단·대상·제외 목록에 넣는 할당 실패는 error_에 두고 계속한다(호출마다
// 분기하지 않는다) — Run이 끝에서 확인한다.
class TargetWalk {
public:
    TargetWalk(Allocator& allocator, const CookTargetRequest& request, CookTargetSet& out) noexcept
        : allocator_(allocator), request_(request), out_(out), never_(allocator),
          primary_(allocator), primaryList_(allocator), visited_(allocator), excluded_(allocator),
          queue_(allocator), reversed_(allocator), reverseQueue_(allocator) {
    }

    Result<void> Run() noexcept {
        const CookSettings& settings = *request_.settings;
        if (auto made = MakeMatchers(allocator_, settings.neverCook.View(), never_); !made) {
            return made;
        }

        if (auto marked = MarkPrimaryAssets(); !marked) {
            return marked;
        }

        const bool partial = !request_.paths.IsEmpty();
        if (partial) {
            for (usize index = 0; index < request_.paths.Size(); ++index) {
                if (auto added = AddRoots(request_.paths[index], static_cast<uint32>(index), false);
                    !added) {
                    return added;
                }
            }
        } else {
            for (usize index = 0; index < settings.alwaysCook.Size(); ++index) {
                if (auto added = AddRoots(settings.alwaysCook[index].View(),
                                          static_cast<uint32>(index), true);
                    !added) {
                    return added;
                }
            }

            for (const AssetIndexEntry* primary : primaryList_) {
                AddRoot(*primary);
            }
        }

        // 폐포를 먼저 비우고 역방향 Build를 하나씩 넓힌다(넓힌 에셋도 폐포를 따른다 — 고정점).
        usize processed = 0;
        usize expanded = 0;
        while (processed < queue_.Size() || expanded < reverseQueue_.Size()) {
            if (processed < queue_.Size()) {
                Process(*queue_[processed++]);
            } else if (auto widened = Expand(*reverseQueue_[expanded++]); !widened) {
                return widened;
            }
        }

        if (partial && request_.existing != nullptr) {
            CheckExisting();
        }

        if (error_ != ErrorCode::None) {
            return Result<void>::Failure(error_);
        }

        return Finish();
    }

private:
    // 여기 오는 실패는 모두 할당 실패다(어느 것을 두어도 같다).
    void Keep(const Result<void>& step) noexcept {
        if (!step) {
            error_ = step.Error();
        }
    }

    void Diagnose(AssetDiagnosticCode code, bool warning, AssetId asset, AssetId related = {},
                  uint32 setting = 0) noexcept {
        Keep(out_.diagnostics.TryPushBack(CookDiagnostic{code, warning, asset, related, setting}));
    }

    bool IsNever(const AssetIndexEntry& entry) const noexcept {
        return AnyMatches(never_.View(), entry.key.View());
    }

    // PrimaryAsset 규칙에 맞는 항목을 모은다(전체·부분 모두 — PrimaryAsset 표시에 쓴다).
    Result<void> MarkPrimaryAssets() noexcept {
        for (const PrimaryAssetRule& rule : request_.settings->primaryAssets) {
            const StringView name = rule.type.View();
            auto type =
                request_.types->AcquireByToolName(Span<const char>(name.Data(), name.ByteCount()));
            if (!type) {
                // 없는 타입은 ReadCookSettings가 거부하는 설정 위반이다. 그 밖(종료 뒤
                // ShuttingDown· lease 포화 Overflow)은 TypeRegistry 오류 그대로다.
                return Result<void>::Failure(type.Error() == ErrorCode::NotFound
                                                 ? ErrorCode::InvalidArgument
                                                 : type.Error());
            }

            Array<PathMatcher> directories(allocator_);
            if (auto made = MakeMatchers(allocator_, rule.directories.View(), directories); !made) {
                return made;
            }

            Array<const AssetIndexEntry*> entries(allocator_);
            if (auto listed =
                    request_.index->ListByType(type.Value().Id(), request_.types, entries);
                !listed) {
                return listed;
            }

            for (const AssetIndexEntry* entry : entries) {
                if (!AnyMatches(directories.View(), entry->key.View())) {
                    continue;
                }

                auto added = primary_.Add(entry->id);
                if (!added) {
                    return Result<void>::Failure(added.Error());
                }

                if (added.Value() == InsertOutcome::Inserted) {
                    if (auto pushed = primaryList_.TryPushBack(entry); !pushed) {
                        return pushed;
                    }
                }
            }
        }

        algo::Sort(primaryList_.View(),
                   [](const AssetIndexEntry* left, const AssetIndexEntry* right) noexcept {
                       return AssetIdLess(left->id, right->id);
                   });
        return Result<void>::Success();
    }

    // text가 가리키는 항목을 뿌리로 더한다. 아무것도 가리키지 않으면 alwaysCook은 경고·인자는
    // 오류 InvalidPath, 에셋 경로가 같은 경로 키 파일 둘 이상이면 오류 DuplicatePath다.
    Result<void> AddRoots(StringView text, uint32 setting, bool fromSettings) noexcept {
        PathMatcher matcher(allocator_);
        if (auto made = MakeMatcher(allocator_, text, matcher); !made) {
            return made;
        }

        Array<const AssetIndexEntry*> entries(allocator_);
        if (matcher.kind == CookPathKind::Asset) {
            auto path = ParseLogicalPath(allocator_, text, nullptr);
            if (!path) {
                return Result<void>::Failure(path.Error());
            }

            // FindByPath의 오류는 NotFound·AlreadyExists뿐이다.
            auto found = request_.index->FindByPath(path.Value());
            if (found) {
                if (auto pushed = entries.TryPushBack(found.Value()); !pushed) {
                    return pushed;
                }
            } else if (found.Error() == ErrorCode::AlreadyExists) {
                // 같은 경로 파일이 둘 이상이다 — 어느 것도 뿌리로 고를 수 없다.
                Diagnose(AssetDiagnosticCode::DuplicatePath, false, {}, {}, setting);
                return Result<void>::Success();
            }
        } else {
            const StringView directory(text.Data(), text.ByteCount() - 1);
            if (auto listed = request_.index->ListUnder(directory, true, entries); !listed) {
                return listed;
            }
        }

        if (entries.IsEmpty()) {
            Diagnose(AssetDiagnosticCode::InvalidPath, fromSettings, {}, {}, setting);
            return Result<void>::Success();
        }

        for (const AssetIndexEntry* entry : entries) {
            if (entry->duplicateId) {
                Diagnose(AssetDiagnosticCode::DuplicateAssetId, false, entry->id, {}, setting);
            } else if (IsNever(*entry)) {
                // 명시한 에셋이 NeverCook이다 — NeverCook이 이긴다(UE와 같다).
                Diagnose(AssetDiagnosticCode::NeverCookReferenced, true, entry->id, {}, setting);
            } else {
                // 부분 쿠킹의 인자 뿌리는 역방향 Build를 넓힌다(alwaysCook은 아니다).
                if (fromSettings) {
                    Enqueue(*entry);
                } else {
                    MarkReverse(*entry);
                }
            }
        }

        return Result<void>::Success();
    }

    // PrimaryAsset 규칙으로만 닿은 뿌리다(NeverCook이면 진단 없이 뺀다).
    void AddRoot(const AssetIndexEntry& entry) noexcept {
        if (entry.duplicateId) {
            Diagnose(AssetDiagnosticCode::DuplicateAssetId, false, entry.id);
        } else if (!IsNever(entry)) {
            Enqueue(entry);
        }
    }

    // 고유 항목(같은 id 없음)만 들어온다 — 대상 처리가 항목을 다시 찾지 않는다.
    void Enqueue(const AssetIndexEntry& entry) noexcept {
        auto added = visited_.Add(entry.id);
        if (!added) {
            Keep(Result<void>::Failure(added.Error()));
        } else if (added.Value() == InsertOutcome::Inserted) {
            Keep(queue_.TryPushBack(&entry));
        }
    }

    // 의존 하나다(hardLike = Hard·bundle — 없거나 NeverCook이면 오류). FindById의 오류는
    // NotFound·AlreadyExists뿐이다.
    void Follow(AssetId from, AssetId to, bool hardLike) noexcept {
        auto found = request_.index->FindById(to);
        if (!found) {
            if (found.Error() == ErrorCode::NotFound) {
                Diagnose(AssetDiagnosticCode::MissingDependency, !hardLike, from, to);
            } else {
                Diagnose(AssetDiagnosticCode::DuplicateAssetId, false, from, to);
            }
        } else if (IsNever(*found.Value())) {
            if (hardLike) {
                Diagnose(AssetDiagnosticCode::NeverCookReferenced, false, from, to);
            } else {
                auto added = excluded_.Add(to);
                Keep(added ? Result<void>::Success() : Result<void>::Failure(added.Error()));
            }
        } else {
            Enqueue(*found.Value());
        }
    }

    // entry를 대상으로 하고 그 역방향 Build도 넓힌다(인자 뿌리와 역방향으로 더한 에셋 — 폐포로 먼저
    // 들어온 에셋이 뒤에 역방향으로 닿아도 넓힌다).
    void MarkReverse(const AssetIndexEntry& entry) noexcept {
        Enqueue(entry);
        auto added = reversed_.Add(entry.id);
        if (!added) {
            Keep(Result<void>::Failure(added.Error()));
        } else if (added.Value() == InsertOutcome::Inserted) {
            Keep(reverseQueue_.TryPushBack(&entry));
        }
    }

    void Process(const AssetIndexEntry& entry) noexcept {
        const AssetId id = entry.id;
        const bool soft = request_.settings->cookSoftReferences;
        for (const PackageDependencyEntry& dependency : entry.dependencies) {
            const bool hard = dependency.kind == PackageDependencyKind::Hard;
            if (hard || (soft && dependency.kind == PackageDependencyKind::Soft)) {
                Follow(id, dependency.asset, hard);
            }
        }

        if (primary_.Contains(id)) {
            for (const PackageTag& tag : entry.tags) {
                if (!tag.key.View().StartsWith(CookBundleTagPrefix,
                                               StringComparison::CaseSensitive)) {
                    continue;
                }

                auto member = AssetId::TryParse(tag.value.View());
                if (!member || member.Value().IsNull() ||
                    tag.key.View().ByteCount() == CookBundleTagPrefix.ByteCount()) {
                    Diagnose(AssetDiagnosticCode::CorruptPackage, false, id);
                } else {
                    Follow(id, member.Value(), true);
                }
            }
        }
    }

    // entry를 Build로 의존하는 에셋도 다시 쿠킹한다(원본을 고치면 그것을 묶는 에셋도 — 10절).
    Result<void> Expand(const AssetIndexEntry& entry) noexcept {
        const AssetId id = entry.id;
        Array<const AssetIndexEntry*> referencers(allocator_);
        if (auto listed = request_.index->Referencers(
                id, DependencyKindBit(PackageDependencyKind::Build), referencers);
            !listed) {
            return listed;
        }

        for (const AssetIndexEntry* referencer : referencers) {
            if (referencer->duplicateId) {
                Diagnose(AssetDiagnosticCode::DuplicateAssetId, false, id, referencer->id);
            } else if (!IsNever(*referencer)) {
                MarkReverse(*referencer);
            }
        }

        return Result<void>::Success();
    }

    // 기존 매니페스트(AssetId 순): 색인에 없는 항목은 사라진 항목(같은 id가 둘 이상인
    // AlreadyExists는 색인에 있다), 이번에 다시 쿠킹하지 않는 남은 항목이 그것을 Hard로 의존하면
    // 오류. 대상의 의존은 옛 매니페스트가 아니라 색인으로 이미 보았다(Follow).
    void CheckExisting() noexcept {
        const CookedManifest& existing = *request_.existing;
        for (const CookedManifestEntry& entry : existing.entries) {
            auto found = request_.index->FindById(entry.asset);
            if (!found && found.Error() == ErrorCode::NotFound) {
                Keep(out_.removed.TryPushBack(entry.asset));
            }
        }

        for (const CookedManifestEntry& entry : existing.entries) {
            if (IsRemoved(entry.asset) || visited_.Contains(entry.asset)) {
                continue;
            }

            for (const CookedManifestDependency& dependency : entry.dependencies) {
                if (dependency.kind == PackageDependencyKind::Hard && IsRemoved(dependency.asset)) {
                    Diagnose(AssetDiagnosticCode::MissingDependency, false, entry.asset,
                             dependency.asset);
                }
            }
        }
    }

    bool IsRemoved(AssetId id) const noexcept {
        return algo::BinarySearch(out_.removed.View(), id,
                                  [](AssetId left, AssetId right) noexcept {
                                      return AssetIdLess(left, right);
                                  }) != algo::NotFound;
    }

    // PrimaryAsset의 bundle(이름 순, 구성원 AssetId 순)이다.
    Result<void> CollectBundles(const AssetIndexEntry& entry, CookTarget& target) noexcept {
        for (const PackageTag& tag : entry.tags) {
            const StringView key = tag.key.View();
            if (!key.StartsWith(CookBundleTagPrefix, StringComparison::CaseSensitive) ||
                key.ByteCount() == CookBundleTagPrefix.ByteCount()) {
                continue;
            }

            auto member = AssetId::TryParse(tag.value.View());
            if (!member || member.Value().IsNull()) {
                continue;
            }

            const StringView name(key.Data() + CookBundleTagPrefix.ByteCount(),
                                  key.ByteCount() - CookBundleTagPrefix.ByteCount());
            // 태그는 (키, 값) 순이고 같은 쌍이 없다(PackageTags 형식) — 같은 bundle은 늘 마지막
            // 항목이고 구성원은 겹치지 않는다.
            const usize count = target.bundles.Size();
            if (count == 0 || target.bundles[count - 1].name.View() != name) {
                CookTargetBundle created(allocator_);
                if (auto assigned = created.name.TryAssign(name); !assigned) {
                    return assigned;
                }

                if (auto pushed = target.bundles.TryPushBack(Move(created)); !pushed) {
                    return pushed;
                }
            }

            CookTargetBundle& bundle = target.bundles[target.bundles.Size() - 1];
            if (auto pushed = bundle.members.TryPushBack(member.Value()); !pushed) {
                return pushed;
            }
        }

        // 구성원은 표준 문자열 순으로 왔다 — AssetId 순으로 맞춘다.
        for (CookTargetBundle& bundle : target.bundles) {
            algo::Sort(bundle.members.View(), [](AssetId left, AssetId right) noexcept {
                return AssetIdLess(left, right);
            });
        }

        return Result<void>::Success();
    }

    Result<void> Finish() noexcept {
        algo::Sort(queue_.View(),
                   [](const AssetIndexEntry* left, const AssetIndexEntry* right) noexcept {
                       return AssetIdLess(left->id, right->id);
                   });
        if (auto reserved = out_.targets.TryReserve(queue_.Size()); !reserved) {
            return reserved;
        }

        for (const AssetIndexEntry* entry : queue_) {
            CookTarget target(allocator_);
            target.id = entry->id;
            target.primary = primary_.Contains(entry->id);
            if (target.primary) {
                if (auto collected = CollectBundles(*entry, target); !collected) {
                    return collected;
                }
            }

            (void)out_.targets.TryPushBack(Move(target));
        }

        // NeverCook 에셋은 대상이 되지 않으므로 제외 목록과 대상은 겹치지 않는다.
        if (auto reserved = out_.excluded.TryReserve(excluded_.Count()); !reserved) {
            return reserved;
        }

        excluded_.ForEach([&](const AssetId& id) noexcept { (void)out_.excluded.TryPushBack(id); });

        algo::Sort(out_.excluded.View(),
                   [](AssetId left, AssetId right) noexcept { return AssetIdLess(left, right); });
        algo::Sort(out_.removed.View(),
                   [](AssetId left, AssetId right) noexcept { return AssetIdLess(left, right); });
        algo::Sort(out_.diagnostics.View(), CookDiagnosticBefore);
        return Result<void>::Success();
    }

    Allocator& allocator_;
    const CookTargetRequest& request_;
    CookTargetSet& out_;
    Array<PathMatcher> never_;
    AssetIdSet primary_;
    Array<const AssetIndexEntry*> primaryList_;
    AssetIdSet visited_;
    AssetIdSet excluded_;
    Array<const AssetIndexEntry*> queue_;
    // 역방향 Build를 넓힐 에셋이다(인자 뿌리와 역방향으로 더한 에셋).
    AssetIdSet reversed_;
    Array<const AssetIndexEntry*> reverseQueue_;
    ErrorCode error_ = ErrorCode::None;
};

} // namespace

Result<void> ComputeCookTargets(Allocator& allocator, const CookTargetRequest& request,
                                CookTargetSet& out) noexcept {
    out.targets.Clear();
    out.excluded.Clear();
    out.removed.Clear();
    out.diagnostics.Clear();
    if (request.index == nullptr || request.types == nullptr || request.settings == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    TargetWalk walk(allocator, request, out);
    return walk.Run();
}

} // namespace rocks
