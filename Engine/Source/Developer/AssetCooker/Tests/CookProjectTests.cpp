#include "CookProjectTestSupport.h"

#include <AssetCooker/AssetCooker.module.generated.h>
#include <Core/FileError.h>
#include <TestSupport/NFailAllocator.h>

// 쿠킹 전체 흐름 시험이다(Docs/Design/AssetSystem 10.2절 결정 8·10·11, "A4d 구현 세부" —
// A4d2): 전체 쿠킹 두 번(두 번째는 전부 재사용·같은 매니페스트 byte — 시나리오 13), 부분 쿠킹(바뀐
// 기존 항목과 그것을 Hard로 쓰는 항목만 다시 쿠킹, 나머지 재사용), 기존 매니페스트 없는 부분 쿠킹은
// 전체 쿠킹, 오류 진단이 있으면 게시하지 않음(게시본 불변·보고서는 씀·같은 입력이면 같은 보고서),
// 깨진 기존 매니페스트는 경고 뒤 전체 쿠킹, 요청 거부·읽기 IO 오류, 할당 실패.
using namespace rocks;

bool rocks::cook_project_test::RegisterCookerTypes(AssetEnv& env) noexcept {
    return static_cast<bool>(generated::AssetCooker::RegisterModuleTypes(env.modules));
}

namespace cook_project_tests_file {

using namespace rocks::editor_set_test;
using namespace rocks::cook_project_test;

namespace {

// verify 쿠킹이면 Leaf에서 NuggetMismatch로 실패하고(너겟 적중 대조 실패 흉내 — A4e1), 그 밖은
// 기본 Cook이다.
class MismatchProcessor final : public CookProcessor {
public:
    NuggetProcessorId Identity() const noexcept override {
        return DefaultCookProcessorId;
    }

    Result<void> Cook(CookContext& context) const noexcept override {
        if (context.verifyNuggets && context.entry->path.View() == "/Game/Leaf") {
            context.failure->code = AssetDiagnosticCode::NuggetMismatch;
            context.failure->error = ErrorCode::InvalidArgument;
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        return CookProcessor::Cook(context);
    }
};

bool Same(Span<const uint8> left, Span<const uint8> right) noexcept {
    if (left.Size() != right.Size()) {
        return false;
    }

    for (usize index = 0; index < left.Size(); ++index) {
        if (left[index] != right[index]) {
            return false;
        }
    }

    return true;
}

bool Contains(const String& text, StringView part) noexcept {
    const StringView view = text.View();
    for (usize at = 0; at + part.ByteCount() <= view.ByteCount(); ++at) {
        if (StringView(view.Data() + at, part.ByteCount()) == part) {
            return true;
        }
    }

    return false;
}

} // namespace

// 전체 쿠킹 두 번: 처음은 여덟을 쿠킹해 게시하고 보고서를 쓴다. 두 번째는 전부 재사용하고(쿠킹 0)
// 매니페스트 byte가 같다(시나리오 13 — 결정적 게시).
TEST(CookProject_FullTwiceReuses) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ProjectScene scene(env, "cook-project-full");
    REQUIRE(scene.ready);
    CookProjectResult result(env.allocator);
    REQUIRE(result.diagnostics.TryPushBack(CookDiagnostic{}));
    REQUIRE(CookProject(env.allocator, scene.Request(), result));
    CHECK(result.published && !result.partial && !result.HasErrors() &&
          result.diagnostics.IsEmpty());
    CHECK(result.targets == 8 && result.cooked == 8 && result.reused == 0 &&
          result.bytesWritten > 0);
    auto manifest = ReadCookedManifest(env.allocator, scene.files.Bytes(Manifest));
    REQUIRE(manifest);
    CHECK(manifest.Value().entries.Size() == 8 && manifest.Value().profileId.View() == "Windows");
    const String report = scene.ReportText();
    CHECK(Contains(report, "\"published\":true") && Contains(report, "\"cooked\":8") &&
          Contains(report, "\"profile\":\"Windows\"") && Contains(report, "\"diagnostics\":[]"));
    const Array<uint8> first = scene.Copy(Manifest);

