#include <Core/Memory/SystemAllocator.h>
#include <Object/ObjectRegistry.h>
#include <Platform/Fatal.h>
#include <Platform/Thread.h>
#include <Tests/Support/Test.h>

using namespace rocks;

namespace {
constexpr TypeId ContextFixtureType{0x6175746f636f6e74ull, 1};

struct ContextFixture final : Object {};

struct WorkerProbe {
    ObjectAccessContext wrong;
    Borrow<ContextFixture>* borrowed;
    ErrorCode borrowError = ErrorCode::None;
};

void InspectFromWorker(void* raw) noexcept {
    auto& probe = *static_cast<WorkerProbe*>(raw);
    ObjectAccessScope wrongScope(probe.wrong);
    probe.borrowError = probe.borrowed->Get().Error();
}
} // namespace

TEST(ActiveObjectScopeRejectsCrossWorldBorrowWithoutChangingLifetime) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(
            types.Register({ContextFixtureType, 1, {}, {}}, MakeRuntimeTypeOps<ContextFixture>()));
        ObjectRegistry worldA(allocator, types, {400, 1});
        ObjectRegistry worldB(allocator, types, {400, 2});
        auto accessA = worldA.OwnerAccess();
        auto accessB = worldB.OwnerAccess();
        auto rootA = worldA.NewObject<ContextFixture>(accessA, ContextFixtureType);
        auto rootB = worldB.NewObject<ContextFixture>(accessB, ContextFixtureType);
        REQUIRE(rootA && rootB);
        ObjectHandle handleA = rootA.Value().Handle();
        auto borrowA = worldA.Resolve<ContextFixture>(accessA, handleA);
        auto borrowB = worldB.Resolve<ContextFixture>(accessB, rootB.Value().Handle());
        REQUIRE(borrowA && borrowB);

        // 실행 문맥이 없으면 기존 Borrow 조회는 그대로 성공한다.
        CHECK(borrowA.Value().Get());
        CHECK(borrowB.Value().Get());
        {
            ObjectAccessScope scopeB(accessB);
            CHECK(borrowA.Value().Get().Error() == ErrorCode::WrongContext);
            CHECK(borrowB.Value().Get());
            {
                ObjectAccessScope scopeA(accessA);
                CHECK(borrowA.Value().Get());
                CHECK(borrowB.Value().Get().Error() == ErrorCode::WrongContext);
            }
            CHECK(borrowA.Value().Get().Error() == ErrorCode::WrongContext);
            CHECK(borrowB.Value().Get());
            // 다른 World scope에서도 소유권 반환은 항상 가능해야 한다.
            borrowA.Value().Reset();
        }
        auto retry = worldA.Resolve<ContextFixture>(accessA, handleA);
        REQUIRE(retry);
        CHECK(retry.Value().Get());

        // 활성 문맥이 잘못되어도 다른 스레드 접근은 WrongThread가 우선이다.
        WorkerProbe probe{accessB, &retry.Value()};
        platform::Thread thread;
        if (thread.Start(InspectFromWorker, &probe) != platform::ThreadStatus::Success ||
            thread.WaitForCompletion(10000) != platform::WaitResult::Signaled ||
            thread.Join() != platform::ThreadStatus::Success) {
            platform::Fatal("Context test worker did not finish");
        }
        CHECK(probe.borrowError == ErrorCode::WrongThread);
        retry.Value().Reset();
        borrowB.Value().Reset();
        rootA.Value().Reset();
        rootB.Value().Reset();
        REQUIRE(worldA.Shutdown(accessA));
        REQUIRE(worldB.Shutdown(accessB));
        REQUIRE(types.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
    CHECK(allocator.Stats().liveBytes == 0);
}
