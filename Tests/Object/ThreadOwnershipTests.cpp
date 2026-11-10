#include "Tests/Support/Test.h"

#include <Core/Memory/SystemAllocator.h>
#include <Object/ConstructionContext.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <pthread.h>
#endif

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

#if defined(_WIN32)
DWORD WINAPI ThreadEntry(void* context) {
    VisitFromForeignThread(*static_cast<ForeignAccess*>(context));
    return 0;
}

bool RunForeignThread(ForeignAccess& state) noexcept {
    HANDLE thread = CreateThread(nullptr, 0, ThreadEntry, &state, 0, nullptr);

    if (thread == nullptr) {
        return false;
    }

    const DWORD result = WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    return result == WAIT_OBJECT_0;
}
#else
void* ThreadEntry(void* context) {
    VisitFromForeignThread(*static_cast<ForeignAccess*>(context));
    return nullptr;
}

bool RunForeignThread(ForeignAccess& state) noexcept {
    pthread_t thread{};

    if (pthread_create(&thread, nullptr, ThreadEntry, &state) != 0) {
        return false;
    }

    return pthread_join(thread, nullptr) == 0;
}
#endif

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
