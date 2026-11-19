#include "AssetCombination.h"

#include <TestSupport/Test.h>

// "지원하는 모든 조합" 시험이다(Docs/Design/AssetSystem.ko.md 14.1절).
// - FixtureAsset_CombinationsCoverPropertyTable: fixture 클래스의 스키마를 순회해 리플렉션 성질
//   표(Meta/TypeSchema.h ValueKindProperties)가 허용하는 모든 (값 종류, 자리) 칸이 EditorOnly 실효
//   플래그 두 값 모두로 fixture 필드에 있는지, Required를 받는 종류 × 자리마다 Required 필드가,
//   검증 hook·참조 struct·Flags enum이 그 자리마다, 중첩 깊이 3 이상이 있는지 본다. 하나라도 빠지면
//   빠진 칸 이름으로 실패한다(표에 행·열이 생기면 fixture를 고치라는 신호).
// - FixtureAsset_CombinationRoundTrip: ComboAsset·ComboEditorAsset을 쿠킹·에디터 × seed 둘로 저장 →
//   다른 Registry에 로드 → 필드 단위 비교(참조는 대상 표의 같은 정체성, weak 밖 대상은 null, 쿠킹의
//   EditorOnly는 기본값) → 다시 저장해 byte 동일.
// - FixtureAsset_CombinationRequired: Required 칸 하나씩 null로 두면 저장이 거부하고, 모두 채우면
//   왕복한다.
using namespace rocks;
using namespace rocks::asset_test;

