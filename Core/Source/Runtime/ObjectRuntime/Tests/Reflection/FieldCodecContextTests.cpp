#include <Core/Memory/SystemAllocator.h>
#include <Object/GeneratedFieldCodec.h>
#include <Serialization/Archive.h>
#include <Serialization/Record.h>
#include <TestSupport/Test.h>

// 공용 codec(EncodeFieldWire·ParseFieldValue)을 직렬화기 없이 시험한다(Docs/Design/
// ReflectionFoundations.ko.md 6.4절): 진단 받을 곳이 없으면 ReportAndDefault도 거부(CanReport),
// 값 검사 순서(wire 종류 TypeMismatch → 정수 폭 Overflow → 그 밖과 trailing byte InvalidArgument),
// 등록 enum 검증 작업량의 청구.
using namespace rocks;

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

    Result<void> AddFields(usize) noexcept override {
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

    uint64 spent = 0;
    uint64 reserved = 0;

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

    Result<void> AddFields(usize) noexcept override {
        return Result<void>::Success();
    }

    RecordLimits NestedRecordLimits() const noexcept override {
        return {};
    }

    uint64 spent = 0;

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
