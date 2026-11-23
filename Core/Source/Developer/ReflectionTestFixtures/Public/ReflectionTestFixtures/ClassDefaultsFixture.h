#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/OrderedMap.h>
#include <Core/Containers/OrderedSet.h>
#include <Core/Optional.h>
#include <Core/Text/Name.h>
#include <Core/Text/String.h>
#include <Localization/Text.h>
#include <Meta/Annotations.h>
#include <Object/AssetId.h>
#include <Object/ClassRef.h>
#include <Object/InstancedStruct.h>
#include <Object/Object.h>
#include <Object/ObjectRefs.h>
#include <Object/SoftObjectRef.h>

namespace rocks::reflection_fixture {

// 클래스 기본값 레코드 fixture다(Docs/Design/RuntimeClassMetadata.ko.md 'A5a 레코드' 시험 —
// ClassDefaultsTests). DefaultsHolder는 값 종류·자리·플래그를 두루 담고(생성자가 0이 아닌 기본값을
// 넣는다), DefaultsVirtual은 가상 함수가 있는 파생(저장소 시작 ≠ Object 기반일 수 있다)이다.
// DefaultsCounter는 생성마다 다른 값(비결정 생성자), DefaultsSoftLink는 non-null Soft 기본값,
// DefaultsAbstractBase는 추상이다.
ROCKS_ENUM()
enum class DefaultsMode : uint8 {
    First = 0,
    Second = 1,
    Third = 2,
};

// 비Atomic struct(멤버 단위 — 경로 조회가 멤버까지 내려간다)다. 안에 EditorOnly 멤버가 있다.
ROCKS_STRUCT()
struct DefaultsPoint {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() int32 x = 3;
    ROCKS_PROPERTY() float y = 1.5f;
    ROCKS_PROPERTY(EditorOnly) int32 note = 4;
};

// Atomic struct(통째 단위 — 멤버 경로는 InvalidArgument)다.
ROCKS_STRUCT(Atomic)
struct DefaultsPair {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() int32 u = 7;
    ROCKS_PROPERTY() int32 v = 8;
};

// 두 단계 struct(멤버 사슬 경로)다.
ROCKS_STRUCT()
struct DefaultsNest {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() DefaultsPoint inner;
    ROCKS_PROPERTY() int32 depth = 2;
};

ROCKS_CLASS(Abstract)
struct DefaultsAbstractBase : Object {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() int32 baseValue = 1;
};

ROCKS_CLASS()
struct DefaultsHolder : DefaultsAbstractBase {
    ROCKS_GENERATED_BODY()

    explicit DefaultsHolder(Allocator& allocator) noexcept
        : label(allocator), values(allocator), table(allocator), tags(allocator), soft(allocator) {
    }

    ROCKS_PROPERTY() int32 count = 42;
    ROCKS_PROPERTY() float scale = 2.5f;
    ROCKS_PROPERTY() double precise = -0.0;
    ROCKS_PROPERTY() bool enabled = true;
    ROCKS_PROPERTY() DefaultsMode mode = DefaultsMode::Second;
    ROCKS_PROPERTY() String label;
    ROCKS_PROPERTY() DefaultsPoint point;
    ROCKS_PROPERTY() DefaultsPair pair;
    ROCKS_PROPERTY() DefaultsNest nest;
    ROCKS_PROPERTY() Array<int32> values;
    ROCKS_PROPERTY() OrderedMap<int32, int32> table;
    ROCKS_PROPERTY() OrderedSet<int32> tags;
    ROCKS_PROPERTY() Optional<int32> maybe = 9;
    ROCKS_PROPERTY() int32 fixed[3] = { 1, 2, 3 };
    ROCKS_PROPERTY() Text caption;
    ROCKS_PROPERTY() InstancedStruct<> payload;
    ROCKS_PROPERTY() Name tag;
    ROCKS_PROPERTY() ClassRef<DefaultsAbstractBase> kind;
    ROCKS_PROPERTY() ObjectRef<Object> link;
    ROCKS_PROPERTY() WeakObjectRef<Object> weak;
    ROCKS_PROPERTY() SoftObjectRef<Object> soft;
    ROCKS_PROPERTY(EditorOnly) int32 editorValue = 11;
    ROCKS_PROPERTY(EditorOnly) DefaultsPoint editorPoint;
    ROCKS_PROPERTY(Transient) int32 scratch = 12;
    ROCKS_PROPERTY() uint8 lit : 1 = 1;
    ROCKS_PROPERTY() uint8 dim : 1 = 0;
};

// 가상 함수가 있는 파생이다(MSVC ABI에서 vfptr 뒤에 Object 기반 — 저장소 시작을 반환하는지 본다).
// final이 아니다(final 클래스의 새 virtual은 Clang -Wunnecessary-virtual-specifier).
ROCKS_CLASS()
struct DefaultsVirtual : DefaultsHolder {
    ROCKS_GENERATED_BODY()

