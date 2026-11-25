#include "RecordingDevice.h"
#include "RegistryShaderSet.h"

#include <Core/Memory/SystemAllocator.h>
#include <Core/Memory/UniquePtr.h>
#include <RenderBase/EngineShaderTypes.h>
#include <RenderBase/FullscreenPass.h>
#include <RenderBase/ShaderSetSlot.h>
#include <ShaderCore/ShaderTypeRegistry.h>
#include <TestSupport/Test.h>

// 셰이더 집합 교체 시험이다(Docs/Design/RenderBase 9.8절, 9.10절 RB4의 교체 성공·실패): 첫 집합,
// 실패 후보의 거부와 현재 유지, 프레임 경계 교체(옛 집합의 캐시 항목·장치 셰이더 회수, epoch가
// 다른 새 캐시 항목), lease가 남은 옛 집합의 회수 대기, 공급 대기 후보의 미룸과 공급 실패, 마지막
// 후보 우선, 종료의 closure(lease가 남으면 Busy).
using namespace rocks;
using namespace rocks::render;
using rocks::render::testing::RecordingDevice;
using rocks::render::testing::RegistryShaderSet;

namespace shader_set_slot_tests_file {

SystemAllocator& Allocator_() noexcept {
    static SystemAllocator allocator;
    return allocator;
}

// 엔진 셰이더 타입을 등록한 등록부와 그 라이브러리 byte 하나다.
struct SlotBench {
    SlotBench() noexcept : device(Allocator_()), registry(Allocator_()), library(Allocator_()) {
        ready = registry.Register(EngineShaderTypes()) &&
                registry.RegisterVertexInputTypes(EngineVertexInputTypes()) && registry.Freeze() &&
                RegistryShaderSet::BuildLibrary(Allocator_(), registry, library);
    }

    // 라이브러리로 연 새 집합이다(corrupt면 손상한 byte로 연다 — Failed).
    UniquePtr<ShaderSet> Open(bool corrupt = false) noexcept {
        auto made = UniquePtr<ShaderSet>::TryMake(
            Allocator_(), RegistryShaderSet::Desc(Allocator_(), device, registry));
        if (!made) {
            return UniquePtr<ShaderSet>();
        }

        UniquePtr<ShaderSet> set = Move(made.Value());
        Array<uint8> bytes(Allocator_());
        if (!bytes.TryAppend(library.View())) {
            return UniquePtr<ShaderSet>();
        }

        if (corrupt) {
            bytes[bytes.Size() / 2] ^= 0xFF;
            bytes[0] ^= 0xFF;
        }

        (void)set->OpenCooked(Move(bytes));
        return set;
    }

    GraphicsPipelineRequest Copy(ShaderSet& set) noexcept {
        auto copy = set.FindEntry(CopyTexturePixelType.hash);
        auto request = FullscreenPipelineRequest(set, copy ? copy.Value() : 0,
                                                 graphics::PixelFormat::RGBA8Unorm);
        return request ? request.Value() : GraphicsPipelineRequest{};
    }

    RecordingDevice device;
    shader::ShaderTypeRegistry registry;
    Array<uint8> library;
    bool ready = false;
};

// 공급자 요청을 붙잡았다가 시험이 끝내는 공급자다.
class HeldProvider final : public ShaderProvider {
public:
    Result<void> RequestLibrary(const ShaderProvisionRequest& request,
                                ShaderSet& set) noexcept override {
        (void)request;
        held = &set;
        return Result<void>::Success();
    }