    CookProjectResult again(env.allocator);
    REQUIRE(CookProject(env.allocator, scene.Request(), again));
    CHECK(again.published && again.cooked == 0 && again.reused == 8 && again.bytesWritten == 0);
    CHECK(Same(scene.files.Bytes(Manifest), first.View()));
    CHECK(Contains(scene.ReportText(), "\"reused\":8"));
}

// verify(10.2절 결정 12 — A4e1): 재사용 없이 모든 대상을 쿠킹하고(기본 처리기 — 같은 매니페스트
// byte), 처리기의 대조 실패 NuggetMismatch는 에셋 오류 진단이다(게시하지 않음·보고서 코드).
// verify가 아니면 같은 처리기도 대조하지 않는다(전부 재사용).
TEST(CookProject_VerifyNuggets) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ProjectScene scene(env, "cook-project-verify");
    REQUIRE(scene.ready);
    CookProjectResult first(env.allocator);
    REQUIRE(CookProject(env.allocator, scene.Request(), first));
    REQUIRE(first.published);
    const Array<uint8> manifest = scene.Copy(Manifest);

    CookProjectRequest request = scene.Request();
    request.verifyNuggets = true;
    CookProjectResult verified(env.allocator);
    REQUIRE(CookProject(env.allocator, request, verified));
    CHECK(verified.published && verified.cooked == 8 && verified.reused == 0 &&
          verified.diagnostics.IsEmpty());
    CHECK(Same(scene.files.Bytes(Manifest), manifest.View()));

    MismatchProcessor mismatch;
    CookProcessorTable table(env.allocator, env.types, mismatch);
    request.processors = &table;
    CookProjectResult failed(env.allocator);
    REQUIRE(CookProject(env.allocator, request, failed));
    REQUIRE(failed.diagnostics.Size() == 1);
    CHECK(!failed.published && failed.reused == 0 &&
          failed.diagnostics[0].code == AssetDiagnosticCode::NuggetMismatch &&
          !failed.diagnostics[0].warning &&
          failed.diagnostics[0].error == ErrorCode::InvalidArgument);
    CHECK(Same(scene.files.Bytes(Manifest), manifest.View()));
    CHECK(Contains(scene.ReportText(), "\"code\":\"NuggetMismatch\""));

    request.verifyNuggets = false;
    CookProjectResult plain(env.allocator);
    REQUIRE(CookProject(env.allocator, request, plain));
    CHECK(plain.published && plain.cooked == 0 && plain.reused == 8);
}

