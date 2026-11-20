#include "LoaderTestSupport.h"

#include <Platform/Thread.h>
#include <TestSupport/NFailAllocator.h>

// 로더 → AssetManager 넘겨받기 API 시험이다(Docs/Design/AssetSystem.ko.md 11.7절 2):
// TakePublished는 PostLoad까지 끝난 단위만 같은 group으로 넘기고, PublishedHolds는 공개된 패키지에
// Hard로 기대는 진행 중 단위 수이며, RetirePublished·ReleaseRetired는 모두 확인한 뒤에만 바꾼다. 각
// API의 초기화 전·다른 스레드·콜백 안·종료 뒤·할당 실패 오류를 확인한다.
using namespace rocks;
namespace loader_handoff_tests_file {

using namespace rocks::asset_test;

namespace {

ErrorCode ErrorOf(const Result<void>& result) noexcept {
    return result ? ErrorCode::None : result.Error();
}

template <class T>
ErrorCode ErrorOf(const Result<T>& result) noexcept {
    return result ? ErrorCode::None : result.Error();
}

AssetLoaderPumpBudget OnePostLoad() noexcept {
    AssetLoaderPumpBudget budget;
    budget.maxPostLoads = 1;
    return budget;
}

// IO·worker를 내보내며 budget으로 Pump를 한 번 한다.
bool Step(LoaderRig& rig, const AssetLoaderPumpBudget& budget) noexcept {
    rig.io.Complete();
    rig.worker.RunAll();
    return rig.loader->Pump(budget).IsOk();
}

bool Contains(Span<const uint32> values, uint32 value) noexcept {
    for (uint32 one : values) {
        if (one == value) {
            return true;
        }
    }

    return false;
}

// PostLoad 안에서 넘겨받기 API를 부른다(콜백 중 — WrongContext).
class ReentrantPostLoad final : public AssetPostLoadHandler {
public:
    explicit ReentrantPostLoad(Allocator& allocator) noexcept : taken(allocator) {
    }

    void PostLoad(ObjectHandle, AssetId asset) noexcept override {
        if (loader == nullptr) {
            return;
        }

        const AssetId assets[] = {asset};
        const uint32 slots[] = {0};
        take = ErrorOf(loader->TakePublished(taken));
        retire = ErrorOf(loader->RetirePublished(assets));
        release = ErrorOf(loader->ReleaseRetired(slots));
        holds = ErrorOf(loader->PublishedHolds(asset));
        ++calls;
    }

