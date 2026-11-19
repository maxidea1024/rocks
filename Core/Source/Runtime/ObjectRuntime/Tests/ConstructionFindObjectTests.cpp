#include <Core/Memory/SystemAllocator.h>
#include <Object/ConstructionContext.h>
#include <Object/TypeRegistry.h>
#include <Platform/Fatal.h>
#include <Platform/Thread.h>
#include <TestSupport/Test.h>

// ConstructionContext::FindObject 시험이다(Docs/Design/ObjectFoundations.ko.md 5.4절, 9절 Od2): 이
// 트랜잭션의 공개 전 객체와 공개된 객체를 객체 경로로 찾고(SCC import 해석), 다른 트랜잭션의 공개
// 전 객체는 outermost면 StaleHandle·경로 구성 요소면 NotFound다. 그 밖의 규칙·오류 순서는
// ObjectRegistry::FindObject와 같고 트랜잭션 상태 규칙을 먼저 본다. 같은 기준의
// ConstructionContext::AcquireType(Oe — import 대상의 타입 확인)과 한 단계 조회 FindChild도 본다.
using namespace rocks;
namespace construction_find_object_tests_file {

namespace {

constexpr TypeId FindNodeType{0x43464f4e4f444531ULL, 1};

struct FindNode final : Object {};

bool RegisterTypes(TypeRegistry& types) noexcept {
    RuntimeTypeOps ops = MakeRuntimeTypeOps<FindNode>();
    ops.completeFieldLayout = true;
    return static_cast<bool>(types.Register({FindNodeType, 1, {}, {}}, Move(ops)));
}

Result<ObjectHandle> NewNode(ObjectRegistry& registry, ObjectHandle outer, StringView name,
                             RootRef<Object>& root) noexcept {
    auto made = registry.NewObject<Object>(registry.OwnerAccess(), FindNodeType, outer, name);
    if (!made) {
        return Result<ObjectHandle>::Failure(made.Error());
    }

    root = Move(made.Value());
    return Result<ObjectHandle>::Success(root.Handle());
}

bool Finds(const Result<ObjectHandle>& found, ObjectHandle expected) noexcept {
    return found && found.Value() == expected;
}

ErrorCode ErrorOf(const Result<ObjectHandle>& found) noexcept {
    return found ? ErrorCode::None : found.Error();
}

// 다른 스레드에서 transaction.FindObject(outermost, path)를 부른 결과다.
ErrorCode FindOnWorker(ConstructionContext& transaction, ObjectHandle outermost,
                       StringView path) noexcept {
    struct Foreign {
        ConstructionContext* transaction;
        ObjectHandle outermost;
        StringView path;
        ErrorCode error = ErrorCode::None;
    } foreign{&transaction, outermost, path};
    platform::Thread thread;
    auto entry = [](void* context) noexcept {
        auto* state = static_cast<Foreign*>(context);
        state->error = ErrorOf(state->transaction->FindObject(state->outermost, state->path));
    };
    if (thread.Start(entry, &foreign) != platform::ThreadStatus::Success) {
        return ErrorCode::DependencyFailed;
    }

    if (thread.WaitForCompletion(10000) != platform::WaitResult::Signaled ||
        thread.Join() != platform::ThreadStatus::Success) {
        platform::Fatal("Construction find test worker failed to join");
    }

    return foreign.error;
}

// 다른 스레드에서 transaction.AcquireType(object)를 부른 결과다.
ErrorCode AcquireOnWorker(ConstructionContext& transaction, ObjectHandle object) noexcept {
    struct Foreign {
        ConstructionContext* transaction;
        ObjectHandle object;
        ErrorCode error = ErrorCode::None;
    } foreign{&transaction, object};
    platform::Thread thread;
    auto entry = [](void* context) noexcept {
        auto* state = static_cast<Foreign*>(context);
        auto type = state->transaction->AcquireType(state->object);
        state->error = type ? ErrorCode::None : type.Error();
    };
    if (thread.Start(entry, &foreign) != platform::ThreadStatus::Success) {
        return ErrorCode::DependencyFailed;
    }

    if (thread.WaitForCompletion(10000) != platform::WaitResult::Signaled ||
        thread.Join() != platform::ThreadStatus::Success) {
        platform::Fatal("Construction type test worker failed to join");
    }

    return foreign.error;
}

ErrorCode TypeError(const Result<TypeLease>& type) noexcept {
    return type ? ErrorCode::None : type.Error();
}

} // namespace

// 이 트랜잭션의 공개 전 outermost·대표 자식·경로 객체와 공개된 객체를 찾는다. 공개된 outermost
// 아래의 공개 전 자식도 찾는다. 다른 트랜잭션과 Registry 조회는 공개 전 객체를 보지 않는다. Commit
// 뒤에는 Registry 조회가 같은 객체를 찾는다.
TEST(ConstructionFindObject_SeesThisTransactionAndPublishedObjects) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry registry(allocator, types, {0x43464f01, 1});
        const auto access = registry.OwnerAccess();
        RootRef<Object> roots[3];
        // 공개된 L(대표 자식 아래 Shared).
        auto live = NewNode(registry, {}, {}, roots[0]);
        REQUIRE(live);
        auto liveAsset = NewNode(registry, live.Value(), {}, roots[1]);
        REQUIRE(liveAsset);
        auto shared = NewNode(registry, liveAsset.Value(), "Shared", roots[2]);
        REQUIRE(shared);
        {
            ConstructionContext transaction(registry, access);
            auto package = transaction.Create<FindNode>(FindNodeType);
            REQUIRE(package);
            CHECK(transaction.FindObject(package.Value(), "").Error() == ErrorCode::NotFound);
            auto asset = transaction.Create<FindNode>(FindNodeType, package.Value());
            REQUIRE(asset);
            auto body = transaction.Create<FindNode>(FindNodeType, asset.Value(), "Body");
            REQUIRE(body);
            auto socket = transaction.Create<FindNode>(FindNodeType, body.Value(), "Socket");
            REQUIRE(socket);
            auto added = transaction.Create<FindNode>(FindNodeType, liveAsset.Value(), "Added");
            REQUIRE(added);
            // 빈 경로 = 대표 자식, 대소문자·구성 요소, 공개된 객체, 공개된 outermost 아래 공개 전
            // 자식.
            CHECK(Finds(transaction.FindObject(package.Value(), ""), asset.Value()));
            CHECK(Finds(transaction.FindObject(package.Value(), "body.SOCKET"), socket.Value()));
            CHECK(Finds(transaction.FindObject(live.Value(), "Shared"), shared.Value()));
            CHECK(Finds(transaction.FindObject(live.Value(), "added"), added.Value()));
            CHECK(transaction.FindObject(package.Value(), "Body.Missing").Error() ==
                  ErrorCode::NotFound);
            // Registry 조회는 공개 전 객체를 보지 않는다(outermost면 StaleHandle, 자식이면
            // NotFound).
            CHECK(registry.FindObject(access, package.Value(), "Body").Error() ==
                  ErrorCode::StaleHandle);
            CHECK(registry.FindObject(access, live.Value(), "Added").Error() ==
                  ErrorCode::NotFound);
            {
                // 다른 트랜잭션: 이 트랜잭션의 공개 전 outermost는 StaleHandle, 공개된 outermost
                // 아래 공개 전 자식은 NotFound다(이름은 점유한다).
                ConstructionContext other(registry, access);
                CHECK(other.FindObject(package.Value(), "Body").Error() == ErrorCode::StaleHandle);
                CHECK(other.FindObject(live.Value(), "Added").Error() == ErrorCode::NotFound);
                auto taken = registry.IsNameTaken(access, liveAsset.Value(), "Added");
                CHECK(taken && taken.Value());
                CHECK(Finds(other.FindObject(live.Value(), "Shared"), shared.Value()));
            }

            const ObjectHandle publish[] = {package.Value()};
            RootRef<Object> held[1];
            REQUIRE(transaction.Commit(publish, held));
            CHECK(
                Finds(registry.FindObject(access, package.Value(), "Body.Socket"), socket.Value()));
            // 끝난 트랜잭션은 트랜잭션 상태 규칙(InvalidArgument)이다.
            CHECK(transaction.FindObject(package.Value(), "Body").Error() ==
                  ErrorCode::InvalidArgument);
            held[0].Reset();
        }

