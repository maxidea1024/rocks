#include <AssetCooker/CookRun.h>
#include <AssetRuntime/PackageReader.h>
#include <Core/Algorithms.h>
#include <Core/Graph/StrongComponents.h>

namespace rocks {

namespace {

bool AssetIdLess(AssetId left, AssetId right) noexcept {
    return GuidLess(left.value, right.value);
}

// 대상(AssetId 순)과 AssetId의 양방향 순서다(BinarySearch).
struct TargetOrder {
    bool operator()(const CookTarget& target, AssetId key) const noexcept {
        return AssetIdLess(target.id, key);
    }

    bool operator()(AssetId key, const CookTarget& target) const noexcept {
        return AssetIdLess(key, target.id);
    }
};

// 실행 상태다. 진단 push의 할당 실패는 error_에 두고 끝에서 돌려준다(CookTargets와 같은 방식).
class CookWalk {
public:
    CookWalk(Allocator& allocator, const CookRunRequest& request, CookRunResult& result) noexcept
        : allocator_(allocator), request_(request), result_(result), before_(allocator),
          order_(allocator), check_(allocator), checkEntry_(allocator) {
    }

    Result<void> Run() noexcept {
        EditorPackageSet& packages = *request_.packages;
        // 쿠킹은 디스크 상태다 — 저장하지 않은 편집이 있으면 거부한다.
        for (usize index = 0; index < packages.Count(); ++index) {
            if (packages.At(index).dirty) {
                return Result<void>::Failure(ErrorCode::Busy);
            }
        }

        if (auto reserved = before_.TryReserve(packages.Count()); !reserved) {
            return reserved;
        }

        for (usize index = 0; index < packages.Count(); ++index) {
            (void)before_.TryPushBack(packages.At(index).id);
        }

        // 로더 대조용 매니페스트 머리다(프로파일 칸 — AS 11.6절 3).
        if (auto assigned = check_.profileId.TryAssign(request_.profile->id.View()); !assigned) {
            return assigned;
        }

        check_.profileSettings = request_.profileSettings;
        check_.cookerVersion = request_.cookerVersion;
        Result<void> run = Order();
        for (usize at = 0; run && at < order_.Size(); ++at) {
            run = CookOne(order_[at]);
        }

        // 멈춰도 이번 쿠킹이 연 패키지는 닫는다(닫기 오류는 원래 오류 뒤에 묻힌다).
        Result<void> closed = packages.CloseOpened(before_.View());
        if (!run) {
            return run;
        }

        if (!closed) {
            return closed;
        }

        if (error_ != ErrorCode::None) {
            return Result<void>::Failure(error_);
        }

        algo::Sort(result_.diagnostics.View(), CookDiagnosticBefore);
        return Result<void>::Success();
    }

private:
    // 여기 오는 실패는 모두 할당 실패다.
    void Keep(const Result<void>& step) noexcept {
        if (!step) {
            error_ = step.Error();
        }
    }

    void Diagnose(AssetDiagnosticCode code, AssetId asset, AssetId related,
                  ErrorCode error) noexcept {
        Keep(
            result_.diagnostics.TryPushBack(CookDiagnostic{code, false, asset, related, 0, error}));
    }

    // 대상 번호다(대상은 AssetId 순 — 없으면 algo::NotFound).
    usize TargetIndex(AssetId id) const noexcept {
        return algo::BinarySearch(request_.targets->targets.View(), id, TargetOrder{});
    }