    AssetLoader* loader = nullptr;
    Array<AssetLoaderPublished> taken;
    ErrorCode take = ErrorCode::None;
    ErrorCode retire = ErrorCode::None;
    ErrorCode release = ErrorCode::None;
    ErrorCode holds = ErrorCode::None;
    uint32 calls = 0;
};

} // namespace

// 넘기기 시점(11.7절 2): PostLoad 예산 1이면 순환 단위 1 ↔ 2는 한 Pump에서 공개되고 PostLoad 하나만
// 불려 아직 넘기지 않는다. 두 번째 PostLoad가 끝난 Pump 뒤에 둘이 같은 group·다른 slot으로 넘어오고
// 따로 끝난 3은 다른 group이다. out의 기존 내용은 앞에 남고, 다시 부르면 새 것이 없다.
TEST(AssetLoader_TakePublishedAfterPostLoad) {
    AssetEnv env;
    REQUIRE(env.Ready());
    LoaderRig rig(env);
    rig.scene.Add(1);
    rig.scene.Add(2);
    rig.scene.Add(3);
    rig.scene.Hard(1, 2);
    rig.scene.Hard(2, 1);
    REQUIRE(rig.Cook());
    RecordingPostLoad postLoad(env.allocator);
    rig.config.postLoad = &postLoad;
    REQUIRE(rig.Start());
    const AssetLoadHandle pair = rig.Request(1);
    Array<AssetLoaderPublished> out(env.allocator);
    AssetLoaderPublished sentinel;
    sentinel.asset = FixedId(77);
    REQUIRE(out.TryPushBack(sentinel));
    constexpr uint32 StepLimit = 64;
    bool half = false;
    for (uint32 round = 0; round < StepLimit && !half; ++round) {
        REQUIRE(Step(rig, OnePostLoad()));
        half = postLoad.order.Size() == 1;
    }

    REQUIRE(half);
    CHECK(rig.IsLoaded(1) && rig.IsLoaded(2));
    REQUIRE(rig.loader->TakePublished(out));
    CHECK(out.Size() == 1);
    REQUIRE(Step(rig, OnePostLoad()));
    CHECK(postLoad.order.Size() == 2);
    REQUIRE(rig.loader->TakePublished(out));
    REQUIRE(out.Size() == 3);
    CHECK(out[0].asset == FixedId(77));
    CHECK(out[1].group == out[2].group && out[1].slot != out[2].slot);
    CHECK((out[1].asset == FixedId(1) && out[2].asset == FixedId(2)) ||
          (out[1].asset == FixedId(2) && out[2].asset == FixedId(1)));
    CHECK(out[1].package == rig.loader->FindPackage(out[1].asset).Value());

    const AssetLoadHandle single = rig.Request(3);
    REQUIRE(rig.SettleHeld(false));
    REQUIRE(rig.loader->TakePublished(out));
    REQUIRE(out.Size() == 4);
    CHECK(out[3].asset == FixedId(3) && out[3].group > out[1].group);
    REQUIRE(rig.loader->TakePublished(out));
    CHECK(out.Size() == 4);
    (void)rig.loader->Release(pair);
    (void)rig.loader->Release(single);
}

// 공개 보유(11.4절 "로드 작업의 내부 보유"): 공개된 3에 Hard로 기대는 4가 IO 중이면
// PublishedHolds(3)은 1이고 그동안 3은 퇴출할 수 없다. 4가 끝나면 0이다. 공개되지 않은 id는 0, 없는
// id는 NotFound다.
TEST(AssetLoader_PublishedHoldsTrackLoadingDependents) {
    AssetEnv env;
    REQUIRE(env.Ready());
    LoaderRig rig(env);
    rig.scene.Add(3);
    rig.scene.Add(4);
    rig.scene.Add(5);
    rig.scene.Add(6);
    rig.scene.Hard(4, 3);
    rig.scene.Hard(5, 6);
    rig.scene.Hard(6, 5);
    rig.scene.Hard(5, 3);
    rig.scene.Hard(6, 3);
    REQUIRE(rig.Cook());
    REQUIRE(rig.Start());
    const AssetLoadHandle base = rig.Request(3);
    REQUIRE(rig.SettleHeld(false));
    CHECK(rig.loader->PublishedHolds(FixedId(3)).Value() == 0);
    CHECK(rig.loader->PublishedHolds(FixedId(4)).Value() == 0);
    CHECK(rig.loader->PublishedHolds(FixedId(99)).Error() == ErrorCode::NotFound);

    const AssetLoadHandle dependent = rig.Request(4);
    REQUIRE(rig.loader->Pump());
    REQUIRE(!rig.io.held.IsEmpty() || rig.io.active != 0);
    CHECK(rig.loader->PublishedHolds(FixedId(3)).Value() == 1);
    const AssetId held[] = {FixedId(3)};
    CHECK(rig.loader->RetirePublished(held).Error() == ErrorCode::InvalidArgument);
    REQUIRE(rig.SettleHeld(false));
    CHECK(rig.loader->PublishedHolds(FixedId(3)).Value() == 0);

    // 한 단위의 두 구성원이 같은 공개 패키지에 기대도 보유는 단위당 하나다.
    const AssetLoadHandle pair = rig.Request(5);
    REQUIRE(rig.loader->Pump());
    CHECK(rig.loader->PublishedHolds(FixedId(3)).Value() == 1);
    REQUIRE(rig.SettleHeld(false));
    CHECK(rig.loader->PublishedHolds(FixedId(3)).Value() == 0);
    (void)rig.loader->Release(base);
    (void)rig.loader->Release(dependent);
    (void)rig.loader->Release(pair);
}

// 공개 보유 할당 실패(11.6절 13·11.7절 2): 공개된 3에 기대는 4의 요청에서 로더 allocator의 어느
// 할당이 실패해도 그 요청만 OutOfMemory이고 3의 공개 보유가 남지 않는다. 실패를 끄고 다시 요청하면
// 성공한다.
TEST(AssetLoader_PublishedHoldAllocationFailures) {
    AssetEnv env;
    REQUIRE(env.Ready());
    test::NFailAllocator failing(env.allocator);
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        LoaderRig rig(env);
        rig.scene.Add(3);
        rig.scene.Add(4);
        rig.scene.Hard(4, 3);
        REQUIRE(rig.Cook());
        rig.config.allocator = &failing;
        REQUIRE(rig.Start());
        const AssetLoadHandle base = rig.Request(3);
        REQUIRE(rig.SettleHeld(false));
        failing.FailOnAttempt(attempt);
        AssetLoadRequest request;
        request.id = FixedId(4);
        auto handle = rig.loader->Request(request);
        const bool failed = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!handle) {
            CHECK(handle.Error() == ErrorCode::OutOfMemory);
        }

