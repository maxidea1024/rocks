#include "ClassRefTestSupport.h"

// 리플렉션 B9d2 ClassRef PROPERTY의 쓰기 검사와 타입 도구 이름 시험이다(Docs/Design/
// ReflectionExtensions.ko.md 5.2절): 등록 write(FieldWriteOperation)·경로 쓰기(최상위·struct 멤버·
// Array 원소 쓰기·삽입·원소 struct 멤버)·staged 쓰기가 등록된 클래스이고 Base의 파생인지
// 보고(미등록 NotFound, struct ID·무관한 클래스·Base 위반 InvalidArgument, 추상 클래스·null 허용)
// 실패한 쓰기는 값을 바꾸지 않는다. ClassRef::Resolve·TypeRegistry::AcquireDerivedClass와 생성 타입
// 도구 이름 색인(기본 이름·Name 지정자, 대소문자 무시)을 본다.
using namespace rocks;
namespace class_ref_field_tests_file {

using namespace rocks::test::classref;

namespace {

const RuntimeFieldOps* FindOps(const TypeLease& lease, Guid id) noexcept {
    for (const RuntimeFieldOps& ops : lease.Ops().fields) {
        if (ops.fieldId == id) {
            return &ops;
        }
    }

    return nullptr;
}

} // namespace

// 최상위 필드(경로 쓰기·등록 write 직접 호출)·struct 멤버·배열 원소·원소 struct 멤버의 경로 쓰기와
// 배열 삽입이 같은 판정을 한다. 실패한 쓰기 뒤 값은 그대로이고, 성공한 쓰기는 옛 값을 돌려준다.
TEST(ClassRefField_WritesCheckRegistrationClassAndBase) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(allocator, types, {0x435246, 1});
        const auto access = objects.OwnerAccess();
        {
            auto lease = types.Acquire(TypeIdOf<ClassHolder>());
            REQUIRE(lease);
            RootRef<ClassHolder> holder = New<ClassHolder>(objects);
            const ObjectHandle handle = holder.Handle();
            auto parse = [&](const char* text, PropertyPathBuffer& out) noexcept {
                REQUIRE(ParsePropertyPath(lease.Value(), StringView(text), out));
            };

            PropertyPathBuffer element;
            parse("list[0]", element);
            const FieldValue derived = ClassValueOf(allocator, TypeIdOf<ClassDerived>());
            REQUIRE(PathInsertElement(objects, access, handle, element.View(), &derived));
            PropertyPathBuffer slotElement;
            parse("slots[0]", slotElement);
            REQUIRE(PathInsertElement(objects, access, handle, slotElement.View(), nullptr));

            const struct {
                TypeId value;
                ErrorCode expected;
            } rejected[] = {{TypeIdOf<ClassStranger>(), ErrorCode::InvalidArgument},
                            {TypeIdOf<ClassValue>(), ErrorCode::InvalidArgument},
                            {TypeId{0x435246, 0x77}, ErrorCode::NotFound}};
            const char* paths[] = {"any", "slot.kind", "list[0]", "slots[0].kind"};
            for (const char* text : paths) {
                PropertyPathBuffer path;
                parse(text, path);
                REQUIRE(PathWrite(objects, access, handle, path.View(),
                                  ClassValueOf(allocator, TypeIdOf<ClassDerived>())));
                for (const auto& item : rejected) {
                    PropertyPathFailure failure;
                    CHECK(ErrorOf(PathWrite(objects, access, handle, path.View(),
                                            ClassValueOf(allocator, item.value), nullptr,
                                            &failure)) == item.expected);
                    CHECK(item.expected != ErrorCode::InvalidArgument ||
                          failure.reason == PropertyPathError::InvalidValue);
                }

                FieldValue kept(allocator);
                REQUIRE(PathRead(objects, access, handle, path.View(), kept));
                CHECK(kept.kind == ValueKind::ClassReference &&
                      kept.guid == TypeIdOf<ClassDerived>());
                // 추상 클래스(ClassBase)로 바꾸고 옛 값을 받은 뒤 null로 비운다.
                FieldValue previous(allocator);
                REQUIRE(PathWrite(objects, access, handle, path.View(),
                                  ClassValueOf(allocator, TypeIdOf<ClassBase>()), &previous));
                CHECK(previous.kind == ValueKind::ClassReference &&
                      previous.guid == TypeIdOf<ClassDerived>());
                REQUIRE(PathWrite(objects, access, handle, path.View(),
                                  ClassValueOf(allocator, TypeId{})));
            }

            // Base가 ClassDerived인 필드는 ClassBase를 받지 않고 ClassLeaf는 받는다. 자기 클래스
            // Base(self)는 ClassHolder를 받는다.
            PropertyPathBuffer derivedPath;
            parse("derived", derivedPath);
            CHECK(ErrorOf(PathWrite(objects, access, handle, derivedPath.View(),
                                    ClassValueOf(allocator, TypeIdOf<ClassBase>()))) ==
                  ErrorCode::InvalidArgument);
            REQUIRE(PathWrite(objects, access, handle, derivedPath.View(),
                              ClassValueOf(allocator, TypeIdOf<ClassLeaf>())));
            PropertyPathBuffer selfPath;
            parse("self", selfPath);
            REQUIRE(PathWrite(objects, access, handle, selfPath.View(),
                              ClassValueOf(allocator, TypeIdOf<ClassHolder>())));

            // 배열 삽입도 같은 판정이다.
            const FieldValue stranger = ClassValueOf(allocator, TypeIdOf<ClassStranger>());
            CHECK(ErrorOf(PathInsertElement(objects, access, handle, element.View(), &stranger)) ==
                  ErrorCode::InvalidArgument);

            // 등록 write를 직접 불러도 같은 판정이다.
            const RuntimeFieldOps* anyOps = FindOps(lease.Value(), HolderField("any"));
            REQUIRE(anyOps && anyOps->write && anyOps->read);
            CHECK(ErrorOf(anyOps->write(objects, access, handle, stranger)) ==
                  ErrorCode::InvalidArgument);
            CHECK(ErrorOf(anyOps->write(objects, access, handle,
                                        ClassValueOf(allocator, TypeId{0x435246, 0x78}))) ==
                  ErrorCode::NotFound);
            REQUIRE(anyOps->write(objects, access, handle,
                                  ClassValueOf(allocator, TypeIdOf<ClassLeaf>())));
            FieldValue read(allocator);
            REQUIRE(anyOps->read(objects, access, handle, read));
            CHECK(read.kind == ValueKind::ClassReference && read.guid == TypeIdOf<ClassLeaf>());
            With(objects, handle, [](ClassHolder& value) noexcept {
                CHECK(value.any.Id() == TypeIdOf<ClassLeaf>());
                CHECK(value.derived.Id() == TypeIdOf<ClassLeaf>());
                CHECK(value.self.Id() == TypeIdOf<ClassHolder>());
                CHECK(value.slot.kind.IsNull());
                REQUIRE(value.list.Size() == 1);
                CHECK(value.list[0].IsNull());
                REQUIRE(value.slots.Size() == 1);
                CHECK(value.slots[0].kind.IsNull());
            });
        }

        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// staged 쓰기(writeStaged)도 등록·클래스·Base 파생을 본다. Commit 뒤 값이 남는다.
TEST(ClassRefField_StagedWritesCheckClasses) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(allocator, types, {0x435246, 2});
        const auto access = objects.OwnerAccess();
        {
            auto lease = types.Acquire(TypeIdOf<ClassHolder>());
            REQUIRE(lease);
            const RuntimeFieldOps* derivedOps = FindOps(lease.Value(), HolderField("derived"));
            REQUIRE(derivedOps && derivedOps->writeStaged);
            ConstructionContext construction(objects, access);
            auto owner = construction.Create<ClassHolder>(TypeIdOf<ClassHolder>());
            REQUIRE(owner);
            CHECK(
                ErrorOf(derivedOps->writeStaged(construction, owner.Value(),
                                                ClassValueOf(allocator, TypeIdOf<ClassBase>()))) ==
                ErrorCode::InvalidArgument);
            CHECK(
                ErrorOf(derivedOps->writeStaged(construction, owner.Value(),
                                                ClassValueOf(allocator, TypeIdOf<ClassValue>()))) ==
                ErrorCode::InvalidArgument);
            CHECK(ErrorOf(derivedOps->writeStaged(
                      construction, owner.Value(),
                      ClassValueOf(allocator, TypeId{0x435246, 0x79}))) == ErrorCode::NotFound);
            REQUIRE(derivedOps->writeStaged(construction, owner.Value(),
                                            ClassValueOf(allocator, TypeIdOf<ClassLeaf>())));
            const ObjectHandle roots[] = {owner.Value()};
            RootRef<Object> held[1];
            REQUIRE(construction.Commit(roots, held));
            With(objects, owner.Value(), [](ClassHolder& value) noexcept {
                CHECK(value.derived.Id() == TypeIdOf<ClassLeaf>());
            });
        }

        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// 별칭 스키마와 정적 Base: ClassHolder의 별칭이 derived의 Base를 ClassBase로 넓혀도, 별칭의 등록
// write는 생성 thunk의 정적 Base(ClassDerived)로 ClassBase를 거부하고, 원래 스키마의 write를 별칭
// 객체에 쓰면 원본 스키마의 Base(ClassDerived)로도 판정한다(둘 다 InvalidArgument). ClassLeaf는
// 받는다.
TEST(ClassRefField_AliasSchemaChecksSourceAndStaticBase) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        const TypeSchema& generated = TypeOf<ClassHolder>();
        REQUIRE(generated.fields.Size() <= 16);
        FieldSchema fields[16];
        const Guid derived = HolderField("derived");
        for (usize index = 0; index < generated.fields.Size(); ++index) {
            fields[index] = generated.fields[index];
            if (fields[index].id == derived) {
                fields[index].value.type = TypeIdOf<ClassBase>();
            }
        }

