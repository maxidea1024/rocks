#include <Core/Containers/Span.h>
#include <TestSupport/Test.h>

using namespace rocks;

namespace {

struct Pair {
    uint32 first;
    uint32 second;
};

} // namespace

TEST(SpanEmptyAndByteCount) {
    Span<int> empty;
    CHECK(empty.IsEmpty());
    CHECK(empty.Size() == 0);
    CHECK(empty.ByteCount() == 0);
    CHECK(empty.Data() == nullptr);

    Pair pairs[3] = {};
    Span<Pair> view(pairs);
    CHECK(!view.IsEmpty());
    CHECK(view.Size() == 3);
    CHECK(view.ByteCount() == 3 * sizeof(Pair));
}

TEST(SpanSliceAndTrySliceBounds) {
    int values[6] = {0, 1, 2, 3, 4, 5};
    Span<int> view(values);

    Span<int> middle = view.Slice(2, 3);
    CHECK(middle.Data() == values + 2);
    CHECK(middle.Size() == 3);

    // 끝 경계: 빈 꼬리와 전체 범위는 허용한다.
    CHECK(view.Slice(6, 0).IsEmpty());
    CHECK(view.Slice(6, 0).Data() == values + 6);
    CHECK(view.Slice(0, 6).Size() == 6);

    auto inside = view.TrySlice(1, 5);
    REQUIRE(inside.IsOk());
    CHECK(inside.Value().Data() == values + 1);
    CHECK(inside.Value().Size() == 5);

    CHECK(view.TrySlice(7, 0).Error() == ErrorCode::InvalidArgument);
    CHECK(view.TrySlice(5, 2).Error() == ErrorCode::InvalidArgument);
    // index + count가 usize를 넘쳐 작은 값으로 감기는 경우도 거부한다.
    CHECK(view.TrySlice(1, MaxSize).Error() == ErrorCode::InvalidArgument);
    CHECK(view.TrySlice(MaxSize, 1).Error() == ErrorCode::InvalidArgument);

    // 빈 view의 0 길이 slice는 nullptr 저장소에서도 성공한다.
    Span<int> empty;
    CHECK(empty.Slice(0, 0).IsEmpty());
    CHECK(empty.TrySlice(0, 0).IsOk());
    CHECK(empty.TrySlice(0, 1).Error() == ErrorCode::InvalidArgument);
}

TEST(SpanLeftRightChopAndMidClamp) {
    int values[5] = {10, 11, 12, 13, 14};
    Span<const int> view(values);

    CHECK(view.Left(2).Data() == values && view.Left(2).Size() == 2);
    CHECK(view.Left(9).Size() == 5);
    CHECK(view.LeftChop(2).Data() == values && view.LeftChop(2).Size() == 3);
    CHECK(view.LeftChop(9).IsEmpty());

    CHECK(view.Right(2).Data() == values + 3 && view.Right(2).Size() == 2);
    CHECK(view.Right(9).Data() == values && view.Right(9).Size() == 5);
    CHECK(view.RightChop(2).Data() == values + 2 && view.RightChop(2).Size() == 3);
    CHECK(view.RightChop(9).IsEmpty());
    CHECK(view.RightChop(9).Data() == values + 5);

    CHECK(view.Mid(1, 3).Data() == values + 1 && view.Mid(1, 3).Size() == 3);
    CHECK(view.Mid(3).Size() == 2);
    CHECK(view.Mid(4, 9).Size() == 1);
    CHECK(view.Mid(9, 2).IsEmpty());
    CHECK(view.Mid(9, 2).Data() == values + 5);

    // Left + RightChop은 원래 범위를 빠짐없이 나눈다.
    for (usize split = 0; split <= view.Size(); ++split) {
        CHECK(view.Left(split).Size() + view.RightChop(split).Size() == view.Size());
        CHECK(view.Left(split).Data() + view.Left(split).Size() == view.RightChop(split).Data());
    }

    Span<int> empty;
    CHECK(empty.Left(3).IsEmpty() && empty.Right(3).IsEmpty());
    CHECK(empty.LeftChop(3).IsEmpty() && empty.RightChop(3).IsEmpty());
    CHECK(empty.Mid(0).IsEmpty());
}

TEST(SpanByteViewsShareStorage) {
    Pair pairs[2] = {{1, 2}, {3, 4}};

    Span<const uint8> bytes = AsBytes(Span<const Pair>(pairs));
    CHECK(bytes.Size() == sizeof(pairs));
    CHECK(static_cast<const void*>(bytes.Data()) == static_cast<const void*>(pairs));

    Span<uint8> writable = AsWritableBytes(Span<Pair>(pairs));
    CHECK(writable.Size() == sizeof(pairs));
    for (uint8& value : writable) {
        value = 0;
    }
    CHECK(pairs[0].first == 0 && pairs[1].second == 0);

    Span<const uint8> none = AsBytes(Span<const Pair>());
    CHECK(none.IsEmpty());
    CHECK(none.Data() == nullptr);
}

TEST(SpanOverlapsComparesByteRanges) {
    uint8 buffer[16] = {};
    Span<uint8> whole(buffer);

    CHECK(Overlaps(whole, whole));
    CHECK(Overlaps(whole.Left(8), whole.Slice(4, 8)));
    CHECK(!Overlaps(whole.Left(8), whole.RightChop(8))); // 맞닿은 범위는 겹치지 않는다
    CHECK(Overlaps(whole.Slice(2, 2), whole));           // 포함
    CHECK(!Overlaps(whole.Slice(4, 0), whole));          // 빈 view
    CHECK(!Overlaps(Span<uint8>(), whole));

    // 원소 크기가 달라도 byte 범위로 판정한다.
    uint32 words[4] = {};
    Span<const uint32> wordView(words);
    Span<const uint8> lastByte = AsBytes(wordView).Right(1);
    CHECK(Overlaps(wordView.Right(1), lastByte));
    CHECK(!Overlaps(wordView.Left(3), lastByte));
}