// 부분 쿠킹: Leaf 내용을 바꾸고 Hero만 인자로 주면 기존 항목 중 바뀐 Leaf와 그것을 Hard로 쓰는
// Hub만 다시 쿠킹하고 나머지는 재사용한다(10절 — cookKey가 달라진 기존 항목). 병합 호환이 아니면
// 쿠킹 전에 IncompatibleCook으로 끝낸다. 기존 매니페스트가 없으면 부분 쿠킹은 전체 쿠킹이다.
TEST(CookProject_PartialRecooksChanged) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ProjectScene scene(env, "cook-project-partial");
    REQUIRE(scene.ready);
    CookProjectResult result(env.allocator);
    REQUIRE(CookProject(env.allocator, scene.Request(), result) && result.published);
    auto before = ReadCookedManifest(env.allocator, scene.files.Bytes(Manifest));
    REQUIRE(before);
    const Sha256Digest oldLeaf = before.Value().Find(FixedId(LeafId))->packageSha256;

    // Leaf를 다른 값으로 다시 쓴다(같은 id — target Registry).
    RootRef<Package> package;
    const ObjectHandle leaf = MakeAsset<TestAsset>(*env.target, LeafId, package);
    TestAsset* value = Pointer<TestAsset>(*env.target, leaf);
    REQUIRE(value != nullptr);
    value->value = HeroValue + 1;
    const Array<uint8> changed =
        Write(env.allocator, *env.target, package.Handle(), EditorOptions());
    REQUIRE(!changed.IsEmpty());
    scene.files.Remove("Content/Leaf.rasset");
    REQUIRE(scene.files.AddFile("Content/Leaf.rasset", changed.View()));
    ++scene.files.modifiedShift;
    REQUIRE(scene.index.Rebuild(scene.files, ProjectScene::ReadOptions()));

    const StringView hero[] = {"/Game/Characters/Hero"};
    CookProjectResult partial(env.allocator);
    REQUIRE(CookProject(env.allocator, scene.Request(hero), partial));
    CHECK(partial.published && partial.partial && !partial.HasErrors());
    CHECK(partial.cooked == 2 && partial.reused == 6 && partial.targets == 8);
    auto after = ReadCookedManifest(env.allocator, scene.files.Bytes(Manifest));
    REQUIRE(after && after.Value().entries.Size() == 8);
    CHECK(!(after.Value().Find(FixedId(LeafId))->packageSha256 == oldLeaf));
    CHECK(after.Value().Find(FixedId(HeroId))->packageSha256 ==
          before.Value().Find(FixedId(HeroId))->packageSha256);

    // 병합 호환이 아니면(CookSettings가 다름) 쿠킹하지 않고 IncompatibleCook으로 끝낸다 — 게시본
    // 그대로, 보고서는 쓴다.
    const Array<uint8> mergedBytes = scene.Copy(Manifest);
    CookProjectRequest other = scene.Request(hero);
    other.cookSettings = Sha256::Hash(Span<const uint8>(reinterpret_cast<const uint8*>("X"), 1));
    CookProjectResult incompatible(env.allocator);
    REQUIRE(CookProject(env.allocator, other, incompatible));
    CHECK(!incompatible.published && incompatible.partial && incompatible.cooked == 0 &&
          incompatible.targets == 0);
    REQUIRE(incompatible.diagnostics.Size() == 1);
    CHECK(incompatible.diagnostics[0].code == AssetDiagnosticCode::IncompatibleCook &&
          !incompatible.diagnostics[0].warning);
    CHECK(Same(scene.files.Bytes(Manifest), mergedBytes.View()));
    CHECK(Contains(scene.ReportText(), "\"code\":\"IncompatibleCook\""));

    // 기존 매니페스트가 없으면 인자가 있어도 전체 쿠킹이다.
    AssetEnv freshEnv;
    REQUIRE(freshEnv.Ready());
    ProjectScene fresh(freshEnv, "cook-project-nomanifest");
    REQUIRE(fresh.ready);
    CookProjectResult full(freshEnv.allocator);
    REQUIRE(CookProject(freshEnv.allocator, fresh.Request(hero), full));
    CHECK(full.published && !full.partial && full.cooked == 8);
}

// 오류: 키 계산 뒤 사라진 파일(Leaf — 콘텐츠 해시 MissingFile)이 있으면 게시하지 않는다. 게시본은
// 그대로이고 보고서는 published false와 진단을 담는다. 같은 입력이면 같은 보고서 byte다.
TEST(CookProject_ErrorsKeepPublished) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ProjectScene scene(env, "cook-project-errors");
    REQUIRE(scene.ready);
    CookProjectResult result(env.allocator);
    REQUIRE(CookProject(env.allocator, scene.Request(), result) && result.published);
    const Array<uint8> published = scene.Copy(Manifest);
    scene.files.Remove("Content/Leaf.rasset");

    CookProjectResult failed(env.allocator);
    REQUIRE(CookProject(env.allocator, scene.Request(), failed));
    CHECK(!failed.published && failed.HasErrors());
    bool missing = false;
    for (const CookDiagnostic& diagnostic : failed.diagnostics) {
        missing = missing || (diagnostic.code == AssetDiagnosticCode::MissingFile &&
                              diagnostic.asset == FixedId(LeafId));
    }

    CHECK(missing);
    CHECK(Same(scene.files.Bytes(Manifest), published.View()));
    const String report = scene.ReportText();
    CHECK(Contains(report, "\"published\":false") && Contains(report, "\"code\":\"MissingFile\"") &&
          Contains(report, "\"level\":\"error\""));
    const Array<uint8> firstReport = scene.Copy(Report);
    CookProjectResult again(env.allocator);
    REQUIRE(CookProject(env.allocator, scene.Request(), again));
    CHECK(Same(scene.files.Bytes(Report), firstReport.View()));
}