        TypeSchema alias = generated;
        alias.id = TypeId{0x435246, 0x60};
        alias.toolName = {};
        alias.fields = Span<const FieldSchema>(fields, generated.fields.Size());
        REQUIRE(types.Register(alias, GeneratedType<ClassHolder>::Ops()));
        ObjectRegistry objects(allocator, types, {0x435246, 3});
        const auto access = objects.OwnerAccess();
        {
            auto created = objects.NewObject<ClassHolder>(access, alias.id);
            REQUIRE(created);
            const ObjectHandle handle = created.Value().Handle();
            auto aliasLease = types.Acquire(alias.id);
            auto holderLease = types.Acquire(TypeIdOf<ClassHolder>());
            REQUIRE(aliasLease && holderLease);
            const RuntimeFieldOps* aliasOps = FindOps(aliasLease.Value(), derived);
            const RuntimeFieldOps* holderOps = FindOps(holderLease.Value(), derived);
            REQUIRE(aliasOps && aliasOps->write && holderOps && holderOps->write);
            const FieldValue base = ClassValueOf(allocator, TypeIdOf<ClassBase>());
            const FieldValue leaf = ClassValueOf(allocator, TypeIdOf<ClassLeaf>());
            CHECK(ErrorOf(aliasOps->write(objects, access, handle, base)) ==
                  ErrorCode::InvalidArgument);
            CHECK(ErrorOf(holderOps->write(objects, access, handle, base)) ==
                  ErrorCode::InvalidArgument);
            REQUIRE(aliasOps->write(objects, access, handle, leaf));
            REQUIRE(holderOps->write(objects, access, handle, leaf));
            With(objects, handle, [](ClassHolder& value) noexcept {
                CHECK(value.derived.Id() == TypeIdOf<ClassLeaf>());
            });
        }

