#include <Core/Memory/SystemAllocator.h>
#include <Object/ClassRef.h>
#include <Object/Object.h>
#include <Object/TypeRegistry.h>
#include <TestSupport/Test.h>

// 리플렉션 B9d2의 TypeRegistry 시험이다(Docs/Design/ReflectionExtensions.ko.md 5.2절): 타입 도구
// 이름(TypeSchema::toolName)의 문법·대소문자 무시 유일성(클래스와 struct가 한 이름 공간)·색인
// 조회와 해제 뒤 재사용·색인 성장, ClassReference 필드의 등록 규칙(Base ID 필수, Base는 의존으로
// 붙잡지 않는다 — 자기 자신·나중 클래스를 Base로 둘 수 있다).
using namespace rocks;

namespace {

static_assert(IsTypeToolName(Span<const char>("Texture2D", 9)));
static_assert(IsTypeToolName(Span<const char>("Weapons.Sword", 13)));
static_assert(IsTypeToolName(Span<const char>("_a._b1", 6)));
static_assert(!IsTypeToolName(Span<const char>()));
static_assert(!IsTypeToolName(Span<const char>("1abc", 4)));
static_assert(!IsTypeToolName(Span<const char>("a..b", 4)));
static_assert(!IsTypeToolName(Span<const char>(".a", 2)));
static_assert(!IsTypeToolName(Span<const char>("a.", 2)));
static_assert(!IsTypeToolName(Span<const char>("a:b", 3)));
static_assert(!IsTypeToolName(Span<const char>("a.1b", 4)));
static_assert(!IsTypeToolName(Span<const char>("none", 4)));
static_assert(!IsTypeToolName(Span<const char>("NoNe", 4)));
static_assert(IsTypeToolName(Span<const char>("none1", 5)));
static_assert(IsTypeToolName(Span<const char>("Game.none", 9)));

struct Holder final : Object {};

uint8 StructKeys[64] = {};

void Construct(void*, Allocator&) noexcept {
}

void Destroy(void*) noexcept {
}

Result<uint32> MemberEncode(const RuntimeStructFieldOps&, const void*, ArchiveWriter&,
                            FieldEncodeContext&) noexcept {
    return Result<uint32>::Failure(ErrorCode::Unsupported);
}

Result<void> MemberDecode(const RuntimeStructFieldOps&, ConstructionContext*, ObjectHandle, void*,
                          const FieldView&, FieldDecodeContext&) noexcept {
    return Result<void>::Failure(ErrorCode::Unsupported);
}

Span<const char> Chars(const char* text) noexcept {
    return Span<const char>(text, StringView(text).ByteCount());
}

// 필드 없는 클래스(같은 native 타입의 별칭 ID — 등록은 받는다)를 tool 이름으로 등록한다.
Result<TypeRegistrationToken> RegisterClass(TypeRegistry& types, TypeId id,
                                            Span<const char> toolName,
                                            Span<const FieldSchema> fields = {}) noexcept {
    TypeSchema schema{id, 1, fields, Chars("Holder")};
    schema.toolName = toolName;
    return types.Register(schema, MakeRuntimeTypeOps<Holder>());
}

// uint32 멤버 하나짜리 struct를 tool 이름으로 등록한다(native 키는 key번째 표지).
Result<TypeRegistrationToken> RegisterStruct(TypeRegistry& types, TypeId id,
                                             Span<const char> toolName, usize key) noexcept {
    static FieldSchema member = [] {
        FieldSchema field;
        field.id = Guid{0x54544e, 0xf1};
        field.name = Span<const char>("value", 5);
        field.value = ValueType{ValueKind::UInt32, {}};
        return field;
    }();
    RuntimeStructFieldOps ops;
    ops.fieldId = member.id;
    ops.encode = &MemberEncode;
    ops.decode = &MemberDecode;
    RuntimeStructOps structOps;
    structOps.size = 4;
    structOps.alignment = 4;
    structOps.nativeType = &StructKeys[key];
    structOps.construct = &Construct;
    structOps.destroy = &Destroy;
    structOps.fields = Span<const RuntimeStructFieldOps>(&ops, 1);
    TypeSchema schema{id, 1, Span<const FieldSchema>(&member, 1), Chars("S")};
    schema.toolName = toolName;
    return types.RegisterStruct(schema, structOps);
}

ErrorCode ErrorOf(const Result<TypeRegistrationToken>& result) noexcept {
    return result ? ErrorCode::None : result.Error();
}

} // namespace

