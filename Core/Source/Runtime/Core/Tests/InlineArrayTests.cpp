#include <Core/Containers/InlineArray.h>
#include <TestSupport/Test.h>

using namespace rocks;
namespace inline_array_tests_file {

namespace {

// 살아 있는 원소 수와 자기 주소를 추적해 생성·소멸 짝과 이동 뒤 원본 처리를 검사한다.
struct Counted {
    static inline int live = 0;
    int value;
    const Counted* self;

    explicit Counted(int initial) noexcept : value(initial), self(this) {
        ++live;
    }

    Counted(const Counted& other) noexcept : value(other.value), self(this) {
        ++live;
    }

    Counted(Counted&& other) noexcept : value(other.value), self(this) {
        other.value = -1;
        ++live;
    }

    ~Counted() noexcept {
        --live;
        self = nullptr;
    }

    Counted& operator=(const Counted&) = delete;

    bool Intact() const noexcept {
        return self == this;
    }
};

} // namespace

TEST(InlineArray_PushPopRespectsFixedCapacity) {
    InlineArray<uint32, 3> values;
    static_assert(InlineArray<uint32, 3>::MaxCapacity == 3);
    CHECK(values.IsEmpty() && !values.IsFull() && values.Capacity() == 3);
    REQUIRE(values.TryPushBack(10));
    REQUIRE(values.TryPushBack(20));
    values.PushBack(30);
    CHECK(values.IsFull() && values.Size() == 3);

    // 가득 찬 뒤의 추가는 넘치는 저장으로 바꾸지 않고 실패하며 내용을 그대로 둔다.
    CHECK(values.TryPushBack(40).Error() == ErrorCode::CapacityExceeded);
    CHECK(values.TryEmplaceBack(50u).Error() == ErrorCode::CapacityExceeded);
    CHECK(values.Size() == 3 && values[0] == 10 && values[2] == 30 && values.Back() == 30);

    CHECK(values.PopBack() == 30);
    CHECK(values.PopBack() == 20);
    CHECK(values.Size() == 1 && values.Back() == 10);
    values.Clear();
    CHECK(values.IsEmpty());
}

TEST(InlineArray_RemoveAtSwapAndIteration) {
    InlineArray<int32, 8> values;
    for (int32 index = 0; index < 5; ++index) {
        values.PushBack(index * 10);
    }
    values.RemoveAtSwap(1);
    // 마지막 원소(40)가 지운 자리로 온다.
    CHECK(values.Size() == 4 && values[1] == 40);
    values.RemoveAtSwap(values.Size() - 1);
    CHECK(values.Size() == 3 && values[2] == 20);

    int32 sum = 0;
    for (const int32 value : values) {
        sum += value;
    }
    CHECK(sum == 0 + 40 + 20);
    CHECK(values.View().Size() == 3 && values.View()[1] == 40);
}

TEST(InlineArray_ConstructsAndDestroysEachElementOnce) {
    Counted::live = 0;
    {
        InlineArray<Counted, 4> values;
        REQUIRE(values.TryEmplaceBack(1));
        REQUIRE(values.TryEmplaceBack(2));
        values.EmplaceBack(3);
        CHECK(Counted::live == 3);
        CHECK(values[0].Intact() && values[2].Intact());

        // 복사는 원소를 하나씩 복사하고 원본을 바꾸지 않는다.
        InlineArray<Counted, 4> copy(values);
        CHECK(Counted::live == 6 && copy[1].value == 2 && copy[1].Intact());

        // 이동은 원소를 옮기고 원본을 비운다(원본 원소도 소멸한다).
        InlineArray<Counted, 4> moved(Move(values));
        CHECK(values.IsEmpty() && moved.Size() == 3 && Counted::live == 6);
        CHECK(moved[2].value == 3 && moved[2].Intact());

        Counted popped = moved.PopBack();
        CHECK(popped.value == 3 && moved.Size() == 2 && Counted::live == 6);

        moved.RemoveAtSwap(0);
        CHECK(moved.Size() == 1 && moved[0].value == 2 && moved[0].Intact());
        CHECK(Counted::live == 5);

        copy = Move(moved);
        CHECK(copy.Size() == 1 && copy[0].value == 2 && moved.IsEmpty());
        CHECK(Counted::live == 2);
    }
    CHECK(Counted::live == 0);
}
} // namespace inline_array_tests_file
