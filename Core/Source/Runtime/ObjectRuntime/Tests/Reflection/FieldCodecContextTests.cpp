#include <Core/Memory/SystemAllocator.h>
#include <Object/GeneratedFieldCodec.h>
#include <Serialization/Archive.h>
#include <Serialization/Record.h>
#include <TestSupport/Test.h>

// 공용 codec(EncodeFieldWire·ParseFieldValue·배열 codec·EnumValueSet)을 직렬화기 없이
// 시험한다(Docs/Design/ReflectionFoundations.ko.md 5.6·6.4절): 진단 받을 곳이 없으면
// ReportAndDefault도 거부(CanReport), 값 검사 순서(wire 종류 TypeMismatch → 정수 폭 Overflow → 그
// 밖과 trailing byte InvalidArgument), 등록 enum 검증 작업량의 청구, 배열의 AddElements 시점,
// 빈 enum 배열의 청구, EnumValueSet 판정이 스키마 규칙과 같음.
using namespace rocks;

namespace codec_fixture {

// 배열 codec 시험의 리플렉션 enum이다(생성 GeneratedType<E>와 같은 모양의 Id·IsDeclared).
enum class Shade : int32 {
    Light = 2,
    Dark = 9
};

constexpr EnumeratorSchema ShadeValues[] = {{{"Light", 5}, {}, {}, 2, 0},
                                            {{"Dark", 4}, {}, {}, 9, 0}};
constexpr EnumSchema ShadeEnum{Guid{0x534844, 1},
                               {"Shade", 5},
                               ValueKind::Int32,
                               0u,
                               Span<const EnumeratorSchema>(ShadeValues, 2),
                               {}};

} // namespace codec_fixture

namespace rocks {

template <>
struct GeneratedType<codec_fixture::Shade> {
    static constexpr TypeId Id() noexcept {
        return codec_fixture::ShadeEnum.id;
    }

    static constexpr bool IsDeclared(int64 value) noexcept {
        return IsDeclaredEnumValue(codec_fixture::ShadeEnum, value);
    }
};

} // namespace rocks

namespace {

constexpr uint32 Wire(WireType type) noexcept {
    return static_cast<uint32>(type);
}

constexpr EnumeratorSchema FlagValues[] = {{{}, {}, {}, 1, 0}, {{}, {}, {}, 4, 0}};
constexpr EnumSchema FlagsEnum{
    Guid{0x464c41, 2}, {}, ValueKind::UInt8, 1u, Span<const EnumeratorSchema>(FlagValues, 2), {}};

FieldSchema Field(ValueKind kind, TypeId type = {}) noexcept {
    FieldSchema field;
    field.id = Guid{0x434f44, 1};
    field.kind = FieldKind::Value;
    field.value = ValueType{kind, type};
    return field;
}

class CountingHooks final : public FieldCodecHooks {
public:
    Result<void> Report(const FieldDiagnostic& diagnostic) noexcept override {
        ++reports;
        last = diagnostic.value;
        return Result<void>::Success();
    }

    usize reports = 0;
    int64 last = 0;
};

// payload를 그대로 여는 복원 문맥이다. Spend·ReserveApply는 합만 센다.
class TestDecodeContext final : public FieldDecodeContext {
public:
    TestDecodeContext(Allocator& allocator, const FieldDecodePolicy& policy,
                      FieldCodecHooks* hooks) noexcept
        : FieldDecodeContext(policy, hooks, Wire(WireType::U64)), allocator_(allocator) {
    }

    // 복원 2단계처럼 진단·수집을 버린다.
    void EnterApplyPhase() noexcept {
        SetRecording(false);
    }

    Result<ArchiveReader> OpenTopLevelValue(const FieldView& field) noexcept override {
        return ArchiveReader::Create(field.payload);
    }

    // 이 시험은 struct 값을 다루지 않는다.
    Result<RecordScope> OpenRecord(const FieldView&, TypeId, uint32, uint32) noexcept override {
        return Result<RecordScope>::Failure(ErrorCode::Unsupported);
    }

    Result<RecordScope> OpenMigratedRecord(Span<const uint8>, TypeId, uint32) noexcept override {
        return Result<RecordScope>::Failure(ErrorCode::Unsupported);
    }