        for (auto& root : roots) {
            root.Reset();
        }

        REQUIRE(registry.Collect(access));
        CHECK(registry.Stats().liveObjects == 0);
        REQUIRE(registry.Shutdown(access));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 오류 순서: 트랜잭션 상태(고정된 시작 오류·끝난 트랜잭션) → 접근(다른 스레드 WrongThread) →
// outermost(null StaleHandle, Outer가 있으면 InvalidArgument) → 경로 형식(빈 구성 요소·4 KiB 초과·
// 이름 형식 InvalidArgument) — ObjectRegistry::FindObject와 같다. 두 규칙을 함께 어기면 앞 단계의
// 오류다(끝난 트랜잭션을 다른 스레드에서 부르면 InvalidArgument, null outermost와 틀린 경로는
// StaleHandle).
TEST(ConstructionFindObject_ErrorOrderMatchesRegistryFindObject) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry registry(allocator, types, {0x43464f02, 1});
        const auto access = registry.OwnerAccess();
        {
            ConstructionContext mismatched(registry, {{0x43464f02, 99}});
            CHECK(mismatched.FindObject({}, "").Error() == ErrorCode::WrongContext);
        }

        {
            ConstructionContext transaction(registry, access);
            auto package = transaction.Create<FindNode>(FindNodeType);
            REQUIRE(package);
            auto asset = transaction.Create<FindNode>(FindNodeType, package.Value());
            REQUIRE(asset);
            CHECK(transaction.FindObject({}, "").Error() == ErrorCode::StaleHandle);
            CHECK(transaction.FindObject(asset.Value(), "").Error() == ErrorCode::InvalidArgument);
            CHECK(transaction.FindObject(package.Value(), "a..b").Error() ==
                  ErrorCode::InvalidArgument);
            CHECK(transaction.FindObject(package.Value(), "a/b").Error() ==
                  ErrorCode::InvalidArgument);
            char longPath[MaxObjectPathBytes + 1];
            for (char& character : longPath) {
                character = 'p';
            }

            CHECK(transaction.FindObject(package.Value(), StringView(longPath, sizeof(longPath)))
                      .Error() == ErrorCode::InvalidArgument);
            // 다른 스레드에서 부르면 WrongThread다(트랜잭션 상태는 열려 있다). 경로 형식이 틀려도
            // 접근 검사가 먼저다. null outermost와 틀린 경로를 함께 주면 outermost(StaleHandle)가
            // 먼저다.
            CHECK(FindOnWorker(transaction, package.Value(), "") == ErrorCode::WrongThread);
            CHECK(FindOnWorker(transaction, package.Value(), "a..b") == ErrorCode::WrongThread);
            CHECK(transaction.FindObject({}, "a..b").Error() == ErrorCode::StaleHandle);
            CHECK(transaction.FindObject(asset.Value(), "a/b").Error() ==
                  ErrorCode::InvalidArgument);
            CHECK(Finds(transaction.FindObject(package.Value(), ""), asset.Value()));
            // 끝난 트랜잭션은 다른 스레드에서 불러도 트랜잭션 상태(InvalidArgument)가 먼저다.
            const ObjectHandle publish[] = {package.Value()};
            RootRef<Object> held[1];
            REQUIRE(transaction.Commit(publish, held));
            CHECK(FindOnWorker(transaction, package.Value(), "") == ErrorCode::InvalidArgument);
            CHECK(transaction.FindObject({}, "a..b").Error() == ErrorCode::InvalidArgument);
            held[0].Reset();
        }

