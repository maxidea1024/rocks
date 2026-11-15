#include "OptionalTestSupport.h"

// 리플렉션 B9e Optional PROPERTY의 FieldValue·경로 시험이다(Docs/Design/ReflectionExtensions.ko.md
// 1.1·1.7·6절, 17절 B9e 항목): 단일 값 내부의 FieldValue kind = 내부 종류와 present(unset 쓰기),
// Optional<값 struct>의 aggregate 규칙, 경로 Emplace·Reset·unset 멤버 NotFound(OptionalUnset),
// Optional 멤버의 경로 쓰기와 옛 값, staged 쓰기, 새 값의 객체 allocator(ObjectAllocator).
using namespace rocks;
using namespace rocks::test::optional;

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

// 단일 값 Optional 필드의 등록 read·write는 kind = 내부 종류, present = set 여부다: unset 읽기는
// present false(enum은 type만 그 enum ID), present = false 쓰기는 unset으로 되돌리고(값 칸·enum
// ID는 보지 않는다), 값 쓰기는 내부 종류 규칙(종류 TypeMismatch·폭 Overflow·미선언 enum
// InvalidArgument)을 따른다. unset 자리의 새 String은 객체 allocator로 만든다. Optional<값
// struct>는 aggregate라 read가 없고 write는 Unsupported다. Optional이 아닌 자리에 present = false는
// InvalidArgument다.
TEST(OptionalField_FieldValueKindIsInnerAndPresentTracksSet) {
    SystemAllocator allocator;
    SystemAllocator values;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(allocator, types, {0x4f5046, 1});
        CHECK(&objects.ObjectAllocator() == &allocator);
        const auto access = objects.OwnerAccess();
        {
            auto lease = types.Acquire(TypeIdOf<OptionalHolder>());
            REQUIRE(lease);
            RootRef<OptionalHolder> holder = New(objects);
            const ObjectHandle handle = holder.Handle();

            const RuntimeFieldOps* count = FindOps(lease.Value(), HolderField("count"));
            REQUIRE(count && count->read && count->write && count->writeStaged && !count->path);
            FieldValue read(values);
            REQUIRE(count->read(objects, access, handle, read));
            CHECK(read.kind == ValueKind::UInt32 && !read.present);
            REQUIRE(count->write(objects, access, handle, U32Value(values, 5)));
            REQUIRE(count->read(objects, access, handle, read));
            CHECK(read.kind == ValueKind::UInt32 && read.present && read.unsignedInteger == 5);
            CHECK(ErrorOf(count->write(objects, access, handle,
                                       U32Value(values, uint64{1} << 40))) == ErrorCode::Overflow);
            FieldValue wide(values);
            wide.kind = ValueKind::UInt64;
            CHECK(ErrorOf(count->write(objects, access, handle, wide)) == ErrorCode::TypeMismatch);
            With(objects, handle, [](OptionalHolder& value) noexcept {
                CHECK(value.count.HasValue() && *value.count == 5u);
            });
            REQUIRE(count->write(objects, access, handle, U32Value(values, 99, false)));
            REQUIRE(count->read(objects, access, handle, read));
            CHECK(!read.present);

            // 생성자가 set으로 만든 값도 unset으로 되돌린다.
            const RuntimeFieldOps* preset = FindOps(lease.Value(), HolderField("preset"));
            REQUIRE(preset);
            REQUIRE(preset->read(objects, access, handle, read));
            CHECK(read.present && read.unsignedInteger == 7);
            REQUIRE(preset->write(objects, access, handle, U32Value(values, 0, false)));
            With(objects, handle,
                 [](OptionalHolder& value) noexcept { CHECK(!value.preset.HasValue()); });

            // String: unset 자리의 새 값은 객체 allocator로 만든다(FieldValue의 allocator가
            // 아니다).
            const RuntimeFieldOps* text = FindOps(lease.Value(), HolderField("text"));
            REQUIRE(text);
            REQUIRE(text->write(objects, access, handle, TextValue(values, "first")));
            With(objects, handle, [&](OptionalHolder& value) noexcept {
                REQUIRE(value.text.HasValue());
                CHECK(value.text->View() == "first");
                CHECK(&value.text->GetAllocator() == &objects.ObjectAllocator());
            });
            REQUIRE(text->write(objects, access, handle, TextValue(values, "second")));
            REQUIRE(text->read(objects, access, handle, read));
            CHECK(read.kind == ValueKind::Utf8String && read.present &&
                  read.string.View() == "second");
            REQUIRE(text->write(objects, access, handle, TextValue(values, {}, false)));
            REQUIRE(text->read(objects, access, handle, read));
            CHECK(!read.present && read.string.View() == "");

            // enum: unset 읽기는 type만 싣는다. 미선언 값은 InvalidArgument, unset 쓰기는 ID를 보지
            // 않는다.
            const RuntimeFieldOps* mode = FindOps(lease.Value(), HolderField("mode"));
            REQUIRE(mode);
            REQUIRE(mode->read(objects, access, handle, read));
            CHECK(read.kind == ValueKind::Enum && !read.present &&
                  read.type == TypeIdOf<OptionalMode>());
            FieldValue enumValue(values);
            enumValue.kind = ValueKind::Enum;
            enumValue.type = TypeIdOf<OptionalMode>();
            enumValue.signedInteger = 0;
            CHECK(ErrorOf(mode->write(objects, access, handle, enumValue)) ==
                  ErrorCode::InvalidArgument);
            enumValue.signedInteger = 2;
            REQUIRE(mode->write(objects, access, handle, enumValue));
            FieldValue clear(values);
            clear.kind = ValueKind::Enum;
            clear.present = false;
            REQUIRE(mode->write(objects, access, handle, clear));
            With(objects, handle,
                 [](OptionalHolder& value) noexcept { CHECK(!value.mode.HasValue()); });

            // Optional<값 struct>는 aggregate다.
            const RuntimeFieldOps* point = FindOps(lease.Value(), HolderField("point"));
            REQUIRE(point && !point->read && point->path);
            CHECK(ErrorOf(point->write(objects, access, handle, U32Value(values, 1))) ==
                  ErrorCode::Unsupported);

            // Optional이 아닌 자리(struct 멤버 presetPoint.x)에 unset은 InvalidArgument다.
            PropertyPathBuffer member;
            REQUIRE(ParsePropertyPath(lease.Value(), "presetPoint.x", member));
            FieldValue unsetInt(values);
            unsetInt.kind = ValueKind::Int32;
            unsetInt.present = false;
            PropertyPathFailure failure;
            CHECK(ErrorOf(PathWrite(objects, access, handle, member.View(), unsetInt, nullptr,
                                    &failure)) == ErrorCode::InvalidArgument);
            CHECK(failure.reason == PropertyPathError::InvalidValue);
        }

        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// 경로 연산: Optional<값 struct>의 멤버 경로는 set일 때만 닿고(unset이면 읽기·쓰기 모두
// NotFound(OptionalUnset)), PathEmplaceOptional이 기본 값을 만들고(이미 set이면 기본 값으로 바꾼다)
// PathResetOptional이 비운다(이미 unset이면 무동작). 끝이 Optional<값 struct>가 아니면
// TypeMismatch(NotOptional), ReadOnly 필드는 Unsupported(ReadOnly), 통째 읽기·쓰기는
// Unsupported(AggregateValue)다. struct 멤버 Optional(단일 값·String)은 경로 읽기·쓰기로 present를
// 다루고 옛 값을 돌려준다. 원소 struct 안의 Optional<struct>도 같다. 문자열 경로는 Optional<struct>
// 뒤의 `.`를 받고 FormatPropertyPath와 왕복한다.
TEST(OptionalField_PathEmplaceResetAndUnsetMember) {
    SystemAllocator allocator;
    SystemAllocator values;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(allocator, types, {0x4f5046, 2});
        const auto access = objects.OwnerAccess();
        {
            auto lease = types.Acquire(TypeIdOf<OptionalHolder>());
            REQUIRE(lease);
            RootRef<OptionalHolder> holder = New(objects);
            const ObjectHandle handle = holder.Handle();
            auto parse = [&](const char* text, PropertyPathBuffer& out) noexcept {
                REQUIRE(ParsePropertyPath(lease.Value(), StringView(text), out));
            };

            PropertyPathBuffer point;
            PropertyPathBuffer pointX;
            PropertyPathBuffer pointLabel;
            PropertyPathBuffer pointWeight;
            parse("point", point);
            parse("point.x", pointX);
            parse("point.label", pointLabel);
            parse("point.weight", pointWeight);
            FieldValue x(values);
            x.kind = ValueKind::Int32;
            x.signedInteger = 5;
            PropertyPathFailure failure;
            CHECK(ErrorOf(PathWrite(objects, access, handle, pointX.View(), x, nullptr,
                                    &failure)) == ErrorCode::NotFound);
            CHECK(failure.reason == PropertyPathError::OptionalUnset && failure.step == 0);
            FieldValue read(values);
            CHECK(ErrorOf(PathRead(objects, access, handle, pointX.View(), read, &failure)) ==
                  ErrorCode::NotFound);
            CHECK(failure.reason == PropertyPathError::OptionalUnset);

            REQUIRE(PathEmplaceOptional(objects, access, handle, point.View()));
            With(objects, handle, [&](OptionalHolder& value) noexcept {
                REQUIRE(value.point.HasValue());
                CHECK(value.point->x == 0 && !value.point->weight.HasValue());
                CHECK(&value.point->label.GetAllocator() == &objects.ObjectAllocator());
            });
            REQUIRE(PathWrite(objects, access, handle, pointX.View(), x));
            REQUIRE(
                PathWrite(objects, access, handle, pointLabel.View(), TextValue(values, "lbl")));
            REQUIRE(PathRead(objects, access, handle, pointX.View(), read));
            CHECK(read.kind == ValueKind::Int32 && read.signedInteger == 5);

            // struct 멤버 Optional<uint32>: present로 쓰고 옛 값을 받는다.
            FieldValue previous(values);
            REQUIRE(PathWrite(objects, access, handle, pointWeight.View(), U32Value(values, 3),
                              &previous));
            CHECK(previous.kind == ValueKind::UInt32 && !previous.present);
            REQUIRE(PathRead(objects, access, handle, pointWeight.View(), read));
            CHECK(read.present && read.unsignedInteger == 3);
            REQUIRE(PathWrite(objects, access, handle, pointWeight.View(),
                              U32Value(values, 0, false), &previous));
            CHECK(previous.present && previous.unsignedInteger == 3);
            REQUIRE(PathRead(objects, access, handle, pointWeight.View(), read));
            CHECK(!read.present);

            // 이미 set이면 기본 값으로 바꾼다.
            REQUIRE(PathEmplaceOptional(objects, access, handle, point.View()));
            With(objects, handle, [](OptionalHolder& value) noexcept {
                REQUIRE(value.point.HasValue());
                CHECK(value.point->x == 0 && value.point->label.View() == "");
            });
            REQUIRE(PathResetOptional(objects, access, handle, point.View()));
            REQUIRE(PathResetOptional(objects, access, handle, point.View()));
            With(objects, handle,
                 [](OptionalHolder& value) noexcept { CHECK(!value.point.HasValue()); });

            PropertyPathBuffer count;
            PropertyPathBuffer slot;
            PropertyPathBuffer locked;
            parse("count", count);
            parse("slot", slot);
            parse("locked", locked);
            CHECK(ErrorOf(PathEmplaceOptional(objects, access, handle, count.View(), &failure)) ==
                  ErrorCode::TypeMismatch);
            CHECK(failure.reason == PropertyPathError::NotOptional);
            CHECK(ErrorOf(PathResetOptional(objects, access, handle, slot.View(), &failure)) ==
                  ErrorCode::TypeMismatch);
            CHECK(failure.reason == PropertyPathError::NotOptional);
            CHECK(ErrorOf(PathEmplaceOptional(objects, access, handle, locked.View(), &failure)) ==
                  ErrorCode::Unsupported);
            CHECK(failure.reason == PropertyPathError::ReadOnly);
            CHECK(ErrorOf(PathRead(objects, access, handle, point.View(), read, &failure)) ==
                  ErrorCode::Unsupported);
            CHECK(failure.reason == PropertyPathError::AggregateValue);

            // struct 멤버 Optional<String>: unset 자리의 새 String은 객체 allocator, 옛 값은
            // previous의 allocator로 복사한다.
            PropertyPathBuffer note;
            parse("slot.note", note);
            REQUIRE(PathWrite(objects, access, handle, note.View(), TextValue(values, "first"),
                              &previous));
            CHECK(previous.kind == ValueKind::Utf8String && !previous.present);
            REQUIRE(PathWrite(objects, access, handle, note.View(), TextValue(values, "second"),
                              &previous));
            CHECK(previous.present && previous.string.View() == "first");
            With(objects, handle, [&](OptionalHolder& value) noexcept {
                REQUIRE(value.slot.note.HasValue());
                CHECK(value.slot.note->View() == "second");
                CHECK(&value.slot.note->GetAllocator() == &objects.ObjectAllocator());
            });
            REQUIRE(PathWrite(objects, access, handle, note.View(), TextValue(values, {}, false)));
            REQUIRE(PathRead(objects, access, handle, note.View(), read));
            CHECK(read.kind == ValueKind::Utf8String && !read.present);

            // 원소 struct 안의 Optional<struct>.
            PropertyPathBuffer element;
            PropertyPathBuffer elementPoint;
            PropertyPathBuffer elementX;
            parse("slots[0]", element);
            parse("slots[0].point", elementPoint);
            parse("slots[0].point.x", elementX);
            REQUIRE(PathInsertElement(objects, access, handle, element.View(), nullptr));
            CHECK(ErrorOf(PathWrite(objects, access, handle, elementX.View(), x, nullptr,
                                    &failure)) == ErrorCode::NotFound);
            CHECK(failure.reason == PropertyPathError::OptionalUnset && failure.step == 1);
            REQUIRE(PathEmplaceOptional(objects, access, handle, elementPoint.View()));
            REQUIRE(PathWrite(objects, access, handle, elementX.View(), x));
            With(objects, handle, [](OptionalHolder& value) noexcept {
                REQUIRE(value.slots.Size() == 1 && value.slots[0].point.HasValue());
                CHECK(value.slots[0].point->x == 5);
            });

            String formatted(values);
            REQUIRE(FormatPropertyPath(lease.Value(), elementX.View(), formatted));
            CHECK(formatted.View() == "slots[0].point.x");
        }

        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// staged 쓰기(writeStaged): Optional<Name>의 set 쓰기는 Name 필드와 같은 토큰 검사(다른 pool의 토큰
// WrongContext)를 하고 unset 쓰기는 토큰을 보지 않는다. unset 자리의 새 String은 트랜잭션
// Registry의 객체 allocator(ConstructionContext::ObjectAllocator)로 만든다.
TEST(OptionalField_StagedWritesCheckNamesAndUseObjectAllocator) {
    SystemAllocator allocator;
    SystemAllocator values;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    NamePool names(allocator);
    NamePool foreign(allocator);
    REQUIRE(names.Initialize());
    REQUIRE(foreign.Initialize());
    {
        ObjectRegistry objects(allocator, types, {0x4f5046, 3}, WithNames(&names));
        {
            auto lease = types.Acquire(TypeIdOf<OptionalHolder>());
            REQUIRE(lease);
            const RuntimeFieldOps* name = FindOps(lease.Value(), HolderField("name"));
            const RuntimeFieldOps* text = FindOps(lease.Value(), HolderField("text"));
            REQUIRE(name && text);
            auto own = names.TryIntern("Grip");
            auto stranger = foreign.TryIntern("Grip");
            REQUIRE(own && stranger);

            ConstructionContext transaction(objects, objects.OwnerAccess());
            CHECK(&transaction.ObjectAllocator() == &objects.ObjectAllocator());
            auto created = transaction.Create<OptionalHolder>(TypeIdOf<OptionalHolder>());
            REQUIRE(created);
            FieldValue token(values);
            token.kind = ValueKind::Name;
            token.name = stranger.Value();
            CHECK(ErrorOf(name->writeStaged(transaction, created.Value(), token)) ==
                  ErrorCode::WrongContext);
            token.present = false;
            REQUIRE(name->writeStaged(transaction, created.Value(), token));
            token.present = true;
            token.name = own.Value();
            REQUIRE(name->writeStaged(transaction, created.Value(), token));
            REQUIRE(text->writeStaged(transaction, created.Value(), TextValue(values, "staged")));
            auto staged = transaction.Get<OptionalHolder>(created.Value());
            REQUIRE(staged);
            CHECK(staged.Value()->name.HasValue() && *staged.Value()->name == own.Value());
            REQUIRE(staged.Value()->text.HasValue());
            CHECK(staged.Value()->text->View() == "staged");
            CHECK(&staged.Value()->text->GetAllocator() == &objects.ObjectAllocator());
            auto committed = transaction.Commit();
            REQUIRE(committed);
        }

        REQUIRE(objects.Collect(objects.OwnerAccess()));
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(names.Shutdown());
    CHECK(foreign.Shutdown());
    CHECK(types.Shutdown());
}

// PathIsOptionalSet은 Optional<값 struct>의 set 여부를 값 없이 돌려준다: 멤버 없는 struct, struct
// 멤버·원소 struct 안의 Optional<struct>, ReadOnly 필드(읽기라 막지 않는다)에서 Emplace·Reset을
// 따라 바뀐다. 끝이 Optional<값 struct>가 아니면 TypeMismatch(NotOptional)다.
TEST(OptionalField_PathIsOptionalSetReportsSetState) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(allocator, types, {0x4f5046, 4});
        const auto access = objects.OwnerAccess();
        {
            auto lease = types.Acquire(TypeIdOf<OptionalHolder>());
            REQUIRE(lease);
            RootRef<OptionalHolder> holder = New(objects);
            const ObjectHandle handle = holder.Handle();
            auto parse = [&](const char* text, PropertyPathBuffer& out) noexcept {
                REQUIRE(ParsePropertyPath(lease.Value(), StringView(text), out));
            };

            auto isSet = [&](const PropertyPathBuffer& path) noexcept {
                auto set = PathIsOptionalSet(objects, access, handle, path.View());
                CHECK(set);
                return set && set.Value();
            };

            const char* paths[] = {"empty", "point", "slot.point", "locked"};
            for (const char* text : paths) {
                PropertyPathBuffer path;
                parse(text, path);
                CHECK(!isSet(path));
                if (!(StringView(text) == "locked")) {
                    REQUIRE(PathEmplaceOptional(objects, access, handle, path.View()));
                    CHECK(isSet(path));
                    REQUIRE(PathResetOptional(objects, access, handle, path.View()));
                    CHECK(!isSet(path));
                }
            }

            PropertyPathBuffer presetPoint;
            parse("presetPoint", presetPoint);
            CHECK(isSet(presetPoint));

            PropertyPathBuffer element;
            PropertyPathBuffer elementPoint;
            parse("slots[0]", element);
            parse("slots[0].point", elementPoint);
            REQUIRE(PathInsertElement(objects, access, handle, element.View(), nullptr));
            CHECK(!isSet(elementPoint));
            REQUIRE(PathEmplaceOptional(objects, access, handle, elementPoint.View()));
            CHECK(isSet(elementPoint));

            PropertyPathFailure failure;
            PropertyPathBuffer count;
            parse("count", count);
            CHECK(PathIsOptionalSet(objects, access, handle, count.View(), &failure).Error() ==
                  ErrorCode::TypeMismatch);
            CHECK(failure.reason == PropertyPathError::NotOptional);
            PropertyPathBuffer slot;
            parse("slot", slot);
            CHECK(PathIsOptionalSet(objects, access, handle, slot.View(), &failure).Error() ==
                  ErrorCode::TypeMismatch);
            CHECK(failure.reason == PropertyPathError::NotOptional);
        }

        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// 읽기는 present를 늘 정한다: unset Optional을 읽어 present가 거짓이 된 FieldValue를 Optional이
// 아닌 값의 읽기(경로 읽기·CopyTargetValue)에 다시 써도 present는 참이 된다. Optional이 아닌 enum
// 자리의 쓰기 검사는 enum ID(TypeMismatch)를 present(InvalidArgument)보다 먼저 본다(헤더의 오류
// 순서).
TEST(OptionalField_ReadsResetPresentAndEnumIdComesFirst) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(allocator, types, {0x4f5046, 5});
        const auto access = objects.OwnerAccess();
        {
            auto lease = types.Acquire(TypeIdOf<OptionalHolder>());
            REQUIRE(lease);
            RootRef<OptionalHolder> holder = New(objects);
            PropertyPathBuffer count;
            PropertyPathBuffer x;
            REQUIRE(ParsePropertyPath(lease.Value(), "count", count));
            REQUIRE(ParsePropertyPath(lease.Value(), "presetPoint.x", x));
            FieldValue reused(allocator);
            REQUIRE(PathRead(objects, access, holder.Handle(), count.View(), reused));
            CHECK(!reused.present);
            REQUIRE(PathRead(objects, access, holder.Handle(), x.View(), reused));
            CHECK(reused.present && reused.kind == ValueKind::Int32 && reused.signedInteger == 3);
            reused.present = false;
            REQUIRE(detail::CopyTargetValue<ValueKind::UInt32>(uint32{4}, reused));
            CHECK(reused.present && reused.unsignedInteger == 4);
        }

        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    // Optional이 아닌 enum 필드에 다른 enum ID + present = false: ID가 먼저다(TypeMismatch). 같은
    // ID면 present 검사의 InvalidArgument다. Optional<enum>의 unset은 ID를 보지 않는다.
    const TypeId modeId{0x4f5046, 0x10};
    FieldSchema plain;
    plain.value = ValueType{ValueKind::Enum, modeId};
    FieldSchema optional;
    optional.value = ValueType{ValueKind::Optional, {}};
    optional.element = ValueType{ValueKind::Enum, modeId};
    FieldValue value(allocator);
    value.kind = ValueKind::Enum;
    value.type = TypeId{0x4f5046, 0x11};
    value.present = false;
    detail::FieldWriteBinding binding;
    binding.value = ValueType{ValueKind::Enum, modeId};
    detail::FieldWriteTarget target;
    target.actual = &plain;
    CHECK(ErrorOf(detail::ValidateFieldWrite(binding, target, value)) == ErrorCode::TypeMismatch);
    value.type = modeId;
    CHECK(ErrorOf(detail::ValidateFieldWrite(binding, target, value)) ==
          ErrorCode::InvalidArgument);
    value.type = TypeId{0x4f5046, 0x11};
    binding.optional = true;
    target.actual = &optional;
    CHECK(ErrorOf(detail::ValidateFieldWrite(binding, target, value)) == ErrorCode::None);
    CHECK(types.Shutdown());
}

namespace {

// 수동 함수표의 Optional 필드다(범용 codec EncodeThroughFieldValue·DecodeThroughFieldValue).
struct ManualOptional final : Object {
    Optional<uint32> count;
    Optional<String> text;
};

constexpr TypeId ManualOptionalType{0x4f5046, 0x20};
constexpr Guid ManualCount{0x4f5046, 0x21};
constexpr Guid ManualText{0x4f5046, 0x22};

template <auto Member>
RuntimeFieldOps ManualOps(Guid id) noexcept {
    RuntimeFieldOps ops;
    ops.fieldId = id;
    ops.read = [](ObjectRegistry& registry, const ObjectAccessContext& access, ObjectHandle object,
                  FieldValue& out) noexcept {
        return detail::ReadGeneratedField<ManualOptional>(registry, access, object, Member, out);
    };

    ops.write = [](ObjectRegistry& registry, const ObjectAccessContext& access, ObjectHandle object,
                   const FieldValue& value) noexcept {
        return detail::WriteGeneratedField<ManualOptional>(registry, access, object, Member, value);
    };

    ops.writeStaged = [](ConstructionContext& construction, ObjectHandle object,
                         const FieldValue& value) noexcept {
        return detail::WriteGeneratedStagedField<ManualOptional>(construction, object, Member,
                                                                 value);
    };

    ops.encode = &detail::EncodeThroughFieldValue;
    ops.decode = &detail::DecodeThroughFieldValue;
    return ops;
}

} // namespace

// 수동 함수표의 Optional<단일 값> 필드도 범용 codec으로 set·unset이 왕복한다(FieldValue의 present가
// wire present byte가 된다).
TEST(OptionalField_ManualFunctionTableRoundTrip) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    FieldSchema fields[2];
    fields[0].id = ManualCount;
    fields[0].name = Span<const char>("count", 5);
    fields[0].value = ValueType{ValueKind::Optional, {}};
    fields[0].element = ValueType{ValueKind::UInt32, {}};
    fields[1].id = ManualText;
    fields[1].name = Span<const char>("text", 4);
    fields[1].value = ValueType{ValueKind::Optional, {}};
    fields[1].element = ValueType{ValueKind::Utf8String, {}};
    const RuntimeFieldOps ops[] = {ManualOps<&ManualOptional::count>(ManualCount),
                                   ManualOps<&ManualOptional::text>(ManualText)};
    auto typeOps = MakeRuntimeTypeOps<ManualOptional>();
    typeOps.fields = Span<const RuntimeFieldOps>(ops, 2);
    typeOps.completeFieldLayout = true;
    REQUIRE(types.Register(
        {ManualOptionalType, 1, Span<const FieldSchema>(fields, 2), {"ManualOptional", 14}},
        Move(typeOps)));
    {
        ObjectRegistry objects(allocator, types, {0x4f5046, 6});
        const auto access = objects.OwnerAccess();
        auto created = objects.NewObject<ManualOptional>(access, ManualOptionalType);
        REQUIRE(created);
        {
            auto borrowed = objects.Resolve<ManualOptional>(access, created.Value().Handle());
            REQUIRE(borrowed);
            auto value = borrowed.Value().Get();
            REQUIRE(value);
            value.Value()->text.Emplace(String(objects.ObjectAllocator()));
            REQUIRE(value.Value()->text->TryAssign("manual"));
        }

        auto saved = SaveObjectGraph(allocator, objects, access, created.Value().Handle());
        REQUIRE(saved);
        auto loaded = LoadObjectGraph(allocator, objects, types, access, saved.Value().View());
        REQUIRE(loaded);
        {
            auto back = objects.Resolve<ManualOptional>(access, loaded.Value().Handle());
            REQUIRE(back);
            auto loadedValue = back.Value().Get();
            REQUIRE(loadedValue);
            CHECK(!loadedValue.Value()->count.HasValue());
            REQUIRE(loadedValue.Value()->text.HasValue());
            CHECK(loadedValue.Value()->text->View() == "manual");
        }

        created.Value().Reset();
        loaded.Value().Reset();
        REQUIRE(objects.Shutdown(access));
    }

    CHECK(types.Shutdown());
}
