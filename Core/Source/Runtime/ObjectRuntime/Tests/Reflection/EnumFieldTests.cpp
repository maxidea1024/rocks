#include <ReflectionTestFixtures/ReflectionFixture.generated.h>
#include <Core/Memory/SystemAllocator.h>
#include <Object/ConstructionContext.h>
#include <Object/ModuleRegistration.h>
#include <Platform/Fatal.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// B3b 리플렉션 enum 시험이다(Docs/Design/ReflectionFoundations.ko.md 3.4–3.5절): 생성 스키마와
// 검증기, 도구 이름 조회, TypeRegistry 종류·enum 의존 계수·native 키 대조, 수동 RegisterEnum 검증과
// 깊은 복사, 모듈 등록, enum 필드의 읽기·쓰기·staged 쓰기 검증.
using namespace rocks;
using reflection_fixture::Bias;
using reflection_fixture::Channels;
using reflection_fixture::EnumFields;
using reflection_fixture::TextureGroup;

namespace {

// 리플렉션하지 않은 enum이다. native 키만 빌려 어긋난 등록을 만든다.
enum class UnrelatedEnum : uint8 {
    Only
};

Span<const char> Text(const char* text) noexcept {
    usize size = 0;
    while (text[size] != '\0') {
        ++size;
    }

    return {text, size};
}

usize SchemaIndex(const TypeSchema& schema, StringView name) noexcept {
    for (usize index = 0; index < schema.fields.Size(); ++index) {
        const FieldSchema& field = schema.fields[index];
        if (StringView(field.name.Data(), field.name.Size()) == name) {
            return index;
        }
    }

    platform::Fatal("fixture field missing");
}

const RuntimeFieldOps& FieldOps(Span<const RuntimeFieldOps> fields, StringView name) noexcept {
    const Guid id = GeneratedType<EnumFields>::Schema()
                        .fields[SchemaIndex(GeneratedType<EnumFields>::Schema(), name)]
                        .id;
    for (const RuntimeFieldOps& field : fields) {
        if (field.fieldId == id) {
            return field;
        }
    }

    platform::Fatal("fixture field ops missing");
}

// 픽스처 enum 셋을 등록한다(EnumFields보다 먼저).
bool RegisterEnums(TypeRegistry& types) noexcept {
    return GeneratedType<TextureGroup>::Register(types).IsOk() &&
           GeneratedType<Bias>::Register(types).IsOk() &&
           GeneratedType<Channels>::Register(types).IsOk();
}

FieldValue EnumValue(Allocator& allocator, TypeId type, int64 value) noexcept {
    FieldValue result(allocator);
    result.kind = ValueKind::Enum;
    result.type = type;
    result.signedInteger = value;
    return result;
}

bool ReadsAs(ObjectRegistry& objects, const RuntimeFieldOps& field, ObjectHandle handle,
             TypeId type, int64 expected, Allocator& allocator) noexcept {
    FieldValue read(allocator);
    return field.read(objects, objects.OwnerAccess(), handle, read) &&
           read.kind == ValueKind::Enum && read.type == type && read.signedInteger == expected;
}

} // namespace

TEST(Enums_GeneratedSchemaAndValidatorsMatch) {
    const EnumSchema& group = GeneratedType<TextureGroup>::Schema();
    CHECK(group.id == TypeIdOf<TextureGroup>());
    CHECK(group.underlying == ValueKind::UInt8);
    CHECK(group.flags == 0);
    CHECK(StringView(group.toolPrefix.Data(), group.toolPrefix.Size()) ==
          StringView("TEXTUREGROUP_"));
    REQUIRE(group.values.Size() == 3);
    CHECK(group.values[1].value == 7);
    CHECK(StringView(group.values[1].toolName.Data(), group.values[1].toolName.Size()) ==
          StringView("8BitData"));
    CHECK(StringView(group.values[1].displayName.Data(), group.values[1].displayName.Size()) ==
          StringView("8비트 데이터"));
    CHECK(HasFlag(group.values[2].flags, EnumeratorFlags::Hidden));
    CHECK(group.values[0].toolName.Size() == 0);

    // 암묵 값은 이전 값 + 1이다.
    const EnumSchema& bias = GeneratedType<Bias>::Schema();
    CHECK(bias.underlying == ValueKind::Int32);
    REQUIRE(bias.values.Size() == 3);
    CHECK(bias.values[0].value == -2 && bias.values[2].value == 1);

    // 생성 constexpr 검증기와 스키마 검증기가 같은 답을 낸다.
    static_assert(GeneratedType<TextureGroup>::IsDeclared(7));
    static_assert(!GeneratedType<TextureGroup>::IsDeclared(8));
    static_assert(GeneratedType<Channels>::IsDeclared(1 | 4 | 8));
    static_assert(!GeneratedType<Channels>::IsDeclared(16));
    const int64 samples[] = {-3, -2, -1, 0, 1, 2, 3, 5, 7, 8, 13, 15, 16, 255, 256, 65535, 65536};
    for (int64 sample : samples) {
        CHECK(GeneratedType<TextureGroup>::IsDeclared(sample) ==
              IsDeclaredEnumValue(group, sample));
        CHECK(GeneratedType<Bias>::IsDeclared(sample) == IsDeclaredEnumValue(bias, sample));
        CHECK(GeneratedType<Channels>::IsDeclared(sample) ==
              IsDeclaredEnumValue(GeneratedType<Channels>::Schema(), sample));
    }
}

