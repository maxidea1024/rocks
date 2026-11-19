#include <Core/Memory/SystemAllocator.h>
#include <Object/GeneratedFieldCodec.h>
#include <Object/TypeRegistry.h>
#include <Serialization/Record.h>
#include <TestSupport/Test.h>

// 리플렉션 B9a2 정수 폭 시험이다(Docs/Design/ReflectionExtensions.ko.md 2절, 17절 B9a 항목): int8·
// int16의 C++ 형식 대응(평범한 char는 Opaque)·FieldValue 변환과 쓰기 범위 검사(Overflow, 실패하면
// 대상 불변)·codec의 wire(I64)와 폭 판정, enum underlying 8종(UE와 같다)의 범위 표와 RegisterEnum
// 경계값(uint64는 int64 최댓값까지, Flags uint64는 비트 62까지). 그래프 왕복·범위 밖 I64 거부·경로
// 쓰기는 ObjectSerialization의 IntegerWidthGraphTests가 생성 fixture로 본다.
using namespace rocks;
namespace integer_width_tests_file {

namespace {

static_assert(detail::ScalarValueKind<int8>() == ValueKind::Int8);
static_assert(detail::ScalarValueKind<signed char>() == ValueKind::Int8);
static_assert(detail::ScalarValueKind<int16>() == ValueKind::Int16);
static_assert(detail::ScalarValueKind<short>() == ValueKind::Int16);
// 평범한 char는 signed char·unsigned char와 다른 형식이고 부호가 구현 정의라 값 종류가 아니다.
static_assert(detail::ScalarValueKind<char>() == ValueKind::Opaque);
static_assert(detail::FieldValueKind<Array<int8>>() == ValueKind::Array);
static_assert(detail::FieldValueKind<int16>() == ValueKind::Int16);

FieldValue Signed(Allocator& allocator, ValueKind kind, int64 value) noexcept {
    FieldValue result(allocator);
    result.kind = kind;
    result.signedInteger = value;
    return result;
}

// int8·int16 대상 하나에 value를 써 보고 결과를 돌려준다. 실패하면 대상이 그대로인지도 본다.
template <ValueKind Target, class T>
ErrorCode WriteNarrow(T& target, const FieldValue& value) noexcept {
    const T before = target;
    auto written = detail::WriteTargetValue<Target>(target, value);
    if (!written) {
        CHECK(target == before);
        return written.Error();
    }

    return ErrorCode::None;
}

constexpr ValueKind UnderlyingKinds[] = {ValueKind::UInt8,  ValueKind::UInt16, ValueKind::UInt32,
                                         ValueKind::UInt64, ValueKind::Int8,   ValueKind::Int16,
                                         ValueKind::Int32,  ValueKind::Int64};

bool IsUnderlyingKind(ValueKind kind) noexcept {
    for (ValueKind underlying : UnderlyingKinds) {
        if (underlying == kind) {
            return true;
        }
    }

    return false;
}

uint8 NativeKey = 0;

// 열거자 둘(first·second)짜리 enum을 underlying으로 등록해 본 결과다. 성공하면 곧바로 해제한다.
ErrorCode RegisterPair(TypeRegistry& types, ValueKind underlying, uint32 flags, int64 first,
                       int64 second) noexcept {
    const EnumeratorSchema values[] = {{{"First", 5}, {}, {}, first, 0},
                                       {{"Second", 6}, {}, {}, second, 0}};
    const EnumSchema schema{Guid{0x494e5457, 1},
                            {"width::Pair", 11},
                            underlying,
                            flags,
                            Span<const EnumeratorSchema>(values, 2),
                            {}};
    auto token = types.RegisterEnum(schema, RuntimeEnumOps{&NativeKey, {}});
    if (!token) {
        return token.Error();
    }

    return types.Unregister(token.Value()) ? ErrorCode::None : ErrorCode::Busy;
}

} // namespace

// int8·int16은 FieldValue의 signedInteger로 오가고, 쓰기는 대상 폭 밖이면 Overflow(대상 불변)다.
// 종류가 다르면 TypeMismatch다.
TEST(IntegerWidth_FieldValueConversionChecksRange) {
    SystemAllocator allocator;
    FieldValue copied(allocator);
    REQUIRE(detail::CopyTargetValue<ValueKind::Int8>(int8{-5}, copied));
    CHECK(copied.kind == ValueKind::Int8 && copied.signedInteger == -5);
    REQUIRE(detail::CopyFieldValue(int16{-30000}, copied));
    CHECK(copied.kind == ValueKind::Int16 && copied.signedInteger == -30000);

    int8 tiny = 3;
    CHECK(WriteNarrow<ValueKind::Int8>(tiny, Signed(allocator, ValueKind::Int8, 127)) ==
          ErrorCode::None);
    CHECK(tiny == 127);
    CHECK(WriteNarrow<ValueKind::Int8>(tiny, Signed(allocator, ValueKind::Int8, -128)) ==
          ErrorCode::None);
    CHECK(tiny == -128);
    CHECK(WriteNarrow<ValueKind::Int8>(tiny, Signed(allocator, ValueKind::Int8, 128)) ==
          ErrorCode::Overflow);
    CHECK(WriteNarrow<ValueKind::Int8>(tiny, Signed(allocator, ValueKind::Int8, -129)) ==
          ErrorCode::Overflow);
    CHECK(WriteNarrow<ValueKind::Int8>(tiny, Signed(allocator, ValueKind::Int16, 1)) ==
          ErrorCode::TypeMismatch);

    int16 narrow = 7;
    CHECK(WriteNarrow<ValueKind::Int16>(narrow, Signed(allocator, ValueKind::Int16, 32767)) ==
          ErrorCode::None);
    CHECK(narrow == 32767);
    CHECK(WriteNarrow<ValueKind::Int16>(narrow, Signed(allocator, ValueKind::Int16, -32768)) ==
          ErrorCode::None);
    CHECK(narrow == -32768);
    CHECK(WriteNarrow<ValueKind::Int16>(narrow, Signed(allocator, ValueKind::Int16, 32768)) ==
          ErrorCode::Overflow);
    CHECK(WriteNarrow<ValueKind::Int16>(narrow, Signed(allocator, ValueKind::Int16, -32769)) ==
          ErrorCode::Overflow);
    CHECK(WriteNarrow<ValueKind::Int16>(narrow, Signed(allocator, ValueKind::Int8, 1)) ==
          ErrorCode::TypeMismatch);
}

// codec: 부호 있는 정수는 폭과 무관하게 I64 wire이고(Int8·Int16 원소 배열도 ValueArray), 폭 판정은
// 저장·로드가 같은 FitsSignedLimit다.
TEST(IntegerWidth_CodecWireAndWidthLimits) {
    const uint32 referenceWire = static_cast<uint32>(WireType::U32);
    const ValueKind signedKinds[] = {ValueKind::Int8, ValueKind::Int16, ValueKind::Int32,
                                     ValueKind::Int64};
    for (ValueKind kind : signedKinds) {
        auto wire = detail::FieldWireType(kind, referenceWire);
        REQUIRE(wire);
        CHECK(wire.Value() == static_cast<uint32>(WireType::I64));
    }

    FieldSchema array;
    array.value = ValueType{ValueKind::Array, {}};
    array.element = ValueType{ValueKind::Int16, {}};
    auto arrayWire = detail::FieldWireType(array, referenceWire);
    REQUIRE(arrayWire);
    CHECK(arrayWire.Value() == static_cast<uint32>(WireType::ValueArray));

    CHECK(detail::SignedLimit(ValueKind::Int8) == NumericLimits<int8>::Max());
    CHECK(detail::SignedLimit(ValueKind::Int16) == NumericLimits<int16>::Max());
    CHECK(detail::SignedLimit(ValueKind::Int32) == NumericLimits<int32>::Max());
    CHECK(detail::SignedLimit(ValueKind::Int64) == 0);
    CHECK(detail::FitsSignedLimit(ValueKind::Int8, -128) &&
          detail::FitsSignedLimit(ValueKind::Int8, 127));
    CHECK(!detail::FitsSignedLimit(ValueKind::Int8, -129) &&
          !detail::FitsSignedLimit(ValueKind::Int8, 128));
    CHECK(detail::FitsSignedLimit(ValueKind::Int16, -32768) &&
          detail::FitsSignedLimit(ValueKind::Int16, 32767));
    CHECK(!detail::FitsSignedLimit(ValueKind::Int16, -32769) &&
          !detail::FitsSignedLimit(ValueKind::Int16, 32768));
    CHECK(detail::FitsSignedLimit(ValueKind::Int64, NumericLimits<int64>::Min()) &&
          detail::FitsSignedLimit(ValueKind::Int64, NumericLimits<int64>::Max()));
}

// enum underlying은 UE와 같은 정수 8종이고(그 밖의 종류는 범위가 없다) 범위는 C++ 폭과 같다.
// UInt64의 최댓값은 int64 최댓값이다(저장 값이 int64).
TEST(IntegerWidth_EnumUnderlyingRangesMatchUe) {
    for (uint32 value = 0; value <= ValueKindCount; ++value) {
        const ValueKind kind = static_cast<ValueKind>(value);
        int64 minimum = 1;
        int64 maximum = -1;
        CHECK(EnumUnderlyingRange(kind, minimum, maximum) == IsUnderlyingKind(kind));
        // 지원하지 않는 종류는 출력 인자를 건드리지 않는다(표지 값 그대로).
        if (!IsUnderlyingKind(kind)) {
            CHECK(minimum == 1 && maximum == -1);
        }
    }

    struct Range {
        ValueKind kind;
        int64 minimum;
        int64 maximum;
    };

    const Range ranges[] = {
        {ValueKind::UInt8, 0, NumericLimits<uint8>::Max()},
        {ValueKind::UInt16, 0, NumericLimits<uint16>::Max()},
        {ValueKind::UInt32, 0, static_cast<int64>(NumericLimits<uint32>::Max())},
        {ValueKind::UInt64, 0, NumericLimits<int64>::Max()},
        {ValueKind::Int8, NumericLimits<int8>::Min(), NumericLimits<int8>::Max()},
        {ValueKind::Int16, NumericLimits<int16>::Min(), NumericLimits<int16>::Max()},
        {ValueKind::Int32, NumericLimits<int32>::Min(), NumericLimits<int32>::Max()},
        {ValueKind::Int64, NumericLimits<int64>::Min(), NumericLimits<int64>::Max()}};
    for (const Range& range : ranges) {
        int64 minimum = 1;
        int64 maximum = -1;
        REQUIRE(EnumUnderlyingRange(range.kind, minimum, maximum));
        CHECK(minimum == range.minimum && maximum == range.maximum);
    }
}

// RegisterEnum은 underlying 8종의 경계값을 받고 범위 밖 값(표현할 수 있으면)을 거부한다. uint64는
// int64 최댓값까지(음수 거부), Flags uint64는 비트 62까지이며 그 enum의 선언 값 판정·대체 값도 같은
// 범위를 쓴다. 정수가 아닌 underlying은 InvalidArgument다.
TEST(IntegerWidth_RegisterEnumAcceptsEightUnderlyingKinds) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    for (ValueKind kind : UnderlyingKinds) {
        int64 minimum = 0;
        int64 maximum = 0;
        REQUIRE(EnumUnderlyingRange(kind, minimum, maximum));
        CHECK(RegisterPair(types, kind, 0, minimum, maximum) == ErrorCode::None);
        if (minimum != NumericLimits<int64>::Min()) {
            CHECK(RegisterPair(types, kind, 0, minimum - 1, maximum) == ErrorCode::InvalidArgument);
        }

        if (maximum != NumericLimits<int64>::Max()) {
            CHECK(RegisterPair(types, kind, 0, minimum, maximum + 1) == ErrorCode::InvalidArgument);
        }
    }