namespace {

// ---- 성질 표 순회 ----

// 순회가 보는 자리다(성질 표의 위치 성질 순서).
constexpr ValueKindProperty Slots[] = {ValueKindProperty::Field,
                                       ValueKindProperty::StructMember,
                                       ValueKindProperty::ArrayElement,
                                       ValueKindProperty::StaticArrayElement,
                                       ValueKindProperty::MapKey,
                                       ValueKindProperty::MapValue,
                                       ValueKindProperty::SetElement,
                                       ValueKindProperty::OptionalInner,
                                       ValueKindProperty::InstancedStructContent};

constexpr usize SlotCount = sizeof(Slots) / sizeof(Slots[0]);

// 자리가 아닌 성질이다(Required·대상 타입·대상 ID·컨테이너·통째 값).
constexpr ValueKindProperty NonSlots[] = {
    ValueKindProperty::AcceptsRequired, ValueKindProperty::HasTargetType,
    ValueKindProperty::RequiresTargetId, ValueKindProperty::Container,
    ValueKindProperty::WholeFieldValue};

template <usize Count>
constexpr uint32 BitsOf(const ValueKindProperty (&properties)[Count]) noexcept {
    uint32 bits = 0;
    for (ValueKindProperty property : properties) {
        bits |= static_cast<uint32>(property);
    }

    return bits;
}

// 표가 어느 종류에든 쓰는 성질 비트가 모두 Slots 또는 NonSlots로 분류되어 있다. 표에 새 자리(열)가
// 생기면 여기서 컴파일이 멈춘다 — Slots와 fixture에 그 자리를 더하라는 신호다.
constexpr bool TableBitsClassified() noexcept {
    const uint32 known = BitsOf(Slots) | BitsOf(NonSlots);
    for (uint32 kind = 0; kind < ValueKindCount; ++kind) {
        if ((ValueKindProperties(static_cast<ValueKind>(kind)) & ~known) != 0) {
            return false;
        }
    }

    return true;
}

static_assert((BitsOf(Slots) & BitsOf(NonSlots)) == 0,
              "a value kind property is both a slot and a non-slot property");
static_assert(TableBitsClassified(),
              "the value kind property table uses a property that is neither a slot nor a "
              "non-slot property; add the new slot to Slots and to the combination fixtures");

const char* SlotName(usize slot) noexcept {
    constexpr const char* Names[SlotCount] = {"Field",       "StructMember",  "ArrayElement",
                                              "StaticArray", "MapKey",        "MapValue",
                                              "SetElement",  "OptionalInner", "InstancedContent"};
    return Names[slot];
}

usize SlotIndex(ValueKindProperty property) noexcept {
    for (usize index = 0; index < SlotCount; ++index) {
        if (Slots[index] == property) {
            return index;
        }
    }

    return IndexNone;
}

// 컨테이너 종류의 원소 자리다.
ValueKindProperty ElementSlot(ValueKind container) noexcept {
    switch (container) {
    case ValueKind::Array:
        return ValueKindProperty::ArrayElement;
    case ValueKind::StaticArray:
        return ValueKindProperty::StaticArrayElement;
    case ValueKind::Map:
        return ValueKindProperty::MapValue;
    case ValueKind::Set:
        return ValueKindProperty::SetElement;
    case ValueKind::Optional:
        return ValueKindProperty::OptionalInner;
    default:
        return ValueKindProperty::None;
    }
}

// 순회 결과 표다.
struct Coverage {
    // [종류][자리][EditorOnly 실효]
    bool cell[ValueKindCount][SlotCount][2] = {};
    bool required[ValueKindCount][SlotCount] = {};
    bool validated[SlotCount] = {};
    bool referenceStruct[SlotCount] = {};
    bool flagsEnum[SlotCount] = {};
    uint32 maxStructDepth = 0;
};

void Mark(Coverage& coverage, ValueKind kind, ValueKindProperty slot, uint32 flags,
          bool required) noexcept {
    const usize index = SlotIndex(slot);
    if (index == IndexNone || static_cast<uint32>(kind) >= ValueKindCount) {
        return;
    }

    const bool editorOnly = (flags & static_cast<uint32>(FieldFlags::EditorOnly)) != 0;
    coverage.cell[static_cast<uint32>(kind)][index][editorOnly ? 1 : 0] = true;
    if (required) {
        coverage.required[static_cast<uint32>(kind)][index] = true;
    }
}

// 필드 목록 하나를 순회한다. struct면 view로 멤버 struct·enum을 찾고, 클래스면 lease로 찾는다.
template <class Owner>
void WalkFields(Coverage& coverage, const Owner& owner, bool isClass, uint32 inherited,
                uint32 depth) noexcept;

void WalkStruct(Coverage& coverage, StructView view, uint32 flags, uint32 depth) noexcept {
    if (depth > coverage.maxStructDepth) {
        coverage.maxStructDepth = depth;
    }

    WalkFields(coverage, view, false, flags, depth);
}

template <class Owner>
void WalkFields(Coverage& coverage, const Owner& owner, bool isClass, uint32 inherited,
                uint32 depth) noexcept {
    const TypeSchema& schema = owner.Schema();
    for (usize index = 0; index < schema.fields.Size(); ++index) {
        const FieldSchema& field = schema.fields[index];
        const uint32 flags = inherited | field.flags;
        const bool required = (field.flags & static_cast<uint32>(FieldFlags::Required)) != 0;
        const ValueKindProperty place =
            isClass ? ValueKindProperty::Field : ValueKindProperty::StructMember;
        const bool container = ValueKindIsContainer(field.value.kind);
        Mark(coverage, field.value.kind, place, flags, required && !container);
        ValueKindProperty targetSlot = place;
        if (container) {
            targetSlot = ElementSlot(field.value.kind);
            Mark(coverage, field.element.kind, targetSlot, flags, required);
            if (field.value.kind == ValueKind::Map) {
                Mark(coverage, field.key.kind, ValueKindProperty::MapKey, flags, false);
            }
        }

        const usize slotIndex = SlotIndex(targetSlot);
        const ValueType& target = FieldTargetType(field);
        if (target.kind == ValueKind::Enum) {
            const EnumSchema* enumeration = owner.FieldEnum(index);
            if (enumeration != nullptr &&
                (enumeration->flags & static_cast<uint32>(EnumFlags::Flags)) != 0) {
                coverage.flagsEnum[slotIndex] = true;
            }
        }

        if (field.value.kind == ValueKind::Map && field.key.kind == ValueKind::Enum) {
            const EnumSchema* key = owner.FieldKeyEnum(index);
            if (key != nullptr && (key->flags & static_cast<uint32>(EnumFlags::Flags)) != 0) {
                coverage.flagsEnum[SlotIndex(ValueKindProperty::MapKey)] = true;
            }
        }

        if (target.kind == ValueKind::Struct) {
            const StructView member = owner.FieldStruct(index);
            if (member.IsEmpty()) {
                continue;
            }

            if (member.StructOps().validate != nullptr) {
                coverage.validated[slotIndex] = true;
            }

            if (member.ContainsStrongReference()) {
                coverage.referenceStruct[slotIndex] = true;
            }

            WalkStruct(coverage, member, flags, depth + 1);
        }
    }
}

// 클래스와 그 기반(Asset·Object 위의 리플렉션 기반)을 순회한다.
bool WalkClass(Coverage& coverage, TypeRegistry& types, TypeId type) noexcept {
    for (TypeId cursor = type; !(cursor == TypeId{});) {
        auto lease = types.Acquire(cursor);
        if (!lease || lease.Value().Kind() != TypeKind::Class) {
            return false;
        }

        WalkFields(coverage, lease.Value(), true, 0, 0);
        cursor = lease.Value().Schema().baseType;
    }

    return true;
}

// 순회 대상이다: 조합 왕복 시험(FixtureAsset_CombinationRoundTrip·CombinationRequired)이
// 저장·로드하는 조합 클래스만이다. 다른 fixture(TestAsset 등)가 칸을 대신 채우지 않게 한다.
TypeId FixtureClasses[] = {IdOf<ComboAsset>(), IdOf<ComboEditorAsset>(), IdOf<ComboRequiredAsset>(),
                           IdOf<ComboPart>(), IdOf<ComboPartDerived>()};

// 빠진 칸 하나를 실패로 남긴다(실패 문구 = 칸 이름).
void Missing(String& text, const char* what, uint32 kind, usize slot) noexcept {
    text.Clear();
    char number[12];
    usize count = 0;
    uint32 value = kind;
    do {
        number[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);

    char digits[12];
    for (usize index = 0; index < count; ++index) {
        digits[index] = number[count - 1 - index];
    }

    (void)text.TryAppend(StringView(what));
    (void)text.TryAppend(StringView(" kind="));
    (void)text.TryAppend(StringView(digits, count));
    (void)text.TryAppend(StringView(" slot="));
    (void)text.TryAppend(StringView(SlotName(slot)));
    test::RecordFailure(text.CStr(), __FILE__, __LINE__);
}

} // namespace

// 성질 표가 허용하는 모든 칸(+ Required·검증 hook·참조 struct·Flags enum·중첩 깊이)이 fixture에
// 있다. InstancedStruct 내용 열(Struct만)은 실행 중 값이라 스키마에 없다 — InstancedStruct 칸이
// 있고 왕복 시험이 ComboPayload·ComboChecked 내용을 비교하는 것으로 덮는다(아래 마지막 검사).
TEST(FixtureAsset_CombinationsCoverPropertyTable) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Coverage coverage;
    for (TypeId type : FixtureClasses) {
        CHECK(WalkClass(coverage, env.types, type));
    }