TEST(Enums_ToolNameLookupIgnoresCaseAndOptionalPrefix) {
    const EnumSchema& group = GeneratedType<TextureGroup>::Schema();
    CHECK(FindEnumerator(group, Text("8BitData")) == &group.values[1]);
    CHECK(FindEnumerator(group, Text("texturegroup_8bitdata")) == &group.values[1]);
    CHECK(FindEnumerator(group, Text("TEXTUREGROUP_World")) == &group.values[0]);
    CHECK(FindEnumerator(group, Text("world")) == &group.values[0]);
    // C++ 이름은 도구 이름이 따로 있으면 받지 않는다. 숨김 열거자도 찾는다(설정 계층이 거부한다).
    CHECK(FindEnumerator(group, Text("EightBitData")) == nullptr);
    CHECK(FindEnumerator(group, Text("Reserved13")) == &group.values[2]);
    CHECK(FindEnumerator(group, Text("TEXTUREGROUP_")) == nullptr);
    CHECK(FindEnumerator(group, Text("")) == nullptr);
    CHECK(FindEnumerator(GeneratedType<Bias>::Schema(), Text("TEXTUREGROUP_Low")) == nullptr);
}

TEST(Enums_RegistryKindsDependenciesAndNativeKeys) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    // Enum 필드가 가리키는 enum은 먼저 등록되어 있어야 한다.
    CHECK(GeneratedType<EnumFields>::Register(types).Error() == ErrorCode::NotFound);
    auto group = GeneratedType<TextureGroup>::Register(types);
    auto bias = GeneratedType<Bias>::Register(types);
    REQUIRE(group && bias);
    CHECK(GeneratedType<EnumFields>::Register(types).Error() == ErrorCode::NotFound);
    auto channels = GeneratedType<Channels>::Register(types);
    REQUIRE(channels);
    CHECK(GeneratedType<TextureGroup>::Register(types).Error() == ErrorCode::AlreadyExists);
    auto owner = GeneratedType<EnumFields>::Register(types);
    REQUIRE(owner);

    // Acquire는 클래스만, AcquireValueType은 Enum만 연다.
    CHECK(types.Acquire(TypeIdOf<TextureGroup>()).Error() == ErrorCode::TypeMismatch);
    CHECK(types.AcquireValueType(TypeIdOf<EnumFields>(), TypeKind::Enum).Error() ==
          ErrorCode::TypeMismatch);
    CHECK(types.AcquireValueType(TypeIdOf<TextureGroup>(), TypeKind::Class).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(types.AcquireValueType(TypeIdOf<TextureGroup>(), TypeKind::Struct).Error() ==
          ErrorCode::TypeMismatch);
    CHECK(types.AcquireValueType(Guid{7, 7}, TypeKind::Enum).Error() == ErrorCode::NotFound);
    CHECK(types.IsA(TypeIdOf<TextureGroup>(), TypeIdOf<EnumFields>()).Error() ==
          ErrorCode::TypeMismatch);
    {
        auto lease = types.AcquireValueType(TypeIdOf<TextureGroup>(), TypeKind::Enum);
        REQUIRE(lease);
        CHECK(lease.Value().Kind() == TypeKind::Enum);
        CHECK(lease.Value().Enum().values.Size() == 3);
        CHECK(lease.Value().Name() == StringView("rocks::reflection_fixture::TextureGroup"));
        CHECK(lease.Value().Ops().nativeType == NativeTypeKey<TextureGroup>());
        auto classLease = types.Acquire(TypeIdOf<EnumFields>());
        REQUIRE(classLease);
        CHECK(classLease.Value().Kind() == TypeKind::Class);
        const TypeSchema& schema = classLease.Value().Schema();
        const EnumSchema* fieldEnum = classLease.Value().FieldEnum(SchemaIndex(schema, "group"));
        CHECK(fieldEnum == &lease.Value().Enum());
        CHECK(classLease.Value().FieldEnum(SchemaIndex(schema, "channels"))->id ==
              TypeIdOf<Channels>());
    }

    // 의존자가 남은 enum은 해제할 수 없다. 의존 계수는 lease가 아니므로 Shutdown을 막지 않는다.
    CHECK(types.Unregister(group.Value()).Error() == ErrorCode::Busy);
    REQUIRE(types.Unregister(owner.Value()));
    REQUIRE(types.Unregister(group.Value()));
    CHECK(types.Stats().registeredTypes == 2);
    auto again = GeneratedType<TextureGroup>::Register(types);
    REQUIRE(again);
    REQUIRE(GeneratedType<EnumFields>::Register(types));
    CHECK(types.Shutdown());
}

