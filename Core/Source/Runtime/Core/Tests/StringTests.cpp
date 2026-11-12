#include <TestSupport/Test.h>
#include <TestSupport/NFailAllocator.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/String.h>

using namespace rocks;

namespace {

constexpr char Korean[] = "\xED\x95\x9C\xEA\xB8\x80";
constexpr char Emoji[] = "\xF0\x9F\x8E\xAE";

// 단계별 실패가 중첩 Result/String 소유권을 빠져나갈 때도 모두 해제되는지 검사한다.
Result<void> RunStringLifecycle(Allocator& allocator) noexcept {
    String value(allocator);
    auto assigned = value.TryAssign(Korean);
    if (!assigned) {
        return assigned;
    }

    auto appended = value.TryAppend(value.View());
    if (!appended) {
        return appended;
    }

    auto reserved = value.TryReserve(128);
    if (!reserved) {
        return reserved;
    }

    auto clone = value.TryClone(allocator);
    if (!clone) {
        return Result<void>::Failure(clone.Error());
    }

    auto expanded = clone.Value().TryAppend(clone.Value().View());
    if (!expanded) {
        return expanded;
    }

    String moved(Move(clone.Value()));
    if (moved.ByteCount() != 24 || !clone.Value().IsEmpty() || !moved.View().ValidateUtf8()) {
        return Result<void>::Failure(ErrorCode::DependencyFailed);
    }

    return Result<void>::Success();
}

} // namespace