    // 키가 있는 대상의 쿠킹 순서(Hard SCC 역위상 — 의존 먼저, SCC 안은 AssetId 순)다.
    Result<void> Order() noexcept {
        const Array<CookTarget>& targets = request_.targets->targets;
        const usize count = targets.Size();
        Array<uint32> first(allocator_);
        Array<uint32> edges(allocator_);
        if (auto sized = first.TryResize(count + 1); !sized) {
            return sized;
        }

        for (usize at = 0; at < count; ++at) {
            first[at] = static_cast<uint32>(edges.Size());
            auto entry = request_.index->FindById(targets[at].id);
            if (!entry) {
                // 대상은 이 색인의 고유 항목이다(ComputeCookTargets) — 다른 색인의 결과를 넘겼다.
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }

            for (const PackageDependencyEntry& dependency : entry.Value()->dependencies) {
                const usize target = dependency.kind == PackageDependencyKind::Hard
                                         ? TargetIndex(dependency.asset)
                                         : algo::NotFound;
                if (target == algo::NotFound) {
                    continue;
                }

                if (auto pushed = edges.TryPushBack(static_cast<uint32>(target)); !pushed) {
                    return pushed;
                }
            }
        }

        first[count] = static_cast<uint32>(edges.Size());
        if (auto reserved = order_.TryReserve(count); !reserved) {
            return reserved;
        }

        return algo::ForEachStrongComponent(
            allocator_, static_cast<uint32>(count), first.View(), edges.View(),
            [&](Span<const uint32> members, bool) noexcept -> Result<void> {
                const usize begin = order_.Size();
                for (uint32 member : members) {
                    if (request_.keys->keys[member].hasKey &&
                        (request_.reuse.IsEmpty() || request_.reuse[member] == 0)) {
                        (void)order_.TryPushBack(member);
                    }
                }

                algo::Sort(Span<uint32>(order_.Data() + begin, order_.Size() - begin),
                           [](uint32 left, uint32 right) noexcept { return left < right; });
                return Result<void>::Success();
            });
    }

    Result<void> CookOne(uint32 at) noexcept {
        const CookTarget& target = request_.targets->targets[at];
        const CookKey& key = request_.keys->keys[at];
        EditorPackageSet& packages = *request_.packages;
        // Order가 이미 찾은 항목이다.
        const AssetIndexEntry& entry = *request_.index->FindById(target.id).Value();
        if (entry.readOnly) {
            // 더 새 타입·커스텀 버전·섹션의 패키지다 — 지금 스키마로 쓰면 모르는 데이터를 잃는다.
            Diagnose(AssetDiagnosticCode::UnsupportedVersion, target.id, {}, ErrorCode::None);
            return Result<void>::Success();
        }

        if (packages.Find(target.id) == nullptr) {
            ++result_.opens;
            AssetDiagnostic failure;
            auto opened = packages.Open(target.id, nullptr, &failure);
            if (!opened) {
                if (opened.Error() == ErrorCode::OutOfMemory) {
                    return Result<void>::Failure(opened.Error());
                }

                Diagnose(failure.code, target.id, failure.related, opened.Error());
                return Result<void>::Success();
            }
        }

        const EditorPackageInfo& info = *packages.Find(target.id);
        if (!info.unresolved.IsEmpty()) {
            // null로 이은 import가 있다 — 그대로 쓰면 참조가 빠진 쿠킹 패키지가 된다.
            Diagnose(AssetDiagnosticCode::MissingDependency, target.id, info.unresolved[0].target,
                     info.unresolved[0].error);
            return Release();
        }

        auto processor = request_.processors->Find(entry.type);
        if (!processor) {
            return Result<void>::Failure(processor.Error());
        }

        MemoryPackageSink sink(allocator_);
        AssetDiagnostic failure;
        CookContext context;
        context.allocator = &allocator_;
        context.registry = &packages.Registry();
        context.access = packages.Access();
        context.package = info.package;
        context.entry = &entry;
        context.profile = request_.profile;
        context.profileSettings = request_.profileSettings;
        context.cookerVersion = request_.cookerVersion;
        context.bulkSource = info.store;
        context.nuggets = request_.nuggets;
        context.sink = &sink;
        context.failure = &failure;
        if (auto cooked = processor.Value()->Cook(context); !cooked) {
            if (cooked.Error() == ErrorCode::OutOfMemory) {
                return cooked;
            }

            Diagnose(failure.code, target.id, failure.related, cooked.Error());
            return Release();
        }

        // 쓴 byte를 쿠킹 패키지로 다시 읽어 검증하고 의존·bulk 크기를 얻는다.
        PackageReadOptions read;
        read.expectedKind = PackageKind::Cooked;
        auto contents = ReadPackage(allocator_, sink.Bytes(), read, nullptr);
        if (!contents) {
            if (contents.Error() == ErrorCode::OutOfMemory) {
                return Result<void>::Failure(contents.Error());
            }

            Diagnose(AssetDiagnosticCode::CorruptPackage, target.id, {}, contents.Error());
            return Release();
        }

        // 매니페스트 의존은 쿠킹 패키지 Dependencies 그대로다(로더가 같은지 대조한다 — AS 11.6절
        // 3). NeverCook·색인에 없는 Soft 대상은 매니페스트 항목이 없어 로드 요청이 NotFound다.
        checkEntry_.dependencies.Clear();
        if (auto reserved =
                checkEntry_.dependencies.TryReserve(contents.Value().dependencies.Size());
            !reserved) {
            return reserved;
        }

        for (const PackageDependencyEntry& dependency : contents.Value().dependencies) {
            (void)checkEntry_.dependencies.TryPushBack(
                CookedManifestDependency{dependency.asset, dependency.kind});
        }

        uint64 bulkRawBytes = 0;
        for (const PackageBulkEntry& bulk : contents.Value().bulkEntries) {
            bulkRawBytes += bulk.rawBytes;
        }

        // 만든 항목을 로더 대조로 확인한다(처리기가 다른 프로파일 값·대표 타입을 썼으면 게시 뒤
        // 로드가 거부할 것을 지금 잡는다).
        const Sha256Digest sha256 = Sha256::Hash(sink.Bytes());
        checkEntry_.asset = target.id;
        checkEntry_.type = entry.type;
        checkEntry_.packageSha256 = sha256;
        checkEntry_.packageBytes = sink.Bytes().Size();
        checkEntry_.bulkRawBytes = bulkRawBytes;
        AssetDiagnostic mismatch;
        if (auto checked =
                CheckCookedPackage(check_, checkEntry_, sink.Bytes(), contents.Value(), &mismatch);
            !checked) {
            Diagnose(AssetDiagnosticCode::CorruptPackage, target.id, mismatch.related,
                     checked.Error());
            return Release();
        }

        CookedPackage package;
        package.asset = target.id;
        package.type = entry.type;
        package.pathHint = entry.path.View();
        package.flags =
            target.primary ? static_cast<uint32>(CookedManifestFlags::PrimaryAsset) : 0u;
        package.bundles = target.bundles.View();
        package.dependencies = checkEntry_.dependencies.View();
        package.bytes = sink.Bytes();
        package.sha256 = sha256;
        package.bulkRawBytes = bulkRawBytes;
        package.cookKey = key.cookKey;
        if (auto put = request_.output->Put(package); !put) {
            return put;
        }

        ++result_.cooked;
        return Release();
    }