TEST(Enums_ClassRegistrationChecksEnumTargetKindAndNativeKey) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    // 같은 ID로 다른 native enum을 등록하면 정적 thunk와 어긋나므로 클래스 등록이 거부된다.
    auto wrong = types.RegisterEnum(GeneratedType<TextureGroup>::Schema(),
                                    RuntimeEnumOps{NativeTypeKey<UnrelatedEnum>(), {}});
    REQUIRE(wrong);
    REQUIRE(GeneratedType<Bias>::Register(types));
    REQUIRE(GeneratedType<Channels>::Register(types));
    CHECK(GeneratedType<EnumFields>::Register(types).Error() == ErrorCode::TypeMismatch);
    REQUIRE(types.Unregister(wrong.Value()));
    REQUIRE(GeneratedType<TextureGroup>::Register(types));

    // 같은 C++ enum을 다른 ID로 다시 등록하지 않는다(thunk는 생성 ID 하나만 받는다).
    EnumSchema alias = GeneratedType<TextureGroup>::Schema();
    alias.id = Guid{0x414c494153ull, 1};
    CHECK(types.RegisterEnum(alias, RuntimeEnumOps{NativeTypeKey<TextureGroup>(), {}}).Error() ==
          ErrorCode::AlreadyExists);

    // base는 클래스여야 한다.
    TypeSchema enumBase = GeneratedType<EnumFields>::Schema();
    enumBase.baseType = TypeIdOf<TextureGroup>();
    CHECK(types.Register(enumBase, GeneratedType<EnumFields>::Ops()).Error() ==
          ErrorCode::TypeMismatch);

    // Enum 필드가 클래스 레코드를 가리키거나, enum ID가 없거나, valueNativeType이 어긋나면
    // 거부한다.
    const TypeSchema& generated = GeneratedType<EnumFields>::Schema();
    const usize group = SchemaIndex(generated, "group");
    auto registerWith = [&](void (*mutate)(FieldSchema&) noexcept) noexcept {
        FieldSchema fields[3];
        for (usize index = 0; index < 3; ++index) {
            fields[index] = generated.fields[index];
        }

        mutate(fields[group]);
        TypeSchema schema = generated;
        schema.fields = Span<const FieldSchema>(fields, 3);
        return types.Register(schema, GeneratedType<EnumFields>::Ops()).Error();
    };
    auto base = GeneratedType<reflection_fixture::Base>::Register(types);
    REQUIRE(base);
    CHECK(registerWith([](FieldSchema& field) noexcept {
              field.value.type = TypeIdOf<reflection_fixture::Base>();
          }) == ErrorCode::TypeMismatch);
    CHECK(registerWith([](FieldSchema& field) noexcept { field.value.type = TypeId{}; }) ==
          ErrorCode::InvalidArgument);
    CHECK(registerWith([](FieldSchema& field) noexcept { field.value.type = TypeIdOf<Bias>(); }) ==
          ErrorCode::TypeMismatch);
    CHECK(registerWith([](FieldSchema& field) noexcept {
              field.flags |= static_cast<uint32>(FieldFlags::Required);
          }) == ErrorCode::InvalidArgument);
    CHECK(registerWith([](FieldSchema& field) noexcept {
              field.kind = FieldKind::ObjectReference;
          }) == ErrorCode::InvalidArgument);

    // 함수표: Enum 필드에 valueNativeType이 없거나 그 밖의 필드에 있으면 거부한다.
    RuntimeTypeOps missing = GeneratedType<EnumFields>::Ops();
    RuntimeFieldOps copied[3];
    for (usize index = 0; index < 3; ++index) {
        copied[index] = missing.fields[index];
        copied[index].valueNativeType = nullptr;
    }

    missing.fields = Span<const RuntimeFieldOps>(copied, 3);
    CHECK(types.Register(generated, Move(missing)).Error() == ErrorCode::InvalidArgument);
    CHECK(types.Shutdown());
}