TEST(StringEmptyAndNullViewsNeedNoAllocation) {
    static_assert(!__is_constructible(String, const String&));
    static_assert(IsNothrowMoveConstructibleV<String>);
    constexpr StringView literal("rocks");
    static_assert(literal.ByteCount() == 5);
    static_assert(literal == StringView("rocks", 5));

    SystemAllocator allocator;
    {
        String value(allocator);
        StringView empty;
        StringView nullString(nullptr);
        StringView nullRange(nullptr, 0);
        CHECK(empty == nullString);
        CHECK(nullString == nullRange);
        CHECK(nullRange == StringView(""));
        CHECK(empty.ValidateUtf8());
        CHECK(empty.TrySliceBytes(0, 0));
        CHECK(value.IsEmpty());
        CHECK(value.ByteCount() == 0);
        CHECK(value.ByteCapacity() == 0);
        REQUIRE(value.CStr() != nullptr);
        CHECK(value.CStr()[0] == '\0');
        CHECK(value.Data() == value.CStr());
        CHECK(value.TryAssign(nullString));
        CHECK(value.TryAppend(nullRange));
        CHECK(value.TryReserve(0));
        auto clone = value.TryClone(allocator);
        REQUIRE(clone);
        CHECK(clone.Value().IsEmpty());
        CHECK(clone.Value().CStr()[0] == '\0');
        value.Clear();
        value.Clear();
        CHECK(allocator.Stats().totalAllocations == 0);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(StringUtf8ScalarBoundariesAndByteCounts) {
    // Unicode scalar 범위 경계의 golden bytes다. U+0000 및 noncharacter도 유효한 scalar다.
    constexpr char boundaries[] = "\0\x7F\xC2\x80\xDF\xBF\xE0\xA0\x80\xED\x9F\xBF"
                                  "\xEE\x80\x80\xEF\xBF\xBF\xF0\x90\x80\x80\xF4\x8F\xBF\xBF";
    StringView golden(boundaries, sizeof(boundaries) - 1);
    CHECK(golden.ValidateUtf8());
    CHECK(StringView(Korean).ValidateUtf8());
    CHECK(StringView(Emoji).ValidateUtf8());
    CHECK(StringView(Korean).ByteCount() == 6);
    CHECK(StringView(Emoji).ByteCount() == 4);

    // 정규화를 암묵 적용하지 않으므로 같은 표시 글자의 서로 다른 byte 표현은 다르다.
    const StringView composed("\xC3\xA9");
    const StringView decomposed("e\xCC\x81");
    CHECK(composed.ValidateUtf8());
    CHECK(decomposed.ValidateUtf8());
    CHECK(composed != decomposed);

    SystemAllocator allocator;
    {
        String value(allocator);
        REQUIRE(value.TryAssign(golden));
        CHECK(value.View() == golden);
        CHECK(value.ByteCount() == sizeof(boundaries) - 1);
        CHECK(value.CStr()[value.ByteCount()] == '\0');
        REQUIRE(value.TryAssign(Korean));
        REQUIRE(value.TryAppend(Emoji));
        CHECK(value.ByteCount() == 10);
        CHECK(value.View().ValidateUtf8());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(StringRejectsMalformedUtf8WithoutMutation) {
    const StringView malformed[] = {
        "\xC0\x80",
        "\xC1\xBF",
        "\xE0\x80\x80",
        "\xF0\x80\x80\x80", // overlong
        "\xED\xA0\x80",
        "\xED\xBF\xBF", // surrogate
        "\xF4\x90\x80\x80",
        "\xF5\x80\x80\x80", // U+10FFFF 초과
        "\x80",
        "\xBF",
        "\xFE",
        "\xFF",
        "\xF8\x88\x80\x80\x80", // 잘못된 leading
        "\xC2",
        "\xE1",
        "\xE1\x80",
        "\xF1",
        "\xF1\x80",
        "\xF1\x80\x80", // 잘림
        "\xC2"
        "A",
        "\xE1\x80"
        "A",
        "\xF1\x80"
        "A\x80", // 잘못된 continuation
    };
    SystemAllocator allocator;
    {
        String value(allocator);
        REQUIRE(value.TryAssign("unchanged"));
        const char* original = value.Data();
        const usize capacity = value.ByteCapacity();
        const uint64 allocations = allocator.Stats().totalAllocations;
        for (StringView invalid : malformed) {
            const auto validation = invalid.ValidateUtf8();
            CHECK(!validation);
            CHECK(validation.Error() == ErrorCode::InvalidArgument);
            const auto assigned = value.TryAssign(invalid);
            CHECK(!assigned);
            CHECK(assigned.Error() == ErrorCode::InvalidArgument);
            const auto appended = value.TryAppend(invalid);
            CHECK(!appended);
            CHECK(appended.Error() == ErrorCode::InvalidArgument);
            CHECK(value.View() == StringView("unchanged"));
            CHECK(value.Data() == original);
            CHECK(value.ByteCapacity() == capacity);
            CHECK(value.CStr()[value.ByteCount()] == '\0');
        }
        CHECK(allocator.Stats().totalAllocations == allocations);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(StringViewSlicesRequireWholeInputAndScalarBoundaries) {
    const StringView value(Korean);
    auto first = value.TrySliceBytes(0, 3);
    auto second = value.TrySliceBytes(3, 3);
    REQUIRE(first);
    REQUIRE(second);
    CHECK(first.Value() == StringView("\xED\x95\x9C"));
    CHECK(second.Value() == StringView("\xEA\xB8\x80"));
    CHECK(second.Value().Data() == value.Data() + 3);
    CHECK(value.TrySliceBytes(3, 0));
    CHECK(value.TrySliceBytes(6, 0));
    CHECK(!value.TrySliceBytes(1, 2));
    CHECK(!value.TrySliceBytes(0, 1));
    CHECK(!value.TrySliceBytes(1, 0));
    CHECK(!value.TrySliceBytes(7, 0));
    CHECK(!value.TrySliceBytes(3, MaxSize));
    CHECK(!value.TrySliceBytes(MaxSize, 0));
    CHECK(!StringView("A\xFF").TrySliceBytes(0, 1));

    // combining mark 경계는 scalar 경계다. grapheme 단위 절단 제한은 이 API의 계약이 아니다.
    auto combining = StringView("e\xCC\x81").TrySliceBytes(1, 2);
    REQUIRE(combining);
    CHECK(combining.Value() == StringView("\xCC\x81"));
}

TEST(StringEmbeddedNullRetainsFullPayload) {
    constexpr char payload[] = {'a', '\0', 'b'};
    SystemAllocator allocator;
    {
        String value(allocator);
        REQUIRE(value.TryAssign(StringView(payload, sizeof(payload))));
        CHECK(value.ByteCount() == 3);
        CHECK(value.View() == StringView(payload, sizeof(payload)));
        CHECK(value.CStr()[1] == '\0');
        CHECK(value.CStr()[2] == 'b');
        CHECK(value.CStr()[3] == '\0');
        // 길이 없는 C 문자열 view는 첫 NUL에서 끝나며 소유 문자열의 길이는 유지된다.
        CHECK(StringView(value.CStr()) == StringView("a"));
        REQUIRE(value.TryAppend(StringView(payload, sizeof(payload))));
        CHECK(value.ByteCount() == 6);
        CHECK(value.CStr()[4] == '\0');
        CHECK(value.CStr()[5] == 'b');
        CHECK(value.CStr()[6] == '\0');
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(StringSelfAliasAssignAndAppendWithGrowth) {
    SystemAllocator allocator;
    {
        String value(allocator);
        REQUIRE(value.TryAssign("abc"));
        CHECK(value.ByteCapacity() == 3);
        REQUIRE(value.TryAppend(value.View()));
        CHECK(value.View() == StringView("abcabc"));
        REQUIRE(value.TryAssign("abcdef"));
        auto suffix = value.View().TrySliceBytes(2, 4);
        REQUIRE(suffix);
        REQUIRE(value.TryAppend(suffix.Value()));
        CHECK(value.View() == StringView("abcdefcdef"));
        REQUIRE(value.TryAssign(value.View()));
        CHECK(value.View() == StringView("abcdefcdef"));
        auto middle = value.View().TrySliceBytes(3, 5);
        REQUIRE(middle);
        REQUIRE(value.TryAssign(middle.Value()));
        CHECK(value.View() == StringView("defcd"));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(StringSelfAliasPreservesTerminatorAsExplicitPayload) {
    SystemAllocator allocator;
    {
        String value(allocator);
        REQUIRE(value.TryAssign("abc"));
        // 종단 NUL까지 명시한 자기 view는 유효한 4-byte payload이며 assign 재할당을 요구한다.
        REQUIRE(value.TryAssign(StringView(value.Data(), value.ByteCount() + 1)));
        const char expected[] = {'a', 'b', 'c', '\0'};
        CHECK(value.View() == StringView(expected, 4));
        CHECK(value.ByteCapacity() == 4);
        CHECK(value.CStr()[4] == '\0');
        REQUIRE(value.TryReserve(32));
        const char* reserved = value.Data();
        // source와 destination이 종단 byte에서 겹치는 in-place append 경로다.
        REQUIRE(value.TryAppend(StringView(value.Data(), value.ByteCount() + 1)));
        CHECK(value.Data() == reserved);
        CHECK(value.ByteCount() == 9);
        CHECK(value.CStr()[3] == '\0');
        CHECK(value.CStr()[4] == 'a');
        CHECK(value.CStr()[7] == '\0');
        CHECK(value.CStr()[8] == '\0');
        CHECK(value.CStr()[9] == '\0');
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(StringReservedStorageSupportsAliasWithoutAllocation) {
    SystemAllocator allocator;
    test::NFailAllocator faults(allocator);
    {
        String value(faults);
        REQUIRE(value.TryAssign(Korean));
        REQUIRE(value.TryReserve(64));
        const char* original = value.Data();
        faults.FailOnAttempt(1);
        REQUIRE(value.TryAppend(value.View()));
        auto second = value.View().TrySliceBytes(6, 6);
        REQUIRE(second);
        REQUIRE(value.TryAssign(second.Value()));
        CHECK(value.View() == StringView(Korean));
        CHECK(value.Data() == original);
        CHECK(faults.Attempts() == 0);
        value.Clear();
        value.Clear();
        CHECK(value.ByteCapacity() == 64);
        CHECK(value.Data() == original);
        CHECK(value.CStr()[0] == '\0');
        REQUIRE(value.TryAppend(Emoji));
        CHECK(value.View() == StringView(Emoji));
        CHECK(faults.Attempts() == 0);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(StringAllocationFailuresPreserveContentAndViews) {
    SystemAllocator allocator;
    test::NFailAllocator faults(allocator);
    {
        String value(faults);
        REQUIRE(value.TryAssign("stable"));
        const char* original = value.Data();
        const usize capacity = value.ByteCapacity();
        const uint64 liveBytes = allocator.Stats().liveBytes;
        const StringView view = value.View();
        faults.FailOnAttempt(1);
        auto reserve = value.TryReserve(256);
        CHECK(!reserve);
        CHECK(reserve.Error() == ErrorCode::OutOfMemory);
        faults.FailOnAttempt(1);
        auto assign = value.TryAssign("a larger replacement payload");
        CHECK(!assign);
        CHECK(assign.Error() == ErrorCode::OutOfMemory);
        faults.FailOnAttempt(1);
        auto append = value.TryAppend(value.View());
        CHECK(!append);
        CHECK(append.Error() == ErrorCode::OutOfMemory);
        faults.FailOnAttempt(1);
        auto aliasAssign = value.TryAssign(StringView(value.Data(), value.ByteCount() + 1));
        CHECK(!aliasAssign);
        CHECK(aliasAssign.Error() == ErrorCode::OutOfMemory);
        faults.FailOnAttempt(1);
        auto clone = value.TryClone(faults);
        CHECK(!clone);
        CHECK(clone.Error() == ErrorCode::OutOfMemory);
        CHECK(value.Data() == original);
        CHECK(value.ByteCapacity() == capacity);
        CHECK(value.View() == StringView("stable"));
        CHECK(view == StringView("stable"));
        CHECK(value.CStr()[value.ByteCount()] == '\0');
        CHECK(allocator.Stats().liveBytes == liveBytes);
        CHECK(allocator.Stats().liveAllocations == 1);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(StringCapacityOverflowDoesNotAllocateOrMutate) {
    SystemAllocator allocator;
    test::NFailAllocator faults(allocator);
    {
        String empty(faults);
        auto emptyReserve = empty.TryReserve(MaxSize);
        CHECK(!emptyReserve);
        CHECK(emptyReserve.Error() == ErrorCode::Overflow);
        CHECK(faults.Attempts() == 0);
        CHECK(empty.IsEmpty());
        CHECK(empty.CStr()[0] == '\0');
        String value(faults);
        REQUIRE(value.TryAssign("stable"));
        const char* original = value.Data();
        const usize capacity = value.ByteCapacity();
        faults.DisableFailure();
        auto reserve = value.TryReserve(MaxSize);
        CHECK(!reserve);
        CHECK(reserve.Error() == ErrorCode::Overflow);
        CHECK(value.Data() == original);
        CHECK(value.ByteCapacity() == capacity);
        CHECK(value.View() == StringView("stable"));
        CHECK(faults.Attempts() == 0);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(StringMoveAndCloneKeepAllocatorOwnership) {
    SystemAllocator firstAllocator;
    SystemAllocator secondAllocator;
    {
        String original(firstAllocator);
        REQUIRE(original.TryAssign(Korean));
        String moved(Move(original));
        CHECK(original.IsEmpty());
        CHECK(original.ByteCapacity() == 0);
        CHECK(original.CStr()[0] == '\0');
        REQUIRE(original.TryAppend("reused"));
        CHECK(original.View() == StringView("reused"));
        auto clone = moved.TryClone(secondAllocator);
        REQUIRE(clone);
        CHECK(clone.Value().View() == moved.View());
        CHECK(clone.Value().Data() != moved.Data());
        REQUIRE(moved.TryAssign("changed"));
        CHECK(clone.Value().View() == StringView(Korean));
        String destination(secondAllocator);
        REQUIRE(destination.TryAssign("previous"));
        destination = Move(moved);
        CHECK(destination.View() == StringView("changed"));
        CHECK(moved.IsEmpty());
        REQUIRE(moved.TryAssign("source reusable"));
        // pointer alias를 통해 자기 이동을 호출하여 경고 없이 self guard 계약을 검증한다.
        String* self = &destination;
        destination = Move(*self);
        CHECK(destination.View() == StringView("changed"));
        CHECK(firstAllocator.Stats().liveAllocations == 3);
        CHECK(secondAllocator.Stats().liveAllocations == 1);
    }
    CHECK(firstAllocator.Stats().liveAllocations == 0);
    CHECK(secondAllocator.Stats().liveAllocations == 0);
}

TEST(StringEveryAllocationFailureReleasesOwnedStorage) {
    SystemAllocator allocator;
    test::NFailAllocator faults(allocator);
    REQUIRE(RunStringLifecycle(faults));
    const uint64 allocationCount = faults.Attempts();
    CHECK(allocationCount == 5);
    CHECK(allocator.Stats().liveAllocations == 0);
    for (uint64 attempt = 1; attempt <= allocationCount; ++attempt) {
        faults.FailOnAttempt(attempt);
        auto completed = RunStringLifecycle(faults);
        CHECK(!completed);
        CHECK(completed.Error() == ErrorCode::OutOfMemory);
        CHECK(faults.Attempts() == attempt);
        CHECK(allocator.Stats().liveAllocations == 0);
        CHECK(allocator.Stats().liveBytes == 0);
    }
}

TEST(StringRepeatedLifecycleLeavesNoAllocations) {
    SystemAllocator allocator;
    for (usize cycle = 0; cycle < 1000; ++cycle) {
        REQUIRE(RunStringLifecycle(allocator));
        CHECK(allocator.Stats().liveAllocations == 0);
        CHECK(allocator.Stats().liveBytes == 0);
    }
}
