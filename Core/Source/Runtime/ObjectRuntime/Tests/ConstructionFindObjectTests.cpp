#include <Core/Memory/SystemAllocator.h>
#include <Object/ConstructionContext.h>
#include <Object/TypeRegistry.h>
#include <Platform/Fatal.h>
#include <Platform/Thread.h>
#include <TestSupport/Test.h>

// ConstructionContext::FindObject 시험이다(Docs/Design/ObjectFoundations.ko.md 5.4절, 9절 Od2): 이
// 트랜잭션의 공개 전 객체와 공개된 객체를 객체 경로로 찾고(SCC import 해석), 다른 트랜잭션의 공개
// 전 객체는 outermost면 StaleHandle·경로 구성 요소면 NotFound다. 그 밖의 규칙·오류 순서는
// ObjectRegistry::FindObject와 같고 트랜잭션 상태 규칙을 먼저 본다.
using namespace rocks;

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
