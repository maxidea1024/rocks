#include <Core/Containers/HashMap.h>
#include <Core/Delegate.h>
#include <Core/Math/RandomStream.h>
#include <Core/Memory/SharedPtr.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Optional.h>
#include <Core/Variant.h>
#include <Tests/Support/HostFailure.h>

// 범용 값·소유권 타입의 계약 위반이 쓰레기 값으로 진행되지 않고 Fatal로 끝나는지 자식 프로세스에서
// 확인한다. 반환에 도달하면 하네스가 실패로 판정한다.
namespace rocks::test {

int RunOptionalEmptyValueControl() noexcept {
    PrepareFatalTestProcess();
    Optional<int> empty;
    return empty.Value();
}

int RunUniquePtrEmptyControl() noexcept {
    PrepareFatalTestProcess();
    UniquePtr<int> empty;
    return *empty;
}

// range-for 도중 삽입은 STL에서 정의되지 않은 동작이다. Rocks는 다음 반복 연산에서 Fatal로
// 드러낸다.
int RunHashMapIteratorAfterInsertControl() noexcept {
    PrepareFatalTestProcess();
    SystemAllocator allocator;
    HashMap<int, int> map(allocator);
    if (!map.Insert(1, 1) || !map.Insert(2, 2)) {
        return 2;
    }
    int visited = 0;
    for (auto entry : map) {
        visited += entry.value;
        if (!map.Insert(100 + visited, 0)) {
            return 2;
        }
    }
    return visited == 0 ? 2 : 0;
}

int RunSharedPtrEmptyControl() noexcept {
    PrepareFatalTestProcess();
    SharedPtr<int> empty;
    return *empty;
}

int RunDelegateExecuteUnboundControl() noexcept {
    PrepareFatalTestProcess();
    Delegate<int()> empty;
    return empty.Execute();
}

// 실행 중인 delegate를 그 호출 안에서 다른 대상으로 바꾸면 실행 중 저장소가 깨지므로 Fatal이다.
int RunDelegateRebindDuringExecutionControl() noexcept {
    PrepareFatalTestProcess();
    Delegate<void()> self;
    self = Delegate<void()>::CreateLambda(
        [&self]() noexcept { self = Delegate<void()>::CreateLambda([]() noexcept {}); });
    self.Execute();
    return 0;
}

int RunVariantWrongAlternativeControl() noexcept {
    PrepareFatalTestProcess();
    Variant<int, float> value = 1.0f;
    return value.Get<int>();
}

int RunSharedRefFromEmptyControl() noexcept {
    PrepareFatalTestProcess();
    SharedPtr<int> empty;
    SharedRef<int> reference = empty.ToSharedRef();
    return *reference;
}

// RandomStream의 전제 조건 위반(경계 0, 뒤집힌 범위, 잘못된 실수 범위)은 값으로 숨기지 않는다.
int RunRandomStreamPreconditionControl(uint32 operation) noexcept {
    PrepareFatalTestProcess();
    RandomStream random(7);
    switch (operation) {
    case 0:
        return static_cast<int>(random.NextUInt32Below(0));
    case 1:
        return random.NextInt32InRange(5, 1);
    default:
        return static_cast<int>(random.NextFloatInRange(1.0f, 1.0f));
    }
}

} // namespace rocks::test