    using DefaultsHolder::DefaultsHolder;

    virtual ~DefaultsVirtual() = default;

    virtual int32 Extra() const noexcept {
        return extra;
    }

    ROCKS_PROPERTY() int32 extra = 99;
};

// 생성마다 다른 값을 넣는다(생성자 결정성 계약 위반 — NonDeterministic).
ROCKS_CLASS()
struct DefaultsCounter final : Object {
    ROCKS_GENERATED_BODY()

    DefaultsCounter() noexcept : serial(++created) {
    }

    // 만든 수다. 정의는 Private/FixtureState.cpp다(공개 헤더에 쓰기 가능한 정적 저장소를 두지
    // 않는다 — BuildTargets BD29).
    ROCKS_REFLECTIONTESTFIXTURES_API static int32 created;

    ROCKS_PROPERTY() int32 stable = 5;
    ROCKS_PROPERTY() int32 serial = 0;
};

// non-null Soft 기본값이다(참조 기본값은 null만 — NonNullReference). 경로가 빈 값이라 할당하지
// 않는다.
ROCKS_CLASS()
struct DefaultsSoftLink final : Object {
    ROCKS_GENERATED_BODY()

    explicit DefaultsSoftLink(Allocator& allocator) noexcept : target(allocator) {
        (void)target.TryAssign(AssetId{Guid{0x5DEF, 0x1}}, StringView());
    }

    ROCKS_PROPERTY() int32 before = 1;
    ROCKS_PROPERTY() SoftObjectRef<Object> target;
};

// Soft 멤버 struct다(struct 안 non-null Soft 기본값 — 경로 slot.target).
ROCKS_STRUCT()
struct DefaultsSoftSlot {
    ROCKS_GENERATED_BODY()

    explicit DefaultsSoftSlot(Allocator& allocator) noexcept : target(allocator) {
    }

    ROCKS_PROPERTY() SoftObjectRef<Object> target;
};

ROCKS_CLASS()
struct DefaultsSoftNested final : Object {
    ROCKS_GENERATED_BODY()

    explicit DefaultsSoftNested(Allocator& allocator) noexcept : slot(allocator) {
        (void)slot.target.TryAssign(AssetId{Guid{0x5DEF, 0x2}}, StringView());
    }

    ROCKS_PROPERTY() DefaultsSoftSlot slot;
};

// 배열 원소 non-null Soft 기본값이다(경로 첫 단계 list).
ROCKS_CLASS()
struct DefaultsSoftList final : Object {
    ROCKS_GENERATED_BODY()

    explicit DefaultsSoftList(Allocator& allocator) noexcept : list(allocator) {
        SoftObjectRef<Object> one(allocator);
        if (one.TryAssign(AssetId{Guid{0x5DEF, 0x3}}, StringView())) {
            (void)list.TryPushBack(Move(one));
        }
    }

    ROCKS_PROPERTY() Array<SoftObjectRef<Object>> list;
};

// non-null ClassRef 기본값이다: pick은 등록된 파생 DefaultsHolder(생성자는 생성 헤더가 보이는
// Private/ClassDefaultsFixture.cpp — ClassRef::Of), stranger는 시험이 나중에 등록하는
// DefaultsAbstractBase 파생 손 타입 id(DefaultsStrangerType — 미등록이면 NotFound이고 기억하지
// 않는다, 등록 뒤 성공. ClassRef base는 리플렉션 클래스여야 한다 — RMC2003).
inline constexpr TypeId DefaultsStrangerType{0x4D414E55414C, 1};

ROCKS_CLASS()
struct DefaultsKindPick final : Object {
    ROCKS_GENERATED_BODY()

    ROCKS_REFLECTIONTESTFIXTURES_API DefaultsKindPick() noexcept;

    ROCKS_PROPERTY() ClassRef<DefaultsAbstractBase> pick;
};

ROCKS_CLASS()
struct DefaultsStrangerKind final : Object {
    ROCKS_GENERATED_BODY()

    DefaultsStrangerKind() noexcept : stranger(DefaultsStrangerType) {
    }

    ROCKS_PROPERTY() ClassRef<DefaultsAbstractBase> stranger;
};

} // namespace rocks::reflection_fixture
