#include <Core/Memory/SystemAllocator.h>
#include <Object/TypeRegistry.h>
#include <TestSupport/Test.h>

// 필드 쓰기 검증의 한 곳(detail::ValidateFieldWrite)과 codec 도우미의 단위 시험이다(Docs/Design/
// ReflectionFoundations.ko.md 3.6·5.6·6.4절): 오류 순서(TypeMismatch → 읽기 전용 → 값 내용), 유효
// 플래그로 전파된 읽기 전용, 실제 대상 enum 스키마 기준 판정, 원소 모드와 ValidateArrayLength,
// PropagateFieldFlags·EnumReplacementValue의 규칙.
using namespace rocks;

namespace {

constexpr uint32 Flag(FieldFlags flag) noexcept {
    return static_cast<uint32>(flag);
}

constexpr EnumeratorSchema FlagValues[] = {{{}, {}, {}, 1, 0}, {{}, {}, {}, 4, 0}};
constexpr EnumSchema FlagsEnum{
    Guid{0x464c41, 1}, {}, ValueKind::UInt8, 1u, Span<const EnumeratorSchema>(FlagValues, 2), {}};
constexpr EnumeratorSchema PlainValues[] = {{{}, {}, {}, 3, 0}, {{}, {}, {}, 9, 0}};
constexpr EnumSchema PlainEnum{
    Guid{0x504c41, 1}, {}, ValueKind::Int32, 0u, Span<const EnumeratorSchema>(PlainValues, 2), {}};

// 바깥 EditorOnly·Transient·ReadOnly만 전파되고 Required는 전파되지 않는다.
static_assert(PropagateFieldFlags(Flag(FieldFlags::EditorOnly) | Flag(FieldFlags::Required),
                                  Flag(FieldFlags::Transient)) ==
              (Flag(FieldFlags::EditorOnly) | Flag(FieldFlags::Transient)));
static_assert(PropagateFieldFlags(Flag(FieldFlags::ReadOnly), 0) == Flag(FieldFlags::ReadOnly));
static_assert(EnumDeclaredMask(FlagsEnum) == 5);
// Flags enum은 범위 안이면 선언되지 않은 비트만 지우고, 범위 밖·음수면 0이다.
static_assert(EnumReplacementValue(FlagsEnum, 7) == 5);
static_assert(EnumReplacementValue(FlagsEnum, -1) == 0);
static_assert(EnumReplacementValue(FlagsEnum, 256) == 0);
// 0이 선언되지 않은 일반 enum은 첫 열거자다.
static_assert(EnumReplacementValue(PlainEnum, 5) == 3);
static_assert(IsDeclaredEnumValue(FlagsEnum, EnumReplacementValue(FlagsEnum, 255)));

FieldSchema Field(ValueKind kind, TypeId type = {}, uint32 flags = 0) noexcept {
    FieldSchema field;
    field.id = Guid{0x46494c, 1};
    field.kind = kind == ValueKind::ObjectReference ? FieldKind::ObjectReference : FieldKind::Value;
    field.flags = flags;
    field.value = ValueType{kind, type};
    return field;
}

} // namespace

TEST(FieldWriteValidation_OrderAndRules) {
    SystemAllocator allocator;
    const FieldSchema count = Field(ValueKind::Int32);
    detail::FieldWriteBinding binding;
    binding.value = count.value;
    detail::FieldWriteTarget target;
    target.actual = &count;
    target.effectiveFlags = count.flags;
    FieldValue value(allocator);
    value.kind = ValueKind::Int32;
    CHECK(detail::ValidateFieldWrite(binding, target, value));

    // 전달 값 종류가 다르면 TypeMismatch, 대상 필드가 없으면(계약 위반) TypeMismatch다.
    value.kind = ValueKind::Int64;
    CHECK(detail::ValidateFieldWrite(binding, target, value).Error() == ErrorCode::TypeMismatch);
    value.kind = ValueKind::Int32;
    detail::FieldWriteTarget missing;
    CHECK(detail::ValidateFieldWrite(binding, missing, value).Error() == ErrorCode::TypeMismatch);

    // 유효 플래그로 전파된 읽기 전용(B6 경로 쓰기의 바깥 ReadOnly)도 Unsupported다.
    target.effectiveFlags = Flag(FieldFlags::ReadOnly);
    CHECK(detail::ValidateFieldWrite(binding, target, value).Error() == ErrorCode::Unsupported);
    target.effectiveFlags = 0;
    binding.readOnly = true;
    CHECK(detail::ValidateFieldWrite(binding, target, value).Error() == ErrorCode::Unsupported);
}