    // 이번 쿠킹이 연 패키지 수가 상한을 넘으면 시작 때 열린 것만 남기고 닫는다.
    Result<void> Release() noexcept {
        EditorPackageSet& packages = *request_.packages;
        if (packages.Count() - before_.Size() <= request_.maxOpenPackages) {
            return Result<void>::Success();
        }

        ++result_.releases;
        return packages.CloseOpened(before_.View());
    }

    Allocator& allocator_;
    const CookRunRequest& request_;
    CookRunResult& result_;
    // 시작 때 열린 패키지 id(id 순)·쿠킹 순서(대상 번호)다.
    Array<AssetId> before_;
    Array<uint32> order_;
    // 로더 대조용 매니페스트 머리와 항목 scratch다(항목 의존이 출력의 의존 목록이다).
    CookedManifest check_;
    CookedManifestEntry checkEntry_;
    ErrorCode error_ = ErrorCode::None;
};

} // namespace

Result<void> CookPackages(Allocator& allocator, const CookRunRequest& request,
                          CookRunResult& result) noexcept {
    result.diagnostics.Clear();
    result.cooked = 0;
    result.opens = 0;
    result.releases = 0;
    if (request.index == nullptr || request.packages == nullptr || request.processors == nullptr ||
        request.targets == nullptr || request.keys == nullptr || request.profile == nullptr ||
        request.output == nullptr || request.maxOpenPackages == 0 ||
        request.keys->keys.Size() != request.targets->targets.Size() ||
        (!request.reuse.IsEmpty() && request.reuse.Size() != request.targets->targets.Size())) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    CookWalk walk(allocator, request, result);
    return walk.Run();
}

} // namespace rocks