    CHECK(RegisterPair(types, ValueKind::Bool, 0, 0, 1) == ErrorCode::InvalidArgument);
    CHECK(RegisterPair(types, ValueKind::Float64, 0, 0, 1) == ErrorCode::InvalidArgument);
    CHECK(RegisterPair(types, ValueKind::Enum, 0, 0, 1) == ErrorCode::InvalidArgument);

    // Flags uint64: 비트 62까지 받고 음수(비트 63)는 거부한다.
    const uint32 flags = static_cast<uint32>(EnumFlags::Flags);
    const int64 bit62 = int64{1} << 62;
    CHECK(RegisterPair(types, ValueKind::UInt64, flags, 1, bit62) == ErrorCode::None);
    CHECK(RegisterPair(types, ValueKind::UInt64, flags, 1, NumericLimits<int64>::Min()) ==
          ErrorCode::InvalidArgument);
    const EnumeratorSchema values[] = {{{"Low", 3}, {}, {}, 1, 0}, {{"Top", 3}, {}, {}, bit62, 0}};
    const EnumSchema mask{Guid{0x494e5457, 2},
                          {"width::Mask", 11},
                          ValueKind::UInt64,
                          flags,
                          Span<const EnumeratorSchema>(values, 2),
                          {}};
    CHECK(IsDeclaredEnumValue(mask, bit62 | 1));
    CHECK(!IsDeclaredEnumValue(mask, 2));
    CHECK(!IsDeclaredEnumValue(mask, -1));
    CHECK(EnumReplacementValue(mask, bit62 | 6) == bit62);
    CHECK(EnumReplacementValue(mask, -1) == 0);

