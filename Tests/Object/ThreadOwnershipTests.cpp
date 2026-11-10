#include "Tests/Support/Test.h"

#include <Core/Memory/SystemAllocator.h>
#include <Object/ConstructionContext.h>

#include <Platform/Thread.h>

namespace {

using namespace rocks;

struct ThreadProbe final : Object {};

struct ForeignAccess {
    ObjectRegistry* registry;
    ObjectAccessContext access;
    ObjectHandle handle;
    RootRef<ThreadProbe>* root;
    Borrow<ThreadProbe>* borrow;
    BackendLease* backend;
    bool pinRejected = false;
    bool resolveRejected = false;
    bool borrowRejected = false;
    bool collectionRejected = false;
};

void VisitFromForeignThread(ForeignAccess& state) noexcept {
    auto pin = state.registry->TryPin<ThreadProbe>(state.access, state.handle);
    state.pinRejected = !pin.IsOk() && pin.Error() == ErrorCode::WrongThread;
    auto resolve = state.registry->Resolve<ThreadProbe>(state.access, state.handle);
    state.resolveRejected = !resolve.IsOk() && resolve.Error() == ErrorCode::WrongThread;
    auto borrowed = state.borrow->Get();
    state.borrowRejected = !borrowed.IsOk() && borrowed.Error() == ErrorCode::WrongThread;
    auto collected = state.registry->Collect(state.access);
    state.collectionRejected = !collected.IsOk() && collected.Error() == ErrorCode::WrongThread;
    state.root->Reset();
    state.backend->Reset();
}

void ThreadEntry(void* context) noexcept {
    VisitFromForeignThread(*static_cast<ForeignAccess*>(context));
}

bool RunForeignThread(ForeignAccess& state) noexcept {
    platform::Thread thread;
    if (thread.Start(ThreadEntry, &state) != platform::ThreadStatus::Success) {
        return false;
    }
    // 실패한 join을 성공처럼 취급하거나 진행 중 worker의 stack context를 먼저 파괴하지 않는다.
    if (thread.WaitForCompletion(10000) != platform::WaitResult::Signaled ||
        thread.Join() != platform::ThreadStatus::Success) {
        platform::Fatal("Object owner test worker failed to join");
    }
    return true;
}

} // namespace

TEST(ObjectOwnerThreadChecksDoNotForbidForeignLeaseRelease) {
    SystemAllocator allocator;

    {
        TypeRegistry types(allocator);
        constexpr Guid type{0x544852454144ULL, 1};
        auto registered =
            types.Register(TypeSchema{type, 1, {}, {}}, MakeRuntimeTypeOps<ThreadProbe>());
        REQUIRE(registered.IsOk());
        ObjectRegistry registry(allocator, types, Guid{121, 1});
        auto access = registry.OwnerAccess();
        auto root = registry.NewObject<ThreadProbe>(access, type);
        REQUIRE(root.IsOk());
        const ObjectHandle handle = root.Value().Handle();
        auto borrowed = registry.Resolve<ThreadProbe>(access, handle);
        auto backend = registry.AcquireBackendLease(access, handle);
        REQUIRE(borrowed.IsOk());
        REQUIRE(backend.IsOk());
        ForeignAccess state{&registry,         access,          handle, &root.Value(),
                            &borrowed.Value(), &backend.Value()};

        REQUIRE(RunForeignThread(state));
        CHECK(state.pinRejected);
        CHECK(state.resolveRejected);
        CHECK(state.borrowRejected);
        CHECK(state.collectionRejected);
        CHECK(!root.Value().IsValid());
        CHECK(registry.Stats().roots == 0);
        CHECK(registry.Stats().backendHolds == 0);
        CHECK(borrowed.Value().Get().IsOk());
        borrowed.Value().Reset();
        REQUIRE(registry.Collect(access).IsOk());
        CHECK(registry.Stats().liveObjects == 0);
        REQUIRE(registry.Shutdown(access).IsOk());
        REQUIRE(types.Unregister(registered.Value()).IsOk());
        REQUIRE(types.Shutdown().IsOk());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}