        REQUIRE(registry.Collect(access));

        CHECK(registry.Stats().reservedObjects == 0);
        REQUIRE(registry.Shutdown(access));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// AcquireType: 생존 객체와 이 트랜잭션의 공개 전 객체는 실제 타입 lease를 돌려주고, null·파괴
// 요청·다른 트랜잭션의 공개 전 객체는 StaleHandle이다. 트랜잭션 상태 규칙(시작 오류·고정된
// 오류·끝난 트랜잭션)이 먼저이고, 열린 트랜잭션을 다른 스레드에서 부르면 WrongThread다.
TEST(ConstructionAcquireType_SeesLiveAndThisTransactionObjects) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry registry(allocator, types, {0x43464f03, 1});
        const auto access = registry.OwnerAccess();
        RootRef<Object> roots[2];
        auto live = NewNode(registry, {}, {}, roots[0]);
        auto doomed = NewNode(registry, {}, {}, roots[1]);
        REQUIRE(live && doomed);
        REQUIRE(registry.RequestDestroy(access, doomed.Value()));
        {
            ConstructionContext mismatched(registry, {{0x43464f03, 99}});
            CHECK(TypeError(mismatched.AcquireType(live.Value())) == ErrorCode::WrongContext);
        }

        {
            ConstructionContext transaction(registry, access);
            auto staged = transaction.Create<FindNode>(FindNodeType);
            REQUIRE(staged);
            auto liveType = transaction.AcquireType(live.Value());
            auto stagedType = transaction.AcquireType(staged.Value());
            REQUIRE(liveType && stagedType);
            CHECK(liveType.Value().Id() == FindNodeType && stagedType.Value().Id() == FindNodeType);
            CHECK(TypeError(transaction.AcquireType({})) == ErrorCode::StaleHandle);
            CHECK(TypeError(transaction.AcquireType(doomed.Value())) == ErrorCode::StaleHandle);
            {
                ConstructionContext other(registry, access);
                CHECK(TypeError(other.AcquireType(staged.Value())) == ErrorCode::StaleHandle);
                CHECK(TypeError(other.AcquireType(live.Value())) == ErrorCode::None);
            }

            CHECK(AcquireOnWorker(transaction, live.Value()) == ErrorCode::WrongThread);
            REQUIRE(transaction.Fail(ErrorCode::DependencyFailed));
            CHECK(TypeError(transaction.AcquireType(live.Value())) == ErrorCode::DependencyFailed);
            transaction.Rollback();
        }