        REQUIRE(rig.SettleHeld(false));
        CHECK(rig.loader->PublishedHolds(FixedId(3)).Value() == 0);
        if (handle) {
            CHECK(rig.StateOf(handle.Value()) == AssetLoadState::Succeeded ||
                  rig.StatusOf(handle.Value()).error == ErrorCode::OutOfMemory);
            (void)rig.loader->Release(handle.Value());
        }

        const AssetLoadHandle again = rig.Request(4);
        REQUIRE(rig.SettleHeld(false));
        CHECK(rig.StateOf(again) == AssetLoadState::Succeeded);
        (void)rig.loader->Release(again);
        (void)rig.loader->Release(base);
        if (!failed) {
            break;
        }

        ++failures;
    }

    CHECK(failures > 0);
}

// PostLoad 중인 기대는 단위(11.6절 16·11.7절 2): PostLoad 예산 1이면 순환 의존 단위 D {11..16}이
// PostLoad를 마치기 전에 D와 공개된 20에 기대는 30이 공개되어 D를 기다리며 PostLoad 대기다. D가
// 끝날 때 30은 이미 공개 뒤라 D의 패키지를 공개 보유로 넘겨받지 않는다(30 공개 때 20의 보유도
// 놓았다). PostLoad는 D 전부 뒤에 30이다.
TEST(AssetLoader_DependencyFinishesAfterDependentPublished) {
    AssetEnv env;
    REQUIRE(env.Ready());
    LoaderRig rig(env);
    constexpr uint64 First = 11;
    constexpr uint64 Last = 16;
    for (uint64 id = First; id <= Last; ++id) {
        rig.scene.Add(id);
        rig.scene.Hard(id, id == Last ? First : id + 1);
    }

    rig.scene.Add(20);
    rig.scene.Add(30);
    rig.scene.Hard(30, First);
    rig.scene.Hard(30, 20);
    REQUIRE(rig.Cook());
    RecordingPostLoad postLoad(env.allocator);
    rig.config.postLoad = &postLoad;
    REQUIRE(rig.Start());
    const AssetLoadHandle published = rig.Request(20);
    REQUIRE(rig.SettleHeld(false));
    postLoad.order.Clear();
    postLoad.packages.Clear();

    const AssetLoadHandle cycle = rig.Request(First);
    constexpr uint32 StepLimit = 64;
    bool committed = false;
    for (uint32 round = 0; round < StepLimit && !committed; ++round) {
        REQUIRE(Step(rig, OnePostLoad()));
        committed = rig.IsLoaded(First);
    }

    REQUIRE(committed);
    const AssetLoadHandle dependent = rig.Request(30);
    bool waiting = false;
    for (uint32 round = 0; round < StepLimit && !waiting; ++round) {
        REQUIRE(Step(rig, OnePostLoad()));
        waiting = rig.IsLoaded(30);
    }

    REQUIRE(waiting);
    REQUIRE(postLoad.order.Size() < Last - First + 1);
    CHECK(rig.loader->PublishedHolds(FixedId(20)).Value() == 0);
    REQUIRE(rig.SettleHeld(false));
    CHECK(rig.loader->PublishedHolds(FixedId(First)).Value() == 0);
    REQUIRE(postLoad.order.Size() == Last - First + 2);
    CHECK(postLoad.order[postLoad.order.Size() - 1] == FixedId(30));
    CHECK(rig.StateOf(dependent) == AssetLoadState::Succeeded);
    (void)rig.loader->Release(published);
    (void)rig.loader->Release(cycle);
    (void)rig.loader->Release(dependent);
}