    Result<Span<const uint8>> MigrateStruct(TypeId, uint32, uint32,
                                            Span<const uint8>) noexcept override {
        return Result<Span<const uint8>>::Failure(ErrorCode::Unsupported);
    }

    // struct 원소 배열은 그래프 시험(ArrayGraphTests)이 다룬다.
    Result<RecordListReader> OpenRecordList(const FieldView&, TypeId) noexcept override {
        return Result<RecordListReader>::Failure(ErrorCode::Unsupported);
    }

    Result<RecordScope> OpenListItem(RecordListReader&, usize) noexcept override {
        return Result<RecordScope>::Failure(ErrorCode::Unsupported);
    }

    Result<void> AddFields(usize) noexcept override {
        return Result<void>::Success();
    }

    // 배열 codec이 원소를 읽거나 대상을 건드리기 전에 부르는지 본다(호출 수·그때까지의 Spend).
    Result<void> AddElements(usize count) noexcept override {
        ++elementCalls;
        spentBeforeElements = spent;
        if (failElements) {
            return Result<void>::Failure(ErrorCode::CapacityExceeded);
        }

        elements += count;
        return Result<void>::Success();
    }

    Result<DecodedReference> ReadReference(ArchiveReader&,
                                           const ReferenceExpectation&) noexcept override {
        return Result<DecodedReference>::Failure(ErrorCode::Unsupported);
    }

    Result<void> Spend(uint64 work) noexcept override {
        spent += work;
        return Result<void>::Success();
    }

    Result<void> ReserveApply(uint64 work) noexcept override {
        reserved += work;
        return Result<void>::Success();
    }

    Allocator& Scratch() noexcept override {
        return allocator_;
    }

    Allocator& ObservationScratch() noexcept override {
        return allocator_;
    }

    uint64 spent = 0;
    uint64 reserved = 0;
    usize elements = 0;
    usize elementCalls = 0;
    uint64 spentBeforeElements = 0;
    bool failElements = false;

private:
    Allocator& allocator_;
};

class TestEncodeContext final : public FieldEncodeContext {
public:
    TestEncodeContext(Allocator& allocator, const FieldEncodePolicy& policy) noexcept
        : FieldEncodeContext(policy, nullptr, Wire(WireType::U64)), allocator_(allocator) {
    }

    Result<void> Spend(uint64 work) noexcept override {
        spent += work;
        return Result<void>::Success();
    }

    Result<void> ReserveApply(uint64) noexcept override {
        return Result<void>::Success();
    }

    Allocator& Scratch() noexcept override {
        return allocator_;
    }

    Allocator& ObservationScratch() noexcept override {
        return allocator_;
    }

    Result<void> AddFields(usize) noexcept override {
        return Result<void>::Success();
    }

    Result<void> AddElements(usize count) noexcept override {
        ++elementCalls;
        spentBeforeElements = spent;
        elements += count;
        return Result<void>::Success();
    }

    RecordLimits NestedRecordLimits() const noexcept override {
        return {};
    }