        {
            ConstructionContext transaction(registry, access);
            auto staged = transaction.Create<FindNode>(FindNodeType);
            REQUIRE(staged);
            const ObjectHandle publish[] = {staged.Value()};
            RootRef<Object> held[1];
            REQUIRE(transaction.Commit(publish, held));
            CHECK(TypeError(transaction.AcquireType(staged.Value())) == ErrorCode::InvalidArgument);
            CHECK(TypeError(registry.AcquireType(access, staged.Value())) == ErrorCode::None);
            held[0].Reset();
        }

        for (auto& root : roots) {
            root.Reset();
        }

        REQUIRE(registry.Collect(access));
        CHECK(registry.Stats().liveObjects == 0);
        REQUIRE(registry.Shutdown(access));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// FindChild(outer, name): FindObject의 구성 요소 한 단계다. outermost가 아닌 outer(공개된 객체·이
// 트랜잭션의 공개 전 객체) 아래 이름 하나를 대소문자 무시로 찾고, 빈 이름은 대표 자식이다. 다른
// 트랜잭션의 공개 전 객체는 outer면 StaleHandle, 자식이면 NotFound다. 오류 순서는 트랜잭션 상태 →
// 접근(WrongThread) → outer(null StaleHandle) → 이름 형식(InvalidArgument) → NotFound다.
TEST(ConstructionFindChild_FindsOneComponentUnderAnyVisibleOuter) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry registry(allocator, types, {0x43464f04, 1});
        const auto access = registry.OwnerAccess();
        RootRef<Object> roots[3];
        auto live = NewNode(registry, {}, {}, roots[0]);
        REQUIRE(live);
        auto liveAsset = NewNode(registry, live.Value(), {}, roots[1]);
        REQUIRE(liveAsset);
        auto shared = NewNode(registry, liveAsset.Value(), "Shared", roots[2]);
        REQUIRE(shared);
        {
            ConstructionContext mismatched(registry, {{0x43464f04, 99}});
            CHECK(mismatched.FindChild(live.Value(), "").Error() == ErrorCode::WrongContext);
        }