// 깨진 기존 매니페스트(상한 초과 포함)는 경고(그 진단 코드) 뒤 없는 것으로 보고 전체 쿠킹해
// 게시한다. 요청 거부: 인자 null InvalidArgument(결과 비움)·잠금 아님 InvalidArgument, 기존
// 매니페스트 읽기 IO 오류·보고서 struct 미등록(NotFound)은 그대로다.
TEST(CookProject_CorruptManifestAndRejects) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ProjectScene scene(env, "cook-project-rejects");
    REQUIRE(scene.ready);
    const uint8 junk[] = {'J', 'U', 'N', 'K'};
    REQUIRE(scene.files.AddDirectory("Saved") && scene.files.AddDirectory("Saved/Cooked") &&
            scene.files.AddDirectory("Saved/Cooked/Windows") &&
            scene.files.AddFile(Manifest, junk));
    CookProjectResult result(env.allocator);
    REQUIRE(CookProject(env.allocator, scene.Request(), result));
    CHECK(result.published && result.cooked == 8 && !result.HasErrors());
    REQUIRE(result.diagnostics.Size() == 1);
    CHECK(result.diagnostics[0].warning &&
          result.diagnostics[0].code == AssetDiagnosticCode::CorruptPackage);

    // 상한을 넘는 기존 매니페스트도 깨진 것으로 본다(경고 CorruptPackage / CapacityExceeded 뒤 전체
    // 쿠킹 — 사용자가 지우기 전까지 멈추지 않는다).
    CookProjectRequest limited = scene.Request();
    limited.manifestLimitBytes = 16;
    REQUIRE(CookProject(env.allocator, limited, result));
    CHECK(result.published && !result.HasErrors());
    REQUIRE(result.diagnostics.Size() == 1);
    CHECK(result.diagnostics[0].warning &&
          result.diagnostics[0].code == AssetDiagnosticCode::CorruptPackage &&
          result.diagnostics[0].error == ErrorCode::CapacityExceeded);

    const auto rejects = [&](void (*mutate)(CookProjectRequest&)) noexcept {
        CookProjectRequest request = scene.Request();
        mutate(request);
        return CookProject(env.allocator, request, result).Error() == ErrorCode::InvalidArgument;
    };
    CHECK(rejects([](CookProjectRequest& r) noexcept { r.files = nullptr; }));
    CHECK(result.diagnostics.IsEmpty() && !result.published);
    CHECK(rejects([](CookProjectRequest& r) noexcept { r.layout = nullptr; }));
    CHECK(rejects([](CookProjectRequest& r) noexcept { r.lock = nullptr; }));
    CHECK(rejects([](CookProjectRequest& r) noexcept { r.index = nullptr; }));
    CHECK(rejects([](CookProjectRequest& r) noexcept { r.packages = nullptr; }));
    CHECK(rejects([](CookProjectRequest& r) noexcept { r.types = nullptr; }));
    CHECK(rejects([](CookProjectRequest& r) noexcept { r.processors = nullptr; }));
    CHECK(rejects([](CookProjectRequest& r) noexcept { r.profile = nullptr; }));
    CHECK(rejects([](CookProjectRequest& r) noexcept { r.settings = nullptr; }));
    CHECK(rejects([](CookProjectRequest& r) noexcept { r.cookLock = nullptr; }));
    // 쿠킹 루트 잠금만 쥐고 에셋 DB 잠금이 없으면 거부한다(잠금 순서 — 10.3.1절 A4f4 2번).
    AssetDatabaseLock unheld;
    CookProjectRequest request = scene.Request();
    request.lock = &unheld;
    CHECK(CookProject(env.allocator, request, result).Error() == ErrorCode::InvalidArgument);
    // 쿠킹 루트 잠금이 없거나 다른 쿠킹 루트(다른 프로필)의 것이면 거부한다.
    CookRootLock noCookLock;
    CookProjectRequest unlocked = scene.Request();
    unlocked.cookLock = &noCookLock;
    CHECK(CookProject(env.allocator, unlocked, result).Error() == ErrorCode::InvalidArgument);
    CookRootLock otherRoot;
    REQUIRE(otherRoot.Acquire(env.allocator, scene.layout.SavedDirectory(), "Android", 0));
    CookProjectRequest elsewhere = scene.Request();
    elsewhere.cookLock = &otherRoot;
    CHECK(CookProject(env.allocator, elsewhere, result).Error() == ErrorCode::InvalidArgument);

    const Array<uint8> current = scene.Copy(Manifest);
    scene.files.Remove(Manifest);
    REQUIRE(scene.files.AddFile(Manifest, current.View(), MemoryFault::Read));
    CHECK(CookProject(env.allocator, scene.Request(), result).Error() ==
          FileErrorCode(asset_database_test::MemoryError));

    // 보고서 struct가 등록되지 않았으면 보고서 쓰기의 타입 조회 오류(NotFound)다.
    AssetEnv bareEnv;
    REQUIRE(bareEnv.Ready());
    ProjectScene bare(bareEnv, "cook-project-bare", false);
    REQUIRE(bare.ready);
    CookProjectResult bareResult(bareEnv.allocator);
    CHECK(CookProject(bareEnv.allocator, bare.Request(), bareResult).Error() ==
          ErrorCode::NotFound);
}