TEST(FieldWriteValidation_ReferenceRules) {
    SystemAllocator allocator;
    const FieldSchema edge = Field(ValueKind::ObjectReference);
    detail::FieldWriteBinding binding;
    binding.value = edge.value;
    detail::FieldWriteTarget target;
    target.actual = &edge;
    FieldValue value(allocator);
    value.kind = ValueKind::ObjectReference;
    CHECK(detail::ValidateFieldWrite(binding, target, value));

    // 원본·대상 기대 타입의 native가 다르면 TypeMismatch다(별칭은 같은 native).
    static uint8 sourceKey = 0;
    static uint8 otherKey = 0;
    target.sourceExpectedNative = &sourceKey;
    target.targetExpectedNative = &otherKey;
    CHECK(detail::ValidateFieldWrite(binding, target, value).Error() == ErrorCode::TypeMismatch);
    target.targetExpectedNative = &sourceKey;
    CHECK(detail::ValidateFieldWrite(binding, target, value));

    // non-null 참조인데 대상 lease가 없으면 TypeMismatch다.
    value.reference = ObjectHandle{1, 1, 1};
    CHECK(detail::ValidateFieldWrite(binding, target, value).Error() == ErrorCode::TypeMismatch);

    // null은 받는다(B5 — null 금지 단계가 없다). 읽기 전용이면 Unsupported다.
    value.reference = ObjectHandle{};
    CHECK(detail::ValidateFieldWrite(binding, target, value));
    binding.readOnly = true;
    CHECK(detail::ValidateFieldWrite(binding, target, value).Error() == ErrorCode::Unsupported);
}

// 원소 모드(배열 필드의 원소 하나 쓰기 — B6 경로 쓰기가 쓴다): actual은 Array여야 하고 binding을
// 원소 형식과 대조하며, ObjectRefArray 원소도 null을 받는다. ValidateArrayLength는 Array가 아니면
// TypeMismatch, maxElements를 넘으면 CapacityExceeded다.
TEST(FieldWriteValidation_ElementModeAndArrayLength) {
    SystemAllocator allocator;
    FieldSchema slots = Field(ValueKind::Array);
    slots.kind = FieldKind::ObjectReference;
    slots.element = ValueType{ValueKind::ObjectReference, {}};
    slots.maxElements = 4;
    detail::FieldWriteBinding binding;
    binding.value = slots.element;
    detail::FieldWriteTarget target;
    target.actual = &slots;
    FieldValue value(allocator);
    value.kind = ValueKind::ObjectReference;
    // 원소 모드가 아니면 Array 필드와 참조 binding은 TypeMismatch다.
    CHECK(detail::ValidateFieldWrite(binding, target, value).Error() == ErrorCode::TypeMismatch);
    target.element = true;
    CHECK(detail::ValidateFieldWrite(binding, target, value));
    // 원소 모드인데 actual이 Array가 아니면 TypeMismatch다.
    const FieldSchema single = Field(ValueKind::ObjectReference);
    target.actual = &single;
    CHECK(detail::ValidateFieldWrite(binding, target, value).Error() == ErrorCode::TypeMismatch);

    // enum 원소: 원소 enum ID와 원소 enum 스키마(TypeLease::FieldEnum이 돌려주는 값)로 판정한다.
    FieldSchema channels = Field(ValueKind::Array);
    channels.element = ValueType{ValueKind::Enum, FlagsEnum.id};
    channels.maxElements = 8;
    binding.value = channels.element;
    target.actual = &channels;
    target.actualEnum = &FlagsEnum;
    FieldValue flags(allocator);
    flags.kind = ValueKind::Enum;
    flags.type = FlagsEnum.id;
    flags.signedInteger = 5;
    CHECK(detail::ValidateFieldWrite(binding, target, flags));
    flags.signedInteger = 2;
    CHECK(detail::ValidateFieldWrite(binding, target, flags).Error() == ErrorCode::InvalidArgument);
    flags.type = PlainEnum.id;
    CHECK(detail::ValidateFieldWrite(binding, target, flags).Error() == ErrorCode::TypeMismatch);

    CHECK(detail::ValidateArrayLength(channels, 8));
    CHECK(detail::ValidateArrayLength(channels, 9).Error() == ErrorCode::CapacityExceeded);
    CHECK(detail::ValidateArrayLength(single, 0).Error() == ErrorCode::TypeMismatch);
}