        {
            ConstructionContext transaction(registry, access);
            auto package = transaction.Create<FindNode>(FindNodeType);
            REQUIRE(package);
            auto asset = transaction.Create<FindNode>(FindNodeType, package.Value());
            REQUIRE(asset);
            auto body = transaction.Create<FindNode>(FindNodeType, asset.Value(), "Body");
            REQUIRE(body);
            auto socket = transaction.Create<FindNode>(FindNodeType, body.Value(), "Socket");
            REQUIRE(socket);
            auto added = transaction.Create<FindNode>(FindNodeType, shared.Value(), "Added");
            REQUIRE(added);
            CHECK(Finds(transaction.FindChild(package.Value(), ""), asset.Value()));
            CHECK(Finds(transaction.FindChild(body.Value(), "socket"), socket.Value()));
            CHECK(Finds(transaction.FindChild(liveAsset.Value(), "SHARED"), shared.Value()));
            CHECK(Finds(transaction.FindChild(shared.Value(), "Added"), added.Value()));
            CHECK(Finds(transaction.FindChild(live.Value(), ""), liveAsset.Value()));
            CHECK(transaction.FindChild(body.Value(), "Missing").Error() == ErrorCode::NotFound);
            CHECK(transaction.FindChild(socket.Value(), "").Error() == ErrorCode::NotFound);
            CHECK(transaction.FindChild({}, "Body").Error() == ErrorCode::StaleHandle);
            CHECK(transaction.FindChild(body.Value(), "a.b").Error() == ErrorCode::InvalidArgument);
            CHECK(transaction.FindChild({}, "a.b").Error() == ErrorCode::StaleHandle);
            {
                ConstructionContext other(registry, access);
                CHECK(other.FindChild(body.Value(), "Socket").Error() == ErrorCode::StaleHandle);
                CHECK(other.FindChild(shared.Value(), "Added").Error() == ErrorCode::NotFound);
                CHECK(Finds(other.FindChild(liveAsset.Value(), "Shared"), shared.Value()));
            }

            struct Foreign {
                ConstructionContext* transaction;
                ObjectHandle outer;
                ErrorCode error = ErrorCode::None;
            } foreign{&transaction, body.Value()};
            {
                platform::Thread thread;
                auto entry = [](void* context) noexcept {
                    auto* state = static_cast<Foreign*>(context);
                    state->error = ErrorOf(state->transaction->FindChild(state->outer, "a.b"));
                };
                REQUIRE(thread.Start(entry, &foreign) == platform::ThreadStatus::Success);
                REQUIRE(thread.Join() == platform::ThreadStatus::Success);
            }

            CHECK(foreign.error == ErrorCode::WrongThread);
            const ObjectHandle publish[] = {package.Value()};
            RootRef<Object> held[1];
            REQUIRE(transaction.Commit(publish, held));
            CHECK(transaction.FindChild(body.Value(), "Socket").Error() ==
                  ErrorCode::InvalidArgument);
            CHECK(Finds(registry.FindChild(access, body.Value(), "Socket"), socket.Value()));
            held[0].Reset();
        }

        for (auto& root : roots) {
            root.Reset();
        }

        REQUIRE(registry.Collect(access));
        CHECK(registry.Stats().liveObjects == 0);
        REQUIRE(registry.Shutdown(access));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}
} // namespace construction_find_object_tests_file