TEST(Enums_ManualRegisterEnumValidatesAndDeepCopies) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    const void* native = NativeTypeKey<TextureGroup>();
    char name[] = "manual::Mode";
    char first[] = "First";
    char display[] = "첫째";
    EnumeratorSchema values[] = {{Text("First"), {}, {}, 0, 0}, {Text("Second"), {}, {}, 1, 0}};
    values[0].name = Span<const char>(first, sizeof(first) - 1);
    values[0].displayName = Span<const char>(display, sizeof(display) - 1);
    EnumSchema schema{Guid{0x454e554d, 1},
                      Span<const char>(name, sizeof(name) - 1),
                      ValueKind::UInt8,
                      0u,
                      Span<const EnumeratorSchema>(values, 2),
                      {}};

    struct Case {
        void (*mutate)(EnumSchema&, EnumeratorSchema*) noexcept;
        ErrorCode expected;
    };

    const Case cases[] = {
        {[](EnumSchema& s, EnumeratorSchema*) noexcept { s.id = Guid{}; },
         ErrorCode::InvalidArgument},
        {[](EnumSchema& s, EnumeratorSchema*) noexcept { s.name = {}; },
         ErrorCode::InvalidArgument},
        {[](EnumSchema& s, EnumeratorSchema*) noexcept { s.underlying = ValueKind::Int64; },
         ErrorCode::InvalidArgument},
        {[](EnumSchema& s, EnumeratorSchema*) noexcept { s.flags = 2; },
         ErrorCode::InvalidArgument},
        {[](EnumSchema& s, EnumeratorSchema*) noexcept { s.values = {}; },
         ErrorCode::InvalidArgument},
        {[](EnumSchema& s, EnumeratorSchema*) noexcept { s.toolPrefix = Text("BAD-"); },
         ErrorCode::InvalidArgument},
        {[](EnumSchema&, EnumeratorSchema* v) noexcept { v[1].value = 256; },
         ErrorCode::InvalidArgument},
        {[](EnumSchema& s, EnumeratorSchema* v) noexcept {
             s.flags = 1;
             s.underlying = ValueKind::Int32;
             v[1].value = -1;
         },
         ErrorCode::InvalidArgument},
        {[](EnumSchema&, EnumeratorSchema* v) noexcept { v[1].name = Text("1st"); },
         ErrorCode::InvalidArgument},
        {[](EnumSchema&, EnumeratorSchema* v) noexcept { v[1].toolName = Text("has space"); },
         ErrorCode::InvalidArgument},
        {[](EnumSchema&, EnumeratorSchema* v) noexcept { v[1].flags = 2; },
         ErrorCode::InvalidArgument},
        // 값 별칭과 (접두 유무를 합친) 도구 이름 중복
        {[](EnumSchema&, EnumeratorSchema* v) noexcept { v[1].value = 0; },
         ErrorCode::AlreadyExists},
        {[](EnumSchema&, EnumeratorSchema* v) noexcept { v[1].toolName = Text("FIRST"); },
         ErrorCode::AlreadyExists},
        {[](EnumSchema& s, EnumeratorSchema* v) noexcept {
             s.toolPrefix = Text("MODE_");
             v[1].toolName = Text("mode_first");
         },
         ErrorCode::AlreadyExists}};
    for (const Case& example : cases) {
        EnumSchema mutated = schema;
        EnumeratorSchema mutatedValues[2] = {values[0], values[1]};
        mutated.values = Span<const EnumeratorSchema>(mutatedValues, 2);
        example.mutate(mutated, mutatedValues);
        CHECK(types.RegisterEnum(mutated, RuntimeEnumOps{native, {}}).Error() == example.expected);
    }

    CHECK(types.RegisterEnum(schema, RuntimeEnumOps{nullptr, {}}).Error() ==
          ErrorCode::InvalidArgument);
    auto registered = types.RegisterEnum(schema, RuntimeEnumOps{native, {}});
    REQUIRE(registered);
    CHECK(types.RegisterEnum(schema, RuntimeEnumOps{native, {}}).Error() ==
          ErrorCode::AlreadyExists);

    // 등록은 이름·열거자 바이트를 깊게 복사한다.
    name[0] = 'X';
    first[0] = 'X';
    display[0] = 'X';
    values[0].value = 9;
    {
        auto lease = types.AcquireValueType(Guid{0x454e554d, 1}, TypeKind::Enum);
        REQUIRE(lease);
        CHECK(lease.Value().Name() == StringView("manual::Mode"));
        const EnumSchema& copy = lease.Value().Enum();
        CHECK(StringView(copy.values[0].name.Data(), copy.values[0].name.Size()) ==
              StringView("First"));
        CHECK(StringView(copy.values[0].displayName.Data(), copy.values[0].displayName.Size()) ==
              StringView("첫째"));
        CHECK(copy.values[0].value == 0);
        CHECK(types.Unregister(registered.Value()).Error() == ErrorCode::Busy);
    }

    REQUIRE(types.Unregister(registered.Value()));
    CHECK(types.Shutdown());
}

