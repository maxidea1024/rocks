#pragma once

#include <Core/Containers/Array.h>
#include <Meta/Annotations.h>
#include <Object/Object.h>

namespace rocks::reflection_fixture {

// 리플렉션 B9a2 정수 폭 fixture다(Docs/Design/ReflectionExtensions.ko.md 2절). int8·int16 클래스
// 필드·struct 멤버·배열 원소와 B9a2가 연 enum underlying 넷(int8·int16·int64·uint64 — 경계값,
// uint64는 int64 최댓값까지, Flags uint64는 비트 62까지)에 uint32 경계값을 더해 MetaCompiler 생성
// 코드로 그래프 왕복·범위 밖 I64 거부·쓰기와 경로 쓰기의 범위 검사로 시험한다
// (IntegerWidthGraphTests). underlying 8종의 나머지 셋(uint8·uint16·int32)은 ReflectionFixture.h의
// enum이다(EnumGraphTests).
ROCKS_ENUM()
enum class ByteOffset : int8 {
    Low = NumericLimits<int8>::Min(),
    Zero = 0,
    High = NumericLimits<int8>::Max(),
};

ROCKS_ENUM()
enum class ShortOffset : int16 {
    Low = NumericLimits<int16>::Min(),
    Zero = 0,
    High = NumericLimits<int16>::Max(),
};

ROCKS_ENUM()
enum class WideOffset : int64 {
    Low = NumericLimits<int64>::Min(),
    Zero = 0,
    High = NumericLimits<int64>::Max(),
};

ROCKS_ENUM()
enum class CountLimit : uint32 {
    Zero = 0,
    Top = NumericLimits<uint32>::Max(),
};

// uint64 enum의 열거자 값은 int64 최댓값 이하다(저장 값이 int64 — 그보다 크면 RMC2401).
ROCKS_ENUM()
enum class HugeIndex : uint64 {
    Zero = 0,
    Top = static_cast<uint64>(NumericLimits<int64>::Max()),
};

// uint64 Flags enum은 비트 0–62를 쓴다.
ROCKS_ENUM(Flags)
enum class HugeMask : uint64 {
    None = 0,
    Bit0 = 1,
    Bit62 = uint64{1} << 62,
};

// int8·int16 struct 멤버(경로 thunk)와 int8 underlying enum 멤버다. 배열 원소(Array<S>)로도 쓴다.
ROCKS_STRUCT()
struct IntegerWidthPair {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() int8 tiny = 0;
    ROCKS_PROPERTY() int16 narrow = 0;
    ROCKS_PROPERTY() ByteOffset offset = ByteOffset::Zero;
};

ROCKS_CLASS()
struct IntegerWidthFields final : Object {
    ROCKS_GENERATED_BODY()

    explicit IntegerWidthFields(Allocator& allocator) noexcept
        : tinies(allocator), narrows(allocator), wideOffsets(allocator), pairs(allocator) {
    }

    ROCKS_PROPERTY() int8 tiny = 0;
    ROCKS_PROPERTY() int16 narrow = 0;
    ROCKS_PROPERTY() IntegerWidthPair pair;
    ROCKS_PROPERTY() Array<int8> tinies;
    ROCKS_PROPERTY() Array<int16> narrows;
    ROCKS_PROPERTY() ByteOffset byteOffset = ByteOffset::Zero;
    ROCKS_PROPERTY() ShortOffset shortOffset = ShortOffset::Zero;
    ROCKS_PROPERTY() WideOffset wideOffset = WideOffset::Zero;
    ROCKS_PROPERTY() CountLimit countLimit = CountLimit::Zero;
    ROCKS_PROPERTY() HugeIndex hugeIndex = HugeIndex::Zero;
    ROCKS_PROPERTY() HugeMask hugeMask = HugeMask::None;
    ROCKS_PROPERTY() Array<WideOffset> wideOffsets;
    ROCKS_PROPERTY() Array<IntegerWidthPair> pairs;
};

} // namespace rocks::reflection_fixture