    ShaderSet* held = nullptr;
};

TEST(ShaderSetSlot_SwapsAtFrameBoundaryAndKeepsTheCurrentOnFailure) {
    SlotBench bench;
    REQUIRE(bench.ready);
    PipelineCache cache(Allocator_(), bench.device);
    ShaderSetSlot slot(Allocator_());
    CHECK(slot.Install(UniquePtr<ShaderSet>()).Error() == ErrorCode::InvalidArgument);
    CHECK(slot.Install(bench.Open(true)).Error() == ErrorCode::DependencyFailed);
    CHECK(slot.Current() == nullptr && slot.Rejections() == 1);
    REQUIRE(slot.Install(bench.Open()));
    CHECK(slot.Install(bench.Open()).Error() == ErrorCode::Busy);
    ShaderSet* first = slot.Current();
    REQUIRE(first != nullptr);
    const uint64 firstEpoch = first->Epoch();
    REQUIRE(cache.FindOrCreate(bench.Copy(*first)));
    CHECK(cache.Size() == 1 && first->LeaseCount() == 1);

    // 검사에 실패한 후보는 거부되고 현재 집합이 그대로다.
    CHECK(slot.Propose(bench.Open(true)).Error() == ErrorCode::DependencyFailed);
    CHECK(slot.Rejections() == 2 && !slot.LastRejection().IsEmpty());
    CHECK(!slot.ApplyPending(&cache) && slot.Current() == first);

    // 진행 중 프레임이 옛 집합의 lease를 잡은 채 교체한다.
    REQUIRE(first->TryAcquireLease());
    // 마지막 후보가 이긴다.
    REQUIRE(slot.Propose(bench.Open()));
    REQUIRE(slot.Propose(bench.Open()));
    CHECK(slot.ApplyPending(&cache));
    ShaderSet* second = slot.Current();
    REQUIRE(second != nullptr && second != first);
    CHECK(second->Epoch() != firstEpoch && slot.Swaps() == 1);
    // 옛 집합: 캐시 항목 회수, Retire(새 lease 거부), 회수 대기.
    CHECK(cache.Size() == 0 && first->State() == ShaderSetState::Retired &&
          !first->TryAcquireLease());
    CHECK(slot.RetiredCount() == 1 && slot.CollectRetired() == 0);
    // 새 집합의 캐시 항목은 새 epoch로 만든다(미적중).
    const uint64 missesBefore = cache.Misses();
    REQUIRE(cache.FindOrCreate(bench.Copy(*second)));
    CHECK(cache.Misses() == missesBefore + 1);
    // 후보가 없으면 아무것도 하지 않는다.
    CHECK(!slot.ApplyPending(&cache));
    // 진행 중 프레임이 끝나 lease가 풀리면 옛 집합을 파괴한다.
    first->ReleaseLease();
    CHECK(slot.CollectRetired() == 1 && slot.RetiredCount() == 0);

    // 종료: lease가 남으면 Busy, 풀리면 성공한다.
    REQUIRE(second->TryAcquireLease());
    CHECK(slot.Shutdown(&cache).Error() == ErrorCode::Busy);
    CHECK(slot.Current() == nullptr && cache.Size() == 0 && slot.RetiredCount() == 1);
    second->ReleaseLease();
    CHECK(slot.Shutdown(&cache));
    CHECK(slot.RetiredCount() == 0);
    bench.device.CompleteAll();
    bench.device.ReclaimCompleted();
}

TEST(ShaderSetSlot_DefersWaitingCandidatesAndRejectsFailedProvision) {
    SlotBench bench;
    REQUIRE(bench.ready);
    ShaderSetSlot slot(Allocator_());
    REQUIRE(slot.Install(bench.Open()));
    ShaderSet* first = slot.Current();

    // 공급 대기 후보는 다음 경계로 미루고, 공급이 끝나면 교체한다.
    HeldProvider provider;
    auto waiting = UniquePtr<ShaderSet>::TryMake(
        Allocator_(), RegistryShaderSet::Desc(Allocator_(), bench.device, bench.registry));
    REQUIRE(waiting);
    REQUIRE(waiting.Value()->OpenFromProvider(&provider));
    REQUIRE(waiting.Value()->State() == ShaderSetState::WaitingForProvider);
    REQUIRE(slot.Propose(Move(waiting.Value())));
    CHECK(!slot.ApplyPending(nullptr) && slot.Current() == first);
    Array<uint8> bytes(Allocator_());
    REQUIRE(bytes.TryAppend(bench.library.View()));
    REQUIRE(provider.held != nullptr);
    provider.held->CompleteProvision(Result<void>::Success(), Move(bytes), StringView());
    CHECK(slot.ApplyPending(nullptr) && slot.Current() != first && slot.Swaps() == 1);
    CHECK(slot.CollectRetired() == 1);

    // 공급이 실패한 후보는 적용 때 버리고 현재 집합을 유지한다.
    HeldProvider failing;
    auto broken = UniquePtr<ShaderSet>::TryMake(
        Allocator_(), RegistryShaderSet::Desc(Allocator_(), bench.device, bench.registry));
    REQUIRE(broken);
    REQUIRE(broken.Value()->OpenFromProvider(&failing));
    REQUIRE(slot.Propose(Move(broken.Value())));
    REQUIRE(failing.held != nullptr);
    failing.held->CompleteProvision(Result<void>::Failure(ErrorCode::DependencyFailed),
                                    Array<uint8>(Allocator_()), StringView("compile failed"));
    ShaderSet* current = slot.Current();
    const uint64 rejections = slot.Rejections();
    CHECK(!slot.ApplyPending(nullptr) && slot.Current() == current);
    CHECK(slot.Rejections() == rejections + 1);
    CHECK(slot.Shutdown(nullptr));
    bench.device.CompleteAll();
    bench.device.ReclaimCompleted();
}

} // namespace shader_set_slot_tests_file