// 할당 실패: 첫 쿠킹·재사용 쿠킹·깨진 기존 매니페스트(경고)·오류 진단 쿠킹(보고서 진단 줄 포함)의
// 모든 지점에서 OutOfMemory이고(결과 객체도 같은 allocator), 지점을 다 지나면 기준과 같다.
TEST(CookProject_AllocationFailures) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ProjectScene scene(env, "cook-project-oom");
    REQUIRE(scene.ready);
    test::NFailAllocator failing(env.allocator);
    const uint8 junk[] = {'J', 'U', 'N', 'K'};
    // 0: 첫 쿠킹, 1: 재사용, 2: 깨진 기존 매니페스트(경고), 3: 게시기 경고만(`.staging` 고아 삭제
    // 실패), 4: 대상 경고만(없는 alwaysCook 경로), 5: 병합 호환 아님(부분 — IncompatibleCook),
    // 6: 오류 진단(사라진 파일 — 게시 안 함).
    constexpr int IncompatiblePass = 5;
    constexpr int ErrorPass = 6;
    const StringView heroPath[] = {"/Game/Characters/Hero"};
    const String orphan = [&]() noexcept {
        String name(env.allocator);
        CHECK(name.TryAssign("Saved/Cooked/Windows/.staging/orphan.tmp"));
        return name;
    }();
    for (int pass = 0; pass <= ErrorPass; ++pass) {
        if (pass == 4) {
            scene.files.faults.remove = {};
            scene.files.Remove(orphan.View());
            String nowhere(env.allocator);
            REQUIRE(nowhere.TryAssign("/Game/Nope") &&
                    scene.settings.alwaysCook.TryPushBack(Move(nowhere)));
        }

        if (pass == ErrorPass) {
            scene.files.Remove("Content/Leaf.rasset");
        }

        uint32 failures = 0;
        for (uint64 attempt = 1;; ++attempt) {
            if (pass == 0) {
                scene.files.Remove(Manifest);
            }

            if (pass == 2) {
                scene.files.Remove(Manifest);
                REQUIRE(scene.files.AddFile(Manifest, junk));
            }

            if (pass == 3) {
                scene.files.faults.remove = {};
                scene.files.Remove(orphan.View());
                REQUIRE(scene.files.AddFile(orphan.View(), junk));
                scene.files.faults.remove = asset_database_test::MemoryError;
            }

            CookProjectResult result(failing);
            CookProjectRequest request = scene.Request();
            if (pass == IncompatiblePass) {
                request = scene.Request(heroPath);
                request.cookSettings =
                    Sha256::Hash(Span<const uint8>(reinterpret_cast<const uint8*>("X"), 1));
            }

            failing.FailOnAttempt(attempt);
            auto cooked = CookProject(failing, request, result);
            const bool hit = failing.Attempts() >= attempt;
            failing.DisableFailure();
            CHECK(scene.set.Count() == 0);
            if (!hit) {
                REQUIRE(cooked);
                CHECK((pass == ErrorPass || pass == IncompatiblePass) ? !result.published
                                                                      : result.published);
                break;
            }

            CHECK(cooked.Error() == ErrorCode::OutOfMemory);
            ++failures;
        }

        CHECK(failures >= (pass == IncompatiblePass ? 3u : 20u));
    }
}

} // namespace cook_project_tests_file
