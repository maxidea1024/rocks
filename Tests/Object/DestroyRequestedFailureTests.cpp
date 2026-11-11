#include <Core/Memory/SystemAllocator.h>
#include <Object/ObjectRegistry.h>
#include <Platform/Fatal.h>
#include <Platform/Thread.h>
#include <Tests/Support/HostFailure.h>
#include <Tests/Support/Test.h>

using namespace rocks;

namespace {
constexpr TypeId StatusProbeType{0x7374617475737465ull, 1};

struct StatusProbe final : Object {};

// 실패가 출력 인자의 기존 값을 어느 쪽도 바꾸지 않는지 확인한다.
void CheckStatusFailure(const StatusProbe& object, ErrorCode expected) noexcept {
    bool value = false;
    CHECK(object.TryIsDestroyRequested(value).Error() == expected);
    CHECK(!value);
    value = true;
    CHECK(object.TryIsDestroyRequested(value).Error() == expected);
    CHECK(value);
}

struct ForeignStatus {
    const StatusProbe* object;
    ObjectAccessContext wrongContext;
    ErrorCode error = ErrorCode::None;
    bool value = true;
};

void QueryStatusFromForeignThread(void* raw) noexcept {
    auto& query = *static_cast<ForeignStatus*>(raw);
    ObjectAccessScope scope(query.wrongContext);
    query.error = query.object->TryIsDestroyRequested(query.value).Error();
}
} // namespace

TEST(DestroyRequestedTryPreservesErrorsAndOutput) {
    SystemAllocator allocator;
    {
        // 아직 Registry에 연결되지 않은 객체는 실패한다.
        StatusProbe unattached;
        CheckStatusFailure(unattached, ErrorCode::StaleHandle);

        TypeRegistry types(allocator);
        REQUIRE(types.Register({StatusProbeType, 1, {}, {}}, MakeRuntimeTypeOps<StatusProbe>()));
        ObjectRegistry registry(allocator, types, {510, 1});
        ObjectRegistry strict(allocator, types, {510, 2}, {8, 0xffffffffu, true});
        const auto access = registry.OwnerAccess();
        const auto strictAccess = strict.OwnerAccess();
        auto root = registry.NewObject<StatusProbe>(access, StatusProbeType);
        auto strictRoot = strict.NewObject<StatusProbe>(strictAccess, StatusProbeType);
        REQUIRE(root && strictRoot);
        auto borrow = registry.Resolve<StatusProbe>(access, root.Value().Handle());
        auto strictBorrow = strict.Resolve<StatusProbe>(strictAccess, strictRoot.Value().Handle());
        REQUIRE(borrow && strictBorrow);
        auto object = borrow.Value().Get();
        auto strictObject = strictBorrow.Value().Get();
        REQUIRE(object && strictObject);

        bool value = true;
        REQUIRE(object.Value()->TryIsDestroyRequested(value));
        CHECK(!value);
        CHECK(!object.Value()->IsDestroyRequested());
        CheckStatusFailure(*strictObject.Value(), ErrorCode::WrongContext);
        {
            ObjectAccessScope wrong(strictAccess);
            CheckStatusFailure(*object.Value(), ErrorCode::WrongContext);
            value = true;
            REQUIRE(strictObject.Value()->TryIsDestroyRequested(value));
            CHECK(!value);
        }
        ForeignStatus foreign{object.Value(), strictAccess};
        platform::Thread thread;
        if (thread.Start(QueryStatusFromForeignThread, &foreign) !=
                platform::ThreadStatus::Success ||
            thread.WaitForCompletion(10000) != platform::WaitResult::Signaled ||
            thread.Join() != platform::ThreadStatus::Success) {
            platform::Fatal("Status test worker did not finish");
        }
        CHECK(foreign.error == ErrorCode::WrongThread);
        CHECK(foreign.value);

        // root가 저장소를 유지하는 테스트에서만 Borrow 반환 후 포인터를 검사한다.
        StatusProbe* stillAllocated = object.Value();
        borrow.Value().Reset();
        CheckStatusFailure(*stillAllocated, ErrorCode::InvalidArgument);
        borrow = registry.Resolve<StatusProbe>(access, root.Value().Handle());
        REQUIRE(borrow);
        object = borrow.Value().Get();
        REQUIRE(object);
        REQUIRE(object.Value()->Destroy());
        CHECK(object.Value()->IsDestroyRequested());
        value = false;
        REQUIRE(object.Value()->TryIsDestroyRequested(value));
        CHECK(value);
        REQUIRE(object.Value()->Destroy());
        CHECK(object.Value()->IsDestroyRequested());

        // 미반환 Borrow 때문에 shutdown은 Busy지만 이후 상태 조회는 ShuttingDown이다.
        CHECK(registry.Shutdown(access).Error() == ErrorCode::Busy);
        CheckStatusFailure(*object.Value(), ErrorCode::ShuttingDown);
        borrow.Value().Reset();
        strictBorrow.Value().Reset();
        root.Value().Reset();
        strictRoot.Value().Reset();
        REQUIRE(registry.Shutdown(access));
        REQUIRE(strict.Shutdown(strictAccess));
        REQUIRE(types.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
    CHECK(allocator.Stats().liveBytes == 0);
}

namespace rocks::test {
void RegisterDestroyRequestedNegativeControl(int (*function)(bool) noexcept) noexcept;

// 전용 child process에서 실행한다. Fatal이 없으면 성공 코드로 돌아가 테스트가 실패한다.
int RunDestroyRequestedNegativeControl(bool wrongContext) noexcept {
    PrepareFatalTestProcess();
    if (!wrongContext) {
        StatusProbe unattached;
        (void)unattached.IsDestroyRequested();
        return 0;
    }
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    if (!types.Register({StatusProbeType, 1, {}, {}}, MakeRuntimeTypeOps<StatusProbe>())) {
        return 0;
    }
    ObjectRegistry registry(allocator, types, {511, 1});
    const auto access = registry.OwnerAccess();
    auto root = registry.NewObject<StatusProbe>(access, StatusProbeType);
    if (!root) {
        return 0;
    }
    auto borrow = registry.Resolve<StatusProbe>(access, root.Value().Handle());
    if (!borrow) {
        return 0;
    }
    auto object = borrow.Value().Get();
    if (!object) {
        return 0;
    }
    ObjectAccessScope wrong({{511, 2}});
    (void)object.Value()->IsDestroyRequested();
    return 0;
}

namespace {
struct DestroyRequestedNegativeRegistration {
    DestroyRequestedNegativeRegistration() noexcept {
        RegisterDestroyRequestedNegativeControl(&RunDestroyRequestedNegativeControl);
    }
};
DestroyRequestedNegativeRegistration destroyRequestedNegativeRegistration;
} // namespace
} // namespace rocks::test