TEST(Enums_RegisterEnumAllocationFailureKeepsRegistry) {
    SystemAllocator backing;
    test::NFailAllocator allocator(backing);
    {
        TypeRegistry types(allocator);
        allocator.FailOnAttempt(1);
        CHECK(GeneratedType<TextureGroup>::Register(types).Error() == ErrorCode::OutOfMemory);
        CHECK(types.Stats().registeredTypes == 0);
        allocator.DisableFailure();
        REQUIRE(GeneratedType<TextureGroup>::Register(types));
        CHECK(types.Shutdown());
    }

    CHECK(backing.Stats().liveAllocations == 0);
}

TEST(Enums_ModuleRegistrationRegistersEnumsOnceAndRetainsShared) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    {
        ModuleRegistration first(types, allocator);
        auto added = first.RegisterEnumIfAbsent(GeneratedType<TextureGroup>::Schema(),
                                                RuntimeEnumOps{NativeTypeKey<TextureGroup>(), {}});
        REQUIRE(added);
        CHECK(added.Value());
        ModuleRegistration second(types, allocator);
        auto retained =
            second.RegisterEnumIfAbsent(GeneratedType<TextureGroup>::Schema(),
                                        RuntimeEnumOps{NativeTypeKey<TextureGroup>(), {}});
        REQUIRE(retained);
        CHECK(!retained.Value());
        CHECK(second
                  .RegisterEnumIfAbsent(GeneratedType<TextureGroup>::Schema(),
                                        RuntimeEnumOps{NativeTypeKey<Bias>(), {}})
                  .Error() == ErrorCode::AlreadyExists);
        CHECK(second.Size() == 1);
        // 붙잡은 lease가 있으므로 등록한 쪽의 해제는 Busy다.
        CHECK(first.Unregister().Error() == ErrorCode::Busy);
        REQUIRE(second.Unregister());
        REQUIRE(first.Unregister());
        CHECK(types.Stats().registeredTypes == 0);

        // 같은 ID의 클래스가 있으면 TypeMismatch다.
        auto node = GeneratedType<reflection_fixture::Base>::Register(types);
        REQUIRE(node);
        EnumSchema clash = GeneratedType<TextureGroup>::Schema();
        clash.id = TypeIdOf<reflection_fixture::Base>();
        CHECK(first.RegisterEnumIfAbsent(clash, RuntimeEnumOps{NativeTypeKey<TextureGroup>(), {}})
                  .Error() == ErrorCode::TypeMismatch);
        CHECK(first.Size() == 0);
        REQUIRE(types.Unregister(node.Value()));
    }

    CHECK(types.Shutdown());
}