// Struct·Array 종류의 통째 쓰기는 종류 대조(TypeMismatch) 뒤 Unsupported다 — 경로 쓰기가 이 함수만
// 거쳐도 struct 값·배열·struct 원소를 FieldValue로 통째 쓰지 못한다. 읽기 전용보다 먼저다.
TEST(FieldWriteValidation_AggregateKindsAreUnsupported) {
    SystemAllocator allocator;
    const FieldSchema holder = Field(ValueKind::Struct, Guid{0x535452, 1});
    detail::FieldWriteBinding binding;
    binding.value = holder.value;
    binding.readOnly = true;
    detail::FieldWriteTarget target;
    target.actual = &holder;
    FieldValue value(allocator);
    value.kind = ValueKind::Int32;
    CHECK(detail::ValidateFieldWrite(binding, target, value).Error() == ErrorCode::TypeMismatch);
    value.kind = ValueKind::Struct;
    CHECK(detail::ValidateFieldWrite(binding, target, value).Error() == ErrorCode::Unsupported);

    FieldSchema items = Field(ValueKind::Array);
    items.element = ValueType{ValueKind::UInt32, {}};
    items.maxElements = 4;
    binding.value = items.value;
    binding.readOnly = false;
    target.actual = &items;
    value.kind = ValueKind::Array;
    CHECK(detail::ValidateFieldWrite(binding, target, value).Error() == ErrorCode::Unsupported);
    value.kind = ValueKind::UInt32;
    CHECK(detail::ValidateFieldWrite(binding, target, value).Error() == ErrorCode::TypeMismatch);

    // 원소 모드의 struct 원소도 통째로 쓰지 않는다.
    FieldSchema slots = Field(ValueKind::Array);
    slots.element = ValueType{ValueKind::Struct, Guid{0x535452, 2}};
    slots.maxElements = 4;
    binding.value = slots.element;
    target.actual = &slots;
    target.element = true;
    value.kind = ValueKind::Struct;
    CHECK(detail::ValidateFieldWrite(binding, target, value).Error() == ErrorCode::Unsupported);
}

TEST(FieldWriteValidation_ContentRulesUseTheActualEnum) {
    SystemAllocator allocator;
    const FieldSchema flags = Field(ValueKind::Enum, FlagsEnum.id);
    detail::FieldWriteBinding binding;
    binding.value = flags.value;
    detail::FieldWriteTarget target;
    target.actual = &flags;
    target.actualEnum = &FlagsEnum;
    FieldValue value(allocator);
    value.kind = ValueKind::Enum;
    value.type = FlagsEnum.id;
    value.signedInteger = 5;
    CHECK(detail::ValidateFieldWrite(binding, target, value));
    value.signedInteger = 2;
    CHECK(detail::ValidateFieldWrite(binding, target, value).Error() == ErrorCode::InvalidArgument);
    // enum ID가 다르면 내용보다 먼저 TypeMismatch, 실제 enum 스키마가 없으면 InvalidArgument다.
    value.type = PlainEnum.id;
    CHECK(detail::ValidateFieldWrite(binding, target, value).Error() == ErrorCode::TypeMismatch);
    value.type = FlagsEnum.id;
    value.signedInteger = 5;
    target.actualEnum = nullptr;
    CHECK(detail::ValidateFieldWrite(binding, target, value).Error() == ErrorCode::InvalidArgument);

    // 소프트 참조는 null 규칙을 읽기 전용 다음에 검사한다.
    const FieldSchema soft = Field(ValueKind::SoftObjectReference);
    binding.value = soft.value;
    target.actual = &soft;
    FieldValue orphan(allocator);
    orphan.kind = ValueKind::SoftObjectReference;
    REQUIRE(orphan.string.TryAssign("Orphan"));
    CHECK(detail::ValidateFieldWrite(binding, target, orphan).Error() ==
          ErrorCode::InvalidArgument);
    binding.readOnly = true;
    CHECK(detail::ValidateFieldWrite(binding, target, orphan).Error() == ErrorCode::Unsupported);
}