// 타입 도구 이름은 비어 있거나 문법(IsTypeToolName — `none` 예약)에 맞아야 하고(InvalidArgument),
// 클래스·struct 사이에서 대소문자 무시로 유일하다(AlreadyExists — 실패한 등록은 상태를 바꾸지
// 않는다). 빈 이름(수동 등록)은 색인에 넣지 않는다. 해제하면 그 이름을 다시 쓸 수 있다.
TEST(TypeToolName_RegistrationRulesAndLookup) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    const char* invalid[] = {"a..b", ".a", "1a", "none", "NONE", "a:b", "a b"};
    for (const char* name : invalid) {
        CHECK(ErrorOf(RegisterClass(types, {0x54544e, 1}, Chars(name))) ==
              ErrorCode::InvalidArgument);
        CHECK(ErrorOf(RegisterStruct(types, {0x54544e, 2}, Chars(name), 0)) ==
              ErrorCode::InvalidArgument);
    }

    auto sword = RegisterClass(types, {0x54544e, 3}, Chars("Weapons.Sword"));
    REQUIRE(sword);
    CHECK(ErrorOf(RegisterClass(types, {0x54544e, 4}, Chars("weapons.SWORD"))) ==
          ErrorCode::AlreadyExists);
    CHECK(ErrorOf(RegisterStruct(types, {0x54544e, 5}, Chars("WEAPONS.sword"), 1)) ==
          ErrorCode::AlreadyExists);
    REQUIRE(RegisterStruct(types, {0x54544e, 6}, Chars("Weapons.Shield"), 2));
    REQUIRE(RegisterClass(types, {0x54544e, 7}, {}));
    REQUIRE(RegisterClass(types, {0x54544e, 8}, {}));
    CHECK(types.Stats().registeredTypes == 4);
    {
        auto found = types.AcquireByToolName(Chars("WEAPONS.SWORD"));
        REQUIRE(found);
        CHECK((found.Value().Id() == TypeId{0x54544e, 3}) &&
              found.Value().Kind() == TypeKind::Class);
        // lease가 있는 동안 해제는 Busy다.
        CHECK(types.Unregister(sword.Value()).Error() == ErrorCode::Busy);
        auto shield = types.AcquireByToolName(Chars("weapons.shield"));
        REQUIRE(shield);
        CHECK(shield.Value().Kind() == TypeKind::Struct);
        CHECK(types.AcquireByToolName(Chars("Holder")).Error() == ErrorCode::NotFound);
        CHECK(types.AcquireByToolName({}).Error() == ErrorCode::InvalidArgument);
    }

    REQUIRE(types.Unregister(sword.Value()));
    CHECK(types.AcquireByToolName(Chars("Weapons.Sword")).Error() == ErrorCode::NotFound);
    auto reused = RegisterStruct(types, {0x54544e, 9}, Chars("weapons.sword"), 3);
    REQUIRE(reused);
    {
        auto found = types.AcquireByToolName(Chars("Weapons.Sword"));
        REQUIRE(found);
        CHECK((found.Value().Id() == TypeId{0x54544e, 9}));
    }

    CHECK(types.Shutdown());
    CHECK(types.AcquireByToolName(Chars("Weapons.Shield")).Error() == ErrorCode::ShuttingDown);
}

// 색인은 등록이 늘 때 새 색인으로 자라고 해제가 남긴 칸은 다음 등록이 정리한다: 많은 이름을 등록·
// 해제·재등록해도 모든 이름이 제 레코드를 찾고 해제한 이름은 찾지 않는다.
TEST(TypeToolName_IndexSurvivesGrowthAndErasure) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    constexpr uint32 Count = 40;
    char names[Count][8] = {};
    TypeRegistrationToken tokens[Count] = {};
    for (uint32 index = 0; index < Count; ++index) {
        names[index][0] = 'T';
        names[index][1] = static_cast<char>('a' + index / 26);
        names[index][2] = static_cast<char>('a' + index % 26);
        auto token = RegisterClass(types, {0x54544f, index + 1}, Span<const char>(names[index], 3));
        REQUIRE(token);
        tokens[index] = token.Value();
    }

    for (uint32 index = 0; index < Count; index += 2) {
        REQUIRE(types.Unregister(tokens[index]));
    }

    for (uint32 index = 0; index < Count; index += 4) {
        auto token =
            RegisterClass(types, {0x54544f, 100 + index}, Span<const char>(names[index], 3));
        REQUIRE(token);
        tokens[index] = token.Value();
    }

    for (uint32 index = 0; index < Count; ++index) {
        auto found = types.AcquireByToolName(Span<const char>(names[index], 3));
        if (index % 4 == 0) {
            REQUIRE(found);
            CHECK((found.Value().Id() == TypeId{0x54544f, 100 + index}));
        } else if (index % 2 == 0) {
            CHECK(found.Error() == ErrorCode::NotFound);
        } else {
            REQUIRE(found);
            CHECK((found.Value().Id() == TypeId{0x54544f, index + 1}));
        }
    }

    CHECK(types.Shutdown());
}