        REQUIRE(objects.Shutdown(access));
    }

    CHECK(types.Shutdown());
}

// ClassRef::Resolve·AcquireDerivedClass는 같은 규칙이다(null·base 0 InvalidArgument, 미등록
// NotFound, struct·무관한 클래스·Base 위반 InvalidArgument, 추상 클래스 허용). 생성 타입은 타입
// 도구 이름(기본은 한정하지 않은 C++ 이름, ClassRenamed는 Name 지정자 "Fixture.Renamed")으로
// 대소문자 무시로 찾는다.
TEST(ClassRef_ResolveAndGeneratedToolNames) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        auto leaf = ClassRef<ClassBase>::Of<ClassLeaf>().Resolve(types);
        REQUIRE(leaf);
        CHECK(leaf.Value().Id() == TypeIdOf<ClassLeaf>());
        CHECK(ClassRef<ClassBase>::Of<ClassBase>().Resolve(types));
        CHECK(ClassRef<ClassBase>().Resolve(types).Error() == ErrorCode::InvalidArgument);
        CHECK(ClassRef<ClassDerived>(TypeIdOf<ClassBase>()).Resolve(types).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(ClassRef<ClassBase>(TypeIdOf<ClassValue>()).Resolve(types).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(ClassRef<ClassBase>(TypeId{0x435246, 0x7a}).Resolve(types).Error() ==
              ErrorCode::NotFound);
        CHECK(ClassRef<ClassBase>::Of<ClassLeaf>() == ClassRef<ClassBase>(TypeIdOf<ClassLeaf>()));

        CHECK(types.AcquireDerivedClass(TypeIdOf<ClassLeaf>(), TypeIdOf<ClassBase>()));
        CHECK(types.AcquireDerivedClass(TypeIdOf<ClassBase>(), TypeIdOf<ClassBase>()));
        CHECK(types.AcquireDerivedClass(TypeIdOf<ClassStranger>(), TypeIdOf<ClassBase>()).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(types.AcquireDerivedClass(TypeIdOf<ClassValue>(), TypeIdOf<ClassBase>()).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(types.AcquireDerivedClass(TypeIdOf<ClassLeaf>(), TypeId{}).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(types.AcquireDerivedClass(TypeId{0x435246, 0x7b}, TypeIdOf<ClassBase>()).Error() ==
              ErrorCode::NotFound);

        auto byName = [&](const char* text) noexcept {
            return types.AcquireByToolName(Span<const char>(text, StringView(text).ByteCount()));
        };

        auto holder = byName("ClassHolder");
        REQUIRE(holder);
        CHECK(holder.Value().Id() == TypeIdOf<ClassHolder>() &&
              holder.Value().Kind() == TypeKind::Class);
        auto lower = byName("classholder");
        CHECK(lower && lower.Value().Id() == TypeIdOf<ClassHolder>());
        auto renamed = byName("fixture.RENAMED");
        CHECK(renamed && renamed.Value().Id() == TypeIdOf<ClassRenamed>());
        CHECK(byName("ClassRenamed").Error() == ErrorCode::NotFound);
        auto value = byName("ClassValue");
        CHECK(value && value.Value().Kind() == TypeKind::Struct);
        // none은 예약이라 그 이름의 타입은 등록되지 않는다(별칭 스키마로 시도 — 수동 등록 경로).
        TypeSchema none = TypeOf<ClassStranger>();
        none.id = TypeId{0x435246, 0x7c};
        none.toolName = Span<const char>("NONE", 4);
        CHECK(types.Register(none, GeneratedType<ClassStranger>::Ops()).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(types.AcquireByToolName({}).Error() == ErrorCode::InvalidArgument);
        const TypeSchema& schema = TypeOf<ClassHolder>();
        CHECK(StringView(schema.toolName.Data(), schema.toolName.Size()) ==
              StringView("ClassHolder"));
    }

    CHECK(types.Shutdown());
}
} // namespace class_ref_field_tests_file