TEST(Enums_FieldWritesValidateTypeAndDeclaredValues) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterEnums(types));
    REQUIRE(GeneratedType<EnumFields>::Register(types));
    {
        ObjectRegistry objects(allocator, types, {0x454e, 1});
        const auto access = objects.OwnerAccess();
        {
            auto lease = types.Acquire(TypeIdOf<EnumFields>());
            REQUIRE(lease);
            const RuntimeFieldOps& group = FieldOps(lease.Value().Ops().fields, "group");
            const RuntimeFieldOps& bias = FieldOps(lease.Value().Ops().fields, "bias");
            const RuntimeFieldOps& channels = FieldOps(lease.Value().Ops().fields, "channels");
            ConstructionContext construction(objects, access);
            auto created = construction.Create<EnumFields>(TypeIdOf<EnumFields>());
            REQUIRE(created);
            // staged 쓰기도 선언 값만 받는다.
            CHECK(group
                      .writeStaged(construction, created.Value(),
                                   EnumValue(allocator, TypeIdOf<TextureGroup>(), 8))
                      .Error() == ErrorCode::InvalidArgument);
            REQUIRE(group.writeStaged(construction, created.Value(),
                                      EnumValue(allocator, TypeIdOf<TextureGroup>(), 13)));
            auto committed = construction.Commit();
            REQUIRE(committed);
            const ObjectHandle handle = committed.Value().Handle();
            CHECK(ReadsAs(objects, group, handle, TypeIdOf<TextureGroup>(), 13, allocator));

            REQUIRE(group.write(objects, access, handle,
                                EnumValue(allocator, TypeIdOf<TextureGroup>(), 7)));
            REQUIRE(
                bias.write(objects, access, handle, EnumValue(allocator, TypeIdOf<Bias>(), -2)));
            REQUIRE(channels.write(objects, access, handle,
                                   EnumValue(allocator, TypeIdOf<Channels>(), 1 | 2 | 8)));
            REQUIRE(channels.write(objects, access, handle,
                                   EnumValue(allocator, TypeIdOf<Channels>(), 0)));
            REQUIRE(channels.write(objects, access, handle,
                                   EnumValue(allocator, TypeIdOf<Channels>(), 4)));

            // 선언되지 않은 값·다른 enum ID·다른 값 종류는 거부하고 값은 그대로다.
            CHECK(group
                      .write(objects, access, handle,
                             EnumValue(allocator, TypeIdOf<TextureGroup>(), 8))
                      .Error() == ErrorCode::InvalidArgument);
            CHECK(group
                      .write(objects, access, handle,
                             EnumValue(allocator, TypeIdOf<TextureGroup>(), 256 + 7))
                      .Error() == ErrorCode::InvalidArgument);
            CHECK(group.write(objects, access, handle, EnumValue(allocator, TypeIdOf<Bias>(), 0))
                      .Error() == ErrorCode::TypeMismatch);
            CHECK(bias.write(objects, access, handle, EnumValue(allocator, TypeIdOf<Bias>(), 2))
                      .Error() == ErrorCode::InvalidArgument);
            CHECK(
                channels
                    .write(objects, access, handle, EnumValue(allocator, TypeIdOf<Channels>(), 16))
                    .Error() == ErrorCode::InvalidArgument);
            CHECK(
                channels
                    .write(objects, access, handle, EnumValue(allocator, TypeIdOf<Channels>(), -1))
                    .Error() == ErrorCode::InvalidArgument);
            FieldValue wrongKind(allocator);
            wrongKind.kind = ValueKind::Int64;
            wrongKind.signedInteger = 7;
            CHECK(group.write(objects, access, handle, wrongKind).Error() ==
                  ErrorCode::TypeMismatch);

            // 등록 전 원본 callback(바인딩 없는 write)도 thunk에서 같은 규칙을 검사한다.
            const RuntimeTypeOps unbound = GeneratedType<EnumFields>::Ops();
            CHECK(FieldOps(unbound.fields, "group")
                      .write(objects, access, handle,
                             EnumValue(allocator, TypeIdOf<TextureGroup>(), 8))
                      .Error() == ErrorCode::InvalidArgument);
            CHECK(FieldOps(unbound.fields, "group")
                      .write(objects, access, handle, EnumValue(allocator, TypeIdOf<Bias>(), 0))
                      .Error() == ErrorCode::TypeMismatch);

            CHECK(ReadsAs(objects, group, handle, TypeIdOf<TextureGroup>(), 7, allocator));
            CHECK(ReadsAs(objects, bias, handle, TypeIdOf<Bias>(), -2, allocator));
            CHECK(ReadsAs(objects, channels, handle, TypeIdOf<Channels>(), 4, allocator));
            auto borrowed = objects.Resolve<EnumFields>(access, handle);
            REQUIRE(borrowed);
            auto value = borrowed.Value().Get();
            REQUIRE(value);
            CHECK(value.Value()->group == TextureGroup::EightBitData);
            CHECK(value.Value()->bias == Bias::Low);
            CHECK(value.Value()->channels == Channels::Blue);
        }

        REQUIRE(objects.Shutdown(access));
    }

    CHECK(types.Shutdown());
}