    uint64 spent = 0;
    usize elements = 0;
    usize elementCalls = 0;
    uint64 spentBeforeElements = 0;

protected:
    Result<void> WriteReferenceWire(ArchiveWriter&, ObjectHandle,
                                    const ReferenceExpectation&) noexcept override {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

private:
    Allocator& allocator_;
};

// I64 하나(+ trailing byte)짜리 payload다.
Result<Array<uint8>> I64Payload(Allocator& allocator, int64 value, bool trailing) noexcept {
    ArchiveWriter writer(allocator);
    auto written = writer.WriteI64(value);
    if (written && trailing) {
        written = writer.WriteBool(true);
    }

    if (!written) {
        return Result<Array<uint8>>::Failure(written.Error());
    }

    return Result<Array<uint8>>::Success(writer.TakeBytes());
}

Result<Array<uint8>> U32Payload(Allocator& allocator, uint32 value, bool trailing) noexcept {
    ArchiveWriter writer(allocator);
    auto written = writer.WriteU32(value);
    if (written && trailing) {
        written = writer.WriteBool(true);
    }

    if (!written) {
        return Result<Array<uint8>>::Failure(written.Error());
    }

    return Result<Array<uint8>>::Success(writer.TakeBytes());
}

} // namespace

// 진단을 받을 곳이 없으면(기록 중인데 hooks 없음) ReportAndDefault도 InvalidArgument다. 기록하지
// 않는 단계(복원 2단계)는 1단계가 이미 판정했으므로 대체 값을 쓴다. 등록 enum 검증은 열거자 수만큼,
// 대체 값 계산은 그만큼 더 청구한다.
TEST(FieldCodec_ReportAndDefaultNeedsAReceiver) {
    SystemAllocator allocator;
    const FieldSchema channels = Field(ValueKind::Enum, FlagsEnum.id);
    auto payload = I64Payload(allocator, 7, false);
    REQUIRE(payload);
    const FieldView view{channels.id, Wire(WireType::I64), payload.Value().View()};
    FieldDecodePolicy policy;
    policy.undeclaredEnum = UndeclaredEnumPolicy::ReportAndDefault;
    {
        TestDecodeContext context(allocator, policy, nullptr);
        FieldScope scope(context, channels, Guid{0x4f424a, 1}, &FlagsEnum);
        CHECK(!context.CanReport());
        CHECK(detail::ParseFieldValue(view, context, nullptr, nullptr).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(context.spent == FlagsEnum.values.Size());

        // 2단계(기록하지 않음)는 같은 입력에서 대체 값을 쓴다.
        context.EnterApplyPhase();
        FieldValue value(allocator);
        CHECK(detail::ParseFieldValue(view, context, nullptr, &value));
        CHECK(value.signedInteger == 5);
    }

    {
        CountingHooks hooks;
        TestDecodeContext context(allocator, policy, &hooks);
        FieldScope scope(context, channels, Guid{0x4f424a, 1}, &FlagsEnum);
        FieldValue value(allocator);
        CHECK(detail::ParseFieldValue(view, context, nullptr, &value));
        CHECK(value.signedInteger == 5);
        CHECK(value.type == FlagsEnum.id);
        CHECK(hooks.reports == 1);
        CHECK(hooks.last == 7);
        CHECK(context.spent == 2 * FlagsEnum.values.Size());
        CHECK(context.reserved == 1);
    }

    // 저장도 등록 enum 검증 몫을 청구한다.
    {
        TestEncodeContext context(allocator, {});
        FieldScope scope(context, channels, Guid{0x4f424a, 1}, &FlagsEnum);
        FieldValue value(allocator);
        value.kind = ValueKind::Enum;
        value.type = FlagsEnum.id;
        value.signedInteger = 5;
        ArchiveWriter out(allocator);
        auto wire = detail::EncodeFieldWire(out, context, value, {}, nullptr);
        REQUIRE(wire);
        CHECK(wire.Value() == Wire(WireType::I64));
        CHECK(context.spent == FlagsEnum.values.Size());
        value.signedInteger = 2;
        CHECK(detail::EncodeFieldWire(out, context, value, {}, nullptr).Error() ==
              ErrorCode::InvalidArgument);
    }
}

// 값 검사 순서: wire 종류(TypeMismatch) → 정수 폭(Overflow) → trailing byte(InvalidArgument).
TEST(FieldCodec_IntegerWidthOrder) {
    SystemAllocator allocator;
    struct Case {
        ValueKind kind;
        WireType wire;
        bool signedPayload;
        int64 value;
        bool trailing;
        ErrorCode expected;
    };

    const Case cases[] = {
        {ValueKind::Int32, WireType::U64, true, 1, false, ErrorCode::TypeMismatch},
        {ValueKind::Int32, WireType::I64, true, 2147483648ll, true, ErrorCode::Overflow},
        {ValueKind::Int32, WireType::I64, true, -2147483649ll, false, ErrorCode::Overflow},
        {ValueKind::Int32, WireType::I64, true, -2147483648ll, true, ErrorCode::InvalidArgument},
        {ValueKind::Int32, WireType::I64, true, -2147483648ll, false, ErrorCode::None},
        {ValueKind::UInt16, WireType::U32, false, 65536, true, ErrorCode::Overflow},
        {ValueKind::UInt16, WireType::U32, false, 65535, true, ErrorCode::InvalidArgument},
        {ValueKind::UInt16, WireType::U32, false, 65535, false, ErrorCode::None},
        {ValueKind::UInt8, WireType::U32, false, 256, false, ErrorCode::Overflow},
    };

    for (const Case& example : cases) {
        const FieldSchema field = Field(example.kind);
        auto payload =
            example.signedPayload
                ? I64Payload(allocator, example.value, example.trailing)
                : U32Payload(allocator, static_cast<uint32>(example.value), example.trailing);
        REQUIRE(payload);
        TestDecodeContext context(allocator, {}, nullptr);
        FieldScope scope(context, field, Guid{0x4f424a, 2});
        const FieldView view{field.id, Wire(example.wire), payload.Value().View()};
        FieldValue value(allocator);
        auto parsed = detail::ParseFieldValue(view, context, nullptr, &value);
        if (example.expected == ErrorCode::None) {
            REQUIRE(parsed);
            if (example.kind == ValueKind::Int32) {
                CHECK(value.signedInteger == example.value);
            } else {
                CHECK(value.unsignedInteger == static_cast<uint64>(example.value));
            }
        } else {
            CHECK(parsed.Error() == example.expected);
        }
    }
}

namespace {

// 원소 형식이 element인 배열 필드다.
FieldSchema ArrayField(ValueKind element, TypeId type = {}, uint32 maxElements = 8) noexcept {
    FieldSchema field = Field(ValueKind::Array);
    field.element = ValueType{element, type};
    field.maxElements = maxElements;
    return field;
}

} // namespace

// 배열 codec은 원소 수를 AddElements로 원소를 읽거나 대상을 비우기 전에(enum 판정 집합을 만들기
// 전에도) 한 번 센다 — 거부되면 대상은 그대로다. 저장은 필드 maxElements를 넘으면 AddElements 전에
// CapacityExceeded다.
TEST(FieldCodec_ArraysCountElementsBeforeTouchingTheTarget) {
    SystemAllocator allocator;
    const FieldSchema field = ArrayField(ValueKind::UInt32);
    Array<uint32> values(allocator);
    REQUIRE(values.TryPushBack(1u) && values.TryPushBack(2u) && values.TryPushBack(3u));
    TestEncodeContext encode(allocator, {});
    ArchiveWriter out(allocator);
    {
        FieldScope scope(encode, field, Guid{0x4f424a, 3});
        auto wire = detail::EncodeArrayField(values, out, encode);
        REQUIRE(wire);
        CHECK(wire.Value() == Wire(WireType::ValueArray));
    }

    CHECK(encode.elementCalls == 1 && encode.elements == 3 && encode.spentBeforeElements == 0);
    const FieldView view{field.id, Wire(WireType::ValueArray), out.View()};
    Array<uint32> target(allocator);
    REQUIRE(target.TryPushBack(7u));
    {
        TestDecodeContext decode(allocator, {}, nullptr);
        decode.failElements = true;
        FieldScope scope(decode, field, Guid{0x4f424a, 3});
        CHECK(detail::DecodeArrayField(nullptr, ObjectHandle{}, &target, view, decode).Error() ==
              ErrorCode::CapacityExceeded);
        CHECK(decode.elementCalls == 1 && decode.reserved == 0);
        CHECK(target.Size() == 1 && target[0] == 7);
    }

    {
        TestDecodeContext decode(allocator, {}, nullptr);
        FieldScope scope(decode, field, Guid{0x4f424a, 3});
        CHECK(detail::DecodeArrayField(nullptr, ObjectHandle{}, &target, view, decode));
        CHECK(decode.elementCalls == 1 && decode.elements == 3);
        CHECK(target.Size() == 3 && target[2] == 3);
        // 2단계 몫: 원소 메모리 + 원소 수 + 1.
        CHECK(decode.reserved == 3 * sizeof(uint32) + 3 + 1);
    }

    const FieldSchema narrow = ArrayField(ValueKind::UInt32, {}, 2);
    TestEncodeContext limited(allocator, {});
    FieldScope scope(limited, narrow, Guid{0x4f424a, 3});
    CHECK(detail::EncodeArrayField(values, out, limited).Error() == ErrorCode::CapacityExceeded);
    CHECK(limited.elementCalls == 0);
}

// 빈 enum 배열은 EnumValueSet을 만들지도 조회 작업을 청구하지도 않는다(저장·복원 같은 조건). 원소가
// 있으면 집합 한 번(열거자 수)과 원소 수 × 조회 작업(EnumLookupWork)을 AddElements 뒤에 청구한다.
TEST(FieldCodec_EmptyEnumArraysSkipTheValueSet) {
    using codec_fixture::Shade;
    using codec_fixture::ShadeEnum;
    SystemAllocator allocator;
    const FieldSchema field = ArrayField(ValueKind::Enum, ShadeEnum.id);
    const uint64 filledWork = ShadeEnum.values.Size() + 3 * detail::EnumLookupWork(2);
    const usize counts[] = {0, 3};
    const bool phases[] = {false, true};
    for (usize count : counts) {
        Array<Shade> values(allocator);
        for (usize index = 0; index < count; ++index) {
            REQUIRE(values.TryPushBack(index == 1 ? Shade::Dark : Shade::Light));
        }

        TestEncodeContext encode(allocator, {});
        ArchiveWriter out(allocator);
        {
            FieldScope scope(encode, field, Guid{0x4f424a, 4}, &ShadeEnum);
            REQUIRE(detail::EncodeArrayField(values, out, encode));
        }

        CHECK(encode.spent == (count == 0 ? 0 : filledWork));
        CHECK(encode.spentBeforeElements == 0);
        const FieldView view{field.id, Wire(WireType::ValueArray), out.View()};
        for (bool apply : phases) {
            TestDecodeContext decode(allocator, {}, nullptr);
            if (apply) {
                decode.EnterApplyPhase();
            }

            Array<Shade> target(allocator);
            FieldScope scope(decode, field, Guid{0x4f424a, 4}, &ShadeEnum);
            REQUIRE(detail::DecodeArrayField(nullptr, ObjectHandle{}, apply ? &target : nullptr,
                                             view, decode));
            CHECK(decode.spent == (count == 0 ? 0 : filledWork));
            CHECK(decode.spentBeforeElements == 0);
            CHECK(decode.reserved == count * sizeof(Shade) + count + 1);
            CHECK(!apply || (target.Size() == count && (count == 0 || target[1] == Shade::Dark)));
        }
    }
}

// EnumValueSet의 판정은 스키마 규칙과 같다: 일반 enum(열거자 16개 이하는 선형, 17개 이상은 정렬
// 사본 이진 탐색 — 0 선언 여부 둘 다)과 Flags enum에서 Contains == IsDeclaredEnumValue, Replacement
// == EnumReplacementValue. Build 청구는 열거자 수(+ 정렬 사본이면 열거자 수 × EnumLookupWork)다.
TEST(FieldCodec_EnumValueSetMatchesSchemaRules) {
    SystemAllocator allocator;
    // 정렬되지 않은 열거자 20개(0 없음)와 17개(0 포함).
    EnumeratorSchema large[20] = {};
    for (usize index = 0; index < 20; ++index) {
        large[index].value = static_cast<int64>((index * 7) % 20) * 9 + 3;
    }

    EnumeratorSchema zero[17] = {};
    for (usize index = 0; index < 17; ++index) {
        zero[index].value = static_cast<int64>(16 - index) * 5;
    }

    const EnumSchema schemas[] = {
        {Guid{0x45, 1}, {}, ValueKind::UInt8, 0u, Span<const EnumeratorSchema>(large, 20), {}},
        {Guid{0x45, 2}, {}, ValueKind::Int32, 0u, Span<const EnumeratorSchema>(zero, 17), {}},
        codec_fixture::ShadeEnum,
        FlagsEnum};
    const uint64 expectedBuild[] = {20 + 20 * detail::EnumLookupWork(20),
                                    17 + 17 * detail::EnumLookupWork(17), 2, 2};
    for (usize which = 0; which < 4; ++which) {
        const EnumSchema& schema = schemas[which];
        TestDecodeContext context(allocator, {}, nullptr);
        auto set = detail::EnumValueSet::Build(schema, context);
        REQUIRE(set);
        CHECK(context.spent == expectedBuild[which]);
        CHECK(set.Value().LookupWork() == detail::EnumLookupWork(schema.values.Size()));
        bool same = true;
        for (int64 value = -300; value <= 300; ++value) {
            same = same && set.Value().Contains(value) == IsDeclaredEnumValue(schema, value) &&
                   set.Value().Replacement(value) == EnumReplacementValue(schema, value);
        }

        CHECK(same);
    }
}