// 퇴출과 칸 반환(11.7절 2·3): 모두 확인한 뒤에만 바꾼다 — 없는 id NotFound, 공개 안 됨·PostLoad 중·
// 같은 id 두 번은 InvalidArgument이고 앞의 유효한 id도 그대로다. 퇴출하면 FindPackage가
// NotFound이고 같은 id 요청은 새 로드다. 칸 반환은 범위 밖·퇴출 아님·같은 칸 두 번이
// InvalidArgument이고, 할당 실패면 아무것도 바꾸지 않는다. 돌려준 칸은 다음 공개가 다시 쓴다.
TEST(AssetLoader_RetireAndReleaseRetired) {
    AssetEnv env;
    REQUIRE(env.Ready());
    test::NFailAllocator failing(env.allocator);
    LoaderRig rig(env);
    rig.scene.Add(1);
    rig.scene.Add(2);
    rig.scene.Add(3);
    rig.scene.Add(5);
    rig.scene.Add(6);
    rig.scene.Hard(5, 6);
    rig.scene.Hard(6, 5);
    REQUIRE(rig.Cook());
    RecordingPostLoad postLoad(env.allocator);
    rig.config.postLoad = &postLoad;
    rig.config.allocator = &failing;
    REQUIRE(rig.Start());
    const AssetLoadHandle one = rig.Request(1);
    const AssetLoadHandle three = rig.Request(3);
    REQUIRE(rig.SettleHeld(false));
    Array<AssetLoaderPublished> out(env.allocator);
    REQUIRE(rig.loader->TakePublished(out));
    REQUIRE(out.Size() == 2);

    const AssetId missing[] = {FixedId(3), FixedId(99)};
    CHECK(rig.loader->RetirePublished(missing).Error() == ErrorCode::NotFound);
    const AssetId unloaded[] = {FixedId(3), FixedId(2)};
    CHECK(rig.loader->RetirePublished(unloaded).Error() == ErrorCode::InvalidArgument);
    const AssetId twice[] = {FixedId(3), FixedId(3)};
    CHECK(rig.loader->RetirePublished(twice).Error() == ErrorCode::InvalidArgument);
    CHECK(rig.IsLoaded(3));

    // PostLoad 중(단위가 남은) 패키지는 퇴출할 수 없다.
    const AssetLoadHandle pair = rig.Request(5);
    bool half = false;
    constexpr uint32 StepLimit = 64;
    for (uint32 round = 0; round < StepLimit && !half; ++round) {
        REQUIRE(Step(rig, OnePostLoad()));
        half = rig.IsLoaded(5) && postLoad.order.Size() == 3;
    }

    REQUIRE(half);
    const AssetId pending[] = {FixedId(5)};
    CHECK(rig.loader->RetirePublished(pending).Error() == ErrorCode::InvalidArgument);
    REQUIRE(rig.SettleHeld(false));

    const AssetId both[] = {FixedId(1), FixedId(3)};
    REQUIRE(rig.loader->RetirePublished(both));
    CHECK(!rig.IsLoaded(1) && !rig.IsLoaded(3));
    CHECK(rig.loader->RetirePublished(both).Error() == ErrorCode::InvalidArgument);

    const uint32 slots[] = {out[0].slot, out[1].slot};
    const uint32 outOfRange[] = {out[0].slot, 1u << 20};
    CHECK(rig.loader->ReleaseRetired(outOfRange).Error() == ErrorCode::InvalidArgument);
    Array<AssetLoaderPublished> pairOut(env.allocator);
    REQUIRE(rig.loader->TakePublished(pairOut));
    REQUIRE(pairOut.Size() == 2);
    const uint32 notRetired[] = {out[0].slot, pairOut[0].slot};
    CHECK(rig.loader->ReleaseRetired(notRetired).Error() == ErrorCode::InvalidArgument);
    const uint32 sameTwice[] = {out[0].slot, out[0].slot};
    CHECK(rig.loader->ReleaseRetired(sameTwice).Error() == ErrorCode::InvalidArgument);
    failing.FailOnAttempt(1);
    CHECK(rig.loader->ReleaseRetired(slots).Error() == ErrorCode::OutOfMemory);
    failing.DisableFailure();
    REQUIRE(rig.loader->ReleaseRetired(slots));
    CHECK(rig.loader->ReleaseRetired(slots).Error() == ErrorCode::InvalidArgument);

    // 같은 id 요청은 새 로드이고 돌려준 칸을 다시 쓴다.
    const usize begun = rig.io.begun.Size();
    const AssetLoadHandle again = rig.Request(1);
    REQUIRE(rig.SettleHeld(false));
    CHECK(rig.IsLoaded(1) && rig.io.begun.Size() == begun + 1);
    Array<AssetLoaderPublished> reused(env.allocator);
    REQUIRE(rig.loader->TakePublished(reused));
    REQUIRE(reused.Size() == 1);
    CHECK(Contains(slots, reused[0].slot));

    // TakePublished 할당 실패는 목록을 남긴다(다음 호출이 받는다).
    const AssetLoadHandle last = rig.Request(3);
    REQUIRE(rig.SettleHeld(false));
    test::NFailAllocator outFailing(env.allocator);
    Array<AssetLoaderPublished> refused(outFailing);
    outFailing.FailOnAttempt(1);
    CHECK(rig.loader->TakePublished(refused).Error() == ErrorCode::OutOfMemory);
    outFailing.DisableFailure();
    REQUIRE(rig.loader->TakePublished(refused));
    CHECK(refused.Size() == 1 && refused[0].asset == FixedId(3));

    REQUIRE(rig.loader->Shutdown());
    CHECK(rig.loader->RetirePublished(both).Error() == ErrorCode::ShuttingDown);
    (void)one;
    (void)three;
    (void)pair;
    (void)again;
    (void)last;
}