// ClassReference 필드는 Base ID가 필수이고(0이면 InvalidArgument) Base를 의존으로 붙잡지 않는다:
// 아직 등록되지 않은 클래스·자기 자신을 Base로 둘 수 있고, Base 클래스의 해제는 Busy가 아니다.
// Required를 받는다. AcquireDerivedClass는 Base 사슬로 판정한다.
TEST(TypeToolName_ClassReferenceFieldRegistration) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    FieldSchema field;
    field.id = Guid{0x54544e, 0xf2};
    field.name = Span<const char>("kind", 4);
    field.flags = static_cast<uint32>(FieldFlags::Required);
    field.value = ValueType{ValueKind::ClassReference, {}};
    const TypeId base{0x54544e, 0x20};
    const TypeId owner{0x54544e, 0x21};
    RuntimeFieldOps fieldOps;
    fieldOps.fieldId = field.id;
    fieldOps.read = [](ObjectRegistry&, const ObjectAccessContext&, ObjectHandle,
                       FieldValue&) noexcept { return Result<void>::Success(); };
    fieldOps.write = [](ObjectRegistry&, const ObjectAccessContext&, ObjectHandle,
                        const FieldValue&) noexcept { return Result<void>::Success(); };
    fieldOps.writeStaged = [](ConstructionContext&, ObjectHandle, const FieldValue&) noexcept {
        return Result<void>::Success();
    };

    auto typeOps = [&fieldOps] {
        auto ops = MakeRuntimeTypeOps<Holder>();
        ops.fields = Span<const RuntimeFieldOps>(&fieldOps, 1);
        return ops;
    };

    TypeSchema schema{owner, 1, Span<const FieldSchema>(&field, 1), Chars("Owner")};
    CHECK(ErrorOf(types.Register(schema, typeOps())) == ErrorCode::InvalidArgument);
    // 자기 자신을 Base로 둔다(ClassRef<Self>).
    field.value.type = owner;
    auto self = types.Register(schema, typeOps());
    REQUIRE(self);
    REQUIRE(types.Unregister(self.Value()));
    // 아직 등록되지 않은 클래스를 Base로 둔 뒤 Base를 등록·해제한다(의존 계수 없음).
    field.value.type = base;
    auto holder = types.Register(schema, typeOps());
    REQUIRE(holder);
    auto baseToken = RegisterClass(types, base, {});
    REQUIRE(baseToken);
    CHECK(types.AcquireDerivedClass(base, base));
    CHECK(types.AcquireDerivedClass(owner, base).Error() == ErrorCode::InvalidArgument);
    REQUIRE(types.Unregister(baseToken.Value()));
    CHECK(types.AcquireDerivedClass(base, base).Error() == ErrorCode::NotFound);
    REQUIRE(types.Unregister(holder.Value()));
    // Array<ClassRef> 원소도 Base ID가 필수다(element.type 0 InvalidArgument). aggregate라 함수표는
    // codec만 둔다(codec 없는 Transient로 등록한다).
    FieldSchema array;
    array.id = field.id;
    array.name = Span<const char>("kinds", 5);
    array.flags = static_cast<uint32>(FieldFlags::Transient);
    array.value = ValueType{ValueKind::Array, {}};
    array.element = ValueType{ValueKind::ClassReference, {}};
    array.maxElements = DefaultMaxElements;
    RuntimeFieldOps arrayOps;
    arrayOps.fieldId = array.id;
    auto arrayTypeOps = [&arrayOps] {
        auto ops = MakeRuntimeTypeOps<Holder>();
        ops.fields = Span<const RuntimeFieldOps>(&arrayOps, 1);
        return ops;
    };

    TypeSchema arraySchema{owner, 1, Span<const FieldSchema>(&array, 1), Chars("Owner")};
    CHECK(ErrorOf(types.Register(arraySchema, arrayTypeOps())) == ErrorCode::InvalidArgument);
    array.element.type = base;
    auto arrayHolder = types.Register(arraySchema, arrayTypeOps());
    REQUIRE(arrayHolder);
    REQUIRE(types.Unregister(arrayHolder.Value()));
    CHECK(types.Shutdown());
}
