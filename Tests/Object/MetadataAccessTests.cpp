#include <Tests/Support/Test.h>
#include <Core/Memory/SystemAllocator.h>
#include <Object/ObjectRegistry.h>
#include <Platform/Thread.h>

using namespace rocks;

namespace {

constexpr Guid QueryType{0x5155455259ULL, 1};
ObjectRegistry* TraceRegistry = nullptr;
ErrorCode TraceQueryResult = ErrorCode::None;

struct QueryObject final : Object {
    void Trace(TraceVisitor&) const noexcept {
        if (TraceRegistry != nullptr) {
            TraceQueryResult =
                TraceRegistry->AcquireRegisteredType(TraceRegistry->OwnerAccess(), QueryType)
                    .Error();
        }
    }
};

struct ThreadQuery {
    ObjectRegistry* registry;
    ObjectHandle handle;
    ErrorCode byId = ErrorCode::None;
    ErrorCode byObject = ErrorCode::None;
};

void QueryFromOtherThread(void* context) noexcept {
    auto& query = *static_cast<ThreadQuery*>(context);
    auto access = query.registry->OwnerAccess();
    query.byId = query.registry->AcquireRegisteredType(access, QueryType).Error();
    query.byObject = query.registry->AcquireType(access, query.handle).Error();
}

} // namespace

TEST(ObjectMetadataQueriesCheckAccessAndRetainOnlyTypeLifetime) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        auto registered = types.Register({QueryType, 1, {}, {}}, MakeRuntimeTypeOps<QueryObject>());
        REQUIRE(registered);
        ObjectRegistry objects(allocator, types, {923, 1});
        const auto access = objects.OwnerAccess();
        auto root = objects.NewObject<QueryObject>(access, QueryType);
        REQUIRE(root);
        auto byId = objects.AcquireRegisteredType(access, QueryType);
        auto byObject = objects.AcquireType(access, root.Value().Handle());
        REQUIRE(byId && byObject);
        CHECK(byId.Value().Schema().id == byObject.Value().Schema().id);
        CHECK(objects.Stats().roots == 1);
        CHECK(objects.Stats().borrows == 0);
        CHECK(objects.AcquireRegisteredType({{923, 2}}, QueryType).Error() ==
              ErrorCode::WrongContext);
        CHECK(objects.AcquireType({{923, 2}}, root.Value().Handle()).Error() ==
              ErrorCode::WrongContext);
        CHECK(objects.AcquireRegisteredType(access, {923, 99}).Error() == ErrorCode::NotFound);

        ThreadQuery query{&objects, root.Value().Handle()};
        platform::Thread thread;
        REQUIRE(thread.Start(&QueryFromOtherThread, &query) == platform::ThreadStatus::Success);
        REQUIRE(thread.Join() == platform::ThreadStatus::Success);
        CHECK(query.byId == ErrorCode::WrongThread);
        CHECK(query.byObject == ErrorCode::WrongThread);

        // Trace 재진입은 등록 메타데이터 읽기여도 collector 처리 문맥을 우회하지 않는다.
        TraceRegistry = &objects;
        TraceQueryResult = ErrorCode::None;
        auto collected = objects.Collect(access);
        TraceRegistry = nullptr;
        REQUIRE(collected);
        CHECK(TraceQueryResult == ErrorCode::Busy);

        REQUIRE(objects.RequestDestroy(access, root.Value().Handle()));
        CHECK(objects.AcquireType(access, root.Value().Handle()).Error() == ErrorCode::Destroyed);
        root.Value().Reset();
        REQUIRE(objects.Collect(access));
        CHECK(objects.Stats().liveObjects == 0);
        CHECK(byObject.Value().Schema().id == QueryType);
        CHECK(types.Unregister(registered.Value()).Error() == ErrorCode::Busy);
        REQUIRE(objects.Shutdown(access));
        CHECK(objects.AcquireRegisteredType(access, QueryType).Error() == ErrorCode::ShuttingDown);
        byId.Value().Reset();
        byObject.Value().Reset();
        REQUIRE(types.Unregister(registered.Value()));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}