// 초기화 전·다른 스레드·콜백 안(11.7절 2): 넷 모두 초기화 전 InvalidArgument, 다른 스레드
// WrongThread이고, PostLoad 안에서는 PublishedHolds를 뺀 셋이 WrongContext다(PublishedHolds는
// 읽기라 콜백 안에서도 된다).
TEST(AssetLoader_HandoffContractErrors) {
    AssetEnv env;
    REQUIRE(env.Ready());
    LoaderRig rig(env);
    rig.scene.Add(1);
    REQUIRE(rig.Cook());
    ReentrantPostLoad reentrant(env.allocator);
    rig.config.postLoad = &reentrant;
    auto made = UniquePtr<AssetLoader>::TryMake(env.allocator, rig.config);
    REQUIRE(made);
    Array<AssetLoaderPublished> out(env.allocator);
    const AssetId assets[] = {FixedId(1)};
    const uint32 slots[] = {0};
    CHECK(ErrorOf(made.Value()->TakePublished(out)) == ErrorCode::InvalidArgument);
    CHECK(ErrorOf(made.Value()->PublishedHolds(FixedId(1))) == ErrorCode::InvalidArgument);
    CHECK(ErrorOf(made.Value()->RetirePublished(assets)) == ErrorCode::InvalidArgument);
    CHECK(ErrorOf(made.Value()->ReleaseRetired(slots)) == ErrorCode::InvalidArgument);

    REQUIRE(rig.Start());
    reentrant.loader = rig.loader.Get();
    ErrorCode errors[4] = {};
    auto body = [&]() noexcept {
        Array<AssetLoaderPublished> other(env.allocator);
        errors[0] = ErrorOf(rig.loader->TakePublished(other));
        errors[1] = ErrorOf(rig.loader->PublishedHolds(FixedId(1)));
        errors[2] = ErrorOf(rig.loader->RetirePublished(assets));
        errors[3] = ErrorOf(rig.loader->ReleaseRetired(slots));
    };
    platform::Thread thread;
    REQUIRE(thread.Start([](void* context) noexcept { (*static_cast<decltype(body)*>(context))(); },
                         &body) == platform::ThreadStatus::Success);
    REQUIRE(thread.Join() == platform::ThreadStatus::Success);
    for (ErrorCode error : errors) {
        CHECK(error == ErrorCode::WrongThread);
    }

    const AssetLoadHandle handle = rig.Request(1);
    REQUIRE(rig.SettleHeld(false));
    CHECK(reentrant.calls == 1);
    CHECK(reentrant.take == ErrorCode::WrongContext &&
          reentrant.retire == ErrorCode::WrongContext &&
          reentrant.release == ErrorCode::WrongContext && reentrant.holds == ErrorCode::None);
    REQUIRE(rig.loader->TakePublished(out));
    CHECK(out.Size() == 1);
    (void)rig.loader->Release(handle);
}

} // namespace loader_handoff_tests_file