    String text(env.allocator);
    uint32 missing = 0;
    for (uint32 kind = 0; kind < ValueKindCount; ++kind) {
        const ValueKind value = static_cast<ValueKind>(kind);
        for (usize slot = 0; slot < SlotCount; ++slot) {
            if (Slots[slot] == ValueKindProperty::InstancedStructContent ||
                !HasValueKindProperty(value, Slots[slot])) {
                continue;
            }

            for (uint32 editor = 0; editor < 2; ++editor) {
                if (!coverage.cell[kind][slot][editor]) {
                    ++missing;
                    Missing(text, editor == 0 ? "missing cell" : "missing EditorOnly cell", kind,
                            slot);
                }
            }

            // Required는 값마다 non-null이다 — Map 키에는 Required가 없다(FieldSchema::key 주석).
            if (ValueKindAcceptsRequired(value) && Slots[slot] != ValueKindProperty::MapKey &&
                !coverage.required[kind][slot]) {
                ++missing;
                Missing(text, "missing Required cell", kind, slot);
            }
        }
    }

    // 값 struct가 놓이는 자리(표의 Struct 행 — InstancedStruct 내용은 아래)마다 검증 hook, 참조
    // struct가 놓이는 자리마다 참조 struct. 참조 struct는 표 밖 규칙으로 Map 값·Optional 내부에 둘
    // 수 없다(강한 참조를 담은 struct — ReflectionExtensions 1.3절).
    uint32 structSlots = 0;
    uint32 referenceSlots = 0;
    for (usize slot = 0; slot < SlotCount; ++slot) {
        if (Slots[slot] == ValueKindProperty::InstancedStructContent ||
            !HasValueKindProperty(ValueKind::Struct, Slots[slot])) {
            continue;
        }

        ++structSlots;
        if (!coverage.validated[slot]) {
            ++missing;
            Missing(text, "missing Validate hook", static_cast<uint32>(ValueKind::Struct), slot);
        }

        if (Slots[slot] == ValueKindProperty::MapValue ||
            Slots[slot] == ValueKindProperty::OptionalInner) {
            continue;
        }

        ++referenceSlots;
        if (!coverage.referenceStruct[slot]) {
            ++missing;
            Missing(text, "missing reference struct", static_cast<uint32>(ValueKind::Struct), slot);
        }
    }

    CHECK(structSlots == 6);
    CHECK(referenceSlots == 4);

    for (usize slot = 0; slot < SlotCount; ++slot) {
        if (Slots[slot] != ValueKindProperty::InstancedStructContent &&
            HasValueKindProperty(ValueKind::Enum, Slots[slot]) && !coverage.flagsEnum[slot]) {
            ++missing;
            Missing(text, "missing Flags enum", static_cast<uint32>(ValueKind::Enum), slot);
        }
    }

    CHECK(coverage.maxStructDepth >= 3);
    CHECK(missing == 0);
    // InstancedStruct 내용 열: Struct만 ○이고, 그 칸은 InstancedStruct 필드가 실제로 struct를 담아
    // 왕복하는 것으로 덮는다(ComboInstanced — ComboPayload·검증 hook ComboChecked).
    CHECK(HasValueKindProperty(ValueKind::Struct, ValueKindProperty::InstancedStructContent));
    for (uint32 kind = 0; kind < ValueKindCount; ++kind) {
        CHECK(HasValueKindProperty(static_cast<ValueKind>(kind),
                                   ValueKindProperty::InstancedStructContent) ==
              (static_cast<ValueKind>(kind) == ValueKind::Struct));
    }

    CHECK(coverage.cell[static_cast<uint32>(ValueKind::InstancedStruct)]
                       [SlotIndex(ValueKindProperty::StructMember)][0]);
}