    // int8 enum은 범위 밖 저장 값을 선언되지 않은 값으로 본다. 일반 enum은 값 비교만으로도 같은
    // 답이므로 IsDeclaredEnumValue의 범위 검사 자체는 아래 Flags 대체 값과 그래프 시험
    // (IntegerWidthGraph_LoadRejectsOutOfRangeValues의 byteOffset 128)이 가려낸다.
    const EnumeratorSchema tinyValues[] = {{{"Low", 3}, {}, {}, -128, 0},
                                           {{"High", 4}, {}, {}, 127, 0}};
    const EnumSchema tiny{Guid{0x494e5457, 3},
                          {"width::Tiny", 11},
                          ValueKind::Int8,
                          0u,
                          Span<const EnumeratorSchema>(tinyValues, 2),
                          {}};
    CHECK(IsDeclaredEnumValue(tiny, -128) && IsDeclaredEnumValue(tiny, 127));
    CHECK(!IsDeclaredEnumValue(tiny, 128 + 127) && !IsDeclaredEnumValue(tiny, -256 - 128));

    // int8 Flags enum: underlying 범위(127) 밖 저장 값의 대체 값은 0이다(범위 검사 없이 mask만
    // 적용하면 257 & 65 = 1이 된다).
    const EnumeratorSchema bitValues[] = {{{"Low", 3}, {}, {}, 1, 0}, {{"High", 4}, {}, {}, 64, 0}};
    const EnumSchema bits{Guid{0x494e5457, 4},
                          {"width::Bits", 11},
                          ValueKind::Int8,
                          flags,
                          Span<const EnumeratorSchema>(bitValues, 2),
                          {}};
    CHECK(EnumReplacementValue(bits, 64 | 2) == 64);
    CHECK(EnumReplacementValue(bits, 256 + 1) == 0);
    CHECK(!IsDeclaredEnumValue(bits, 256 + 1));
    CHECK(types.Shutdown());
}
} // namespace integer_width_tests_file
