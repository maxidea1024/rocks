#include <Graphics/DeviceOwnership.h>
#include <Platform/Thread.h>
#include <TestSupport/Test.h>

using namespace rocks;
namespace device_ownership_tests_file {

using namespace rocks::graphics;

namespace {

// 다른 스레드에서 소유권을 다루는 시험 상태다. 각 단계는 이벤트로 차례를 정한다.
struct OtherThread {
    DeviceOwnership* ownership = nullptr;
    platform::ManualResetEvent took;
    platform::ManualResetEvent release;
    ErrorCode takeWhileOwned = ErrorCode::None;
    ErrorCode check = ErrorCode::None;
    bool acquired = false;
    bool released = false;

    static void Run(void* raw) noexcept {
        auto* self = static_cast<OtherThread*>(raw);
        self->takeWhileOwned = self->ownership->Acquire().Error();
        self->took.Signal();
        self->release.Wait();
        self->acquired = self->ownership->Acquire().IsOk();
        auto checked = self->ownership->Check();
        self->check = checked ? ErrorCode::None : checked.Error();
        self->released = self->ownership->Release().IsOk();
    }
};

} // namespace

TEST(DeviceOwnership_ReleaseAcquireAndChecks) {
    DeviceOwnership ownership;
    CHECK(ownership.IsOwner() && ownership.HasOwner());
    CHECK(ownership.Check());
    // 이미 소유자인 스레드의 Acquire는 성공이다.
    CHECK(ownership.Acquire());

    OtherThread other;
    other.ownership = &ownership;
    platform::Thread thread;
    REQUIRE(thread.Start(&OtherThread::Run, &other) == platform::ThreadStatus::Success);
    REQUIRE(other.took.Wait() == platform::WaitResult::Signaled);
    // 이 스레드가 가진 동안 다른 스레드는 잡을 수 없다.
    CHECK(other.takeWhileOwned == ErrorCode::Busy);

    REQUIRE(ownership.Release());
    CHECK(!ownership.HasOwner() && !ownership.IsOwner());
    CHECK(ownership.Release().Error() == ErrorCode::WrongThread);
    if constexpr (!IsReleaseBuild) {
        const uint64 before = ownership.Rejected();
        CHECK(ownership.Check().Error() == ErrorCode::WrongThread);
        CHECK(ownership.Rejected() == before + 1);
    } else {
        CHECK(ownership.Check());
    }

    other.release.Signal();
    REQUIRE(thread.Join() == platform::ThreadStatus::Success);
    CHECK(other.acquired && other.check == ErrorCode::None && other.released);
    CHECK(ownership.Acquire());
    CHECK(ownership.IsOwner());
}
} // namespace device_ownership_tests_file
