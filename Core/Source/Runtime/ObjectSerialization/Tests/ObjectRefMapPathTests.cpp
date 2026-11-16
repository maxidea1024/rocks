#include <ReflectionTestFixtures/ReflectionTestFixtures.module.generated.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/NamePool.h>
#include <Object/ConstructionContext.h>
#include <Object/PropertyPath.h>
#include <TestSupport/Test.h>

// 리플렉션 B9f4 ObjectRefMap의 경로 key 단계 연산
// 시험이다(Docs/Design/ReflectionExtensions.ko.md 7.3절 "B9f4 주의"·7.4절): Registry API 갈래(고정
// 없이 SetMapReference·EraseMapReference·ClearMapReferences)의 쓰기(없으면 끝에 넣고 previous는
// present = false, 있으면 자리를 지키며 교체하고 previous는 옛 handle)· 읽기·지우기(removed는 지운
// handle)·조회·원소 수·비우기, 대상 타입 검사, MaxElements, InsertKey 거부, Name·enum 키(인라인
// struct 안 ObjectRefMap 포함), 경로로 넣은 값이 대상을 살리는지, 순회·encode 고정 중 쓰기 Busy.
using namespace rocks;
using reflection_fixture::RefMapDerived;
using reflection_fixture::RefMapHolder;
using reflection_fixture::RefMapOther;
using reflection_fixture::RefMapTarget;

namespace {

ErrorCode ErrorOf(const Result<void>& result) noexcept {
    return result ? ErrorCode::None : result.Error();
}

FieldValue Reference(Allocator& allocator, ObjectHandle target) noexcept {
    FieldValue value(allocator);
    value.kind = ValueKind::ObjectReference;
    value.reference = target;
    return value;
}

// RefMapHolder 하나를 root로 쥐는 경로 시험 문맥이다.
struct PathFixture {
    PathFixture(TypeRegistry& types, ObjectRegistry& registry) noexcept
        : objects(registry), lease(types.Acquire(TypeIdOf<RefMapHolder>())) {
        REQUIRE(lease);
        auto created =
            registry.NewObject<RefMapHolder>(registry.OwnerAccess(), TypeIdOf<RefMapHolder>());
        REQUIRE(created);
        holder = Move(created.Value());
    }

    PropertyPathBuffer Parse(StringView text) const noexcept {
        PropertyPathBuffer path;
        if (!ParsePropertyPath(lease.Value(), text, path)) {
            platform::Fatal("test property path parse failed");
        }

        return path;
    }

    ErrorCode Write(StringView text, const FieldValue& value, FieldValue* previous = nullptr,
                    PropertyPathFailure* failure = nullptr) noexcept {
        const PropertyPathBuffer path = Parse(text);
        return ErrorOf(PathWrite(objects, objects.OwnerAccess(), holder.Handle(), path.View(),
                                 value, previous, failure));
    }

    ErrorCode Read(StringView text, FieldValue& out,
                   PropertyPathFailure* failure = nullptr) noexcept {
        const PropertyPathBuffer path = Parse(text);
        return ErrorOf(
            PathRead(objects, objects.OwnerAccess(), holder.Handle(), path.View(), out, failure));
    }

    ErrorCode Erase(StringView text, FieldValue* removed = nullptr,
                    PropertyPathFailure* failure = nullptr) noexcept {
        const PropertyPathBuffer path = Parse(text);
        return ErrorOf(PathEraseKey(objects, objects.OwnerAccess(), holder.Handle(), path.View(),
                                    removed, failure));
    }

    usize Count(StringView text) noexcept {
        const PropertyPathBuffer path = Parse(text);
        auto count = PathCount(objects, objects.OwnerAccess(), holder.Handle(), path.View());
        CHECK(count);
        return count ? count.Value() : NumericLimits<usize>::Max();
    }

    bool Contains(StringView text) noexcept {
        const PropertyPathBuffer path = Parse(text);
        auto found = PathContains(objects, objects.OwnerAccess(), holder.Handle(), path.View());
        CHECK(found);
        return found && found.Value();
    }

    ObjectRegistry& objects;
    Result<TypeLease> lease;
    RootRef<RefMapHolder> holder;
};

RegistryOptions WithNames(NamePool& names) noexcept {
    RegistryOptions options;
    options.names = &names;
    return options;
}

} // namespace

// ObjectRefMap key 경로: 쓰기·읽기·지우기·조회·원소 수·비우기와 옛 값, 삽입 순서, 대상 타입
// (TypeMismatch — 컨테이너 불변), 상한(CapacityExceeded(TooManyElements)),
// InsertKey(InvalidArgument (InvalidValue)), 없는 키(NotFound(KeyNotFound)), Name·enum 키, 경로로
// 넣은 값이 대상을 살린다.
TEST(ObjectRefMapPath_KeyReadWriteEraseAndOrder) {
    SystemAllocator allocator;
    SystemAllocator values;
    TypeRegistry types(allocator);
    NamePool names(allocator);
    REQUIRE(names.Initialize());
    {
        ModuleRegistration registration(types, allocator);
        REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(registration));
        {
            ObjectRegistry objects(allocator, types, {0x524d50, 1}, WithNames(names));
            const auto access = objects.OwnerAccess();
            auto first = objects.NewObject<RefMapTarget>(access, TypeIdOf<RefMapTarget>());
            auto second = objects.NewObject<RefMapDerived>(access, TypeIdOf<RefMapDerived>());
            auto other = objects.NewObject<RefMapOther>(access, TypeIdOf<RefMapOther>());
            REQUIRE(first && second && other);
            const ObjectHandle one = first.Value().Handle();
            const ObjectHandle two = second.Value().Handle();
            {
                PathFixture fixture(types, objects);
                FieldValue previous(values);
                REQUIRE(fixture.Write("byString[b]", Reference(values, one), &previous) ==
                        ErrorCode::None);
                CHECK(previous.kind == ValueKind::ObjectReference && !previous.present);
                REQUIRE(fixture.Write("byString[a]", Reference(values, ObjectHandle{})) ==
                        ErrorCode::None);
                REQUIRE(fixture.Write("byString[b]", Reference(values, two), &previous) ==
                        ErrorCode::None);
                CHECK(previous.kind == ValueKind::ObjectReference && previous.present &&
                      previous.reference == one);
                FieldValue out(values);
                REQUIRE(fixture.Read("byString[b]", out) == ErrorCode::None);
                CHECK(out.kind == ValueKind::ObjectReference && out.reference == two);
                PropertyPathFailure failure;
                CHECK(fixture.Read("byString[zzz]", out, &failure) == ErrorCode::NotFound);
                CHECK(failure.reason == PropertyPathError::KeyNotFound);
                CHECK(fixture.Count("byString") == 2 && fixture.Contains("byString[a]") &&
                      !fixture.Contains("byString[c]"));
                {
                    auto borrowed = objects.Resolve<RefMapHolder>(access, fixture.holder.Handle());
                    REQUIRE(borrowed);
                    usize position = 0;
                    const StringView order[] = {"b", "a"};
                    for (auto entry : borrowed.Value().Get().Value()->byString.View()) {
                        CHECK(position < 2 && entry.key.View() == order[position]);
                        ++position;
                    }
                }

                // 대상 타입이 아니면 TypeMismatch이고 그대로다.
                CHECK(fixture.Write("byString[c]", Reference(values, other.Value().Handle())) ==
                      ErrorCode::TypeMismatch);
                CHECK(fixture.Count("byString") == 2);

                // 지우기: removed는 지운 handle, 없는 키는 NotFound(KeyNotFound).
                FieldValue removed(values);
                REQUIRE(fixture.Erase("byString[b]", &removed) == ErrorCode::None);
                CHECK(removed.kind == ValueKind::ObjectReference && removed.reference == two);
                CHECK(fixture.Erase("byString[b]", nullptr, &failure) == ErrorCode::NotFound);
                CHECK(failure.reason == PropertyPathError::KeyNotFound);

                // InsertKey는 단일 값 Map에 InvalidArgument(InvalidValue)다.
                const PropertyPathBuffer insertPath = fixture.Parse("byString[c]");
                CHECK(PathInsertKey(objects, access, fixture.holder.Handle(), insertPath.View(),
                                    &failure)
                          .Error() == ErrorCode::InvalidArgument);
                CHECK(failure.reason == PropertyPathError::InvalidValue);

                // 상한 2: 셋째 새 키는 CapacityExceeded(TooManyElements), 교체는 된다.
                REQUIRE(fixture.Write("limited[1]", Reference(values, one)) == ErrorCode::None);
                REQUIRE(fixture.Write("limited[2]", Reference(values, one)) == ErrorCode::None);
                CHECK(fixture.Write("limited[3]", Reference(values, one), nullptr, &failure) ==
                      ErrorCode::CapacityExceeded);
                CHECK(failure.reason == PropertyPathError::TooManyElements);
                REQUIRE(fixture.Write("limited[2]", Reference(values, two)) == ErrorCode::None);
                CHECK(fixture.Count("limited") == 2);

                // enum 키와 인라인 struct 안 Name 키 Map(넣기 준비가 이름을 intern한다).
                REQUIRE(fixture.Write("bySlot[Hand]", Reference(values, two)) == ErrorCode::None);
                REQUIRE(fixture.Write("bundle.byName[Socket]", Reference(values, one)) ==
                        ErrorCode::None);
                REQUIRE(fixture.Read("bundle.byName[socket]", out) == ErrorCode::None);
                CHECK(out.reference == one);
                CHECK(fixture.Count("bundle.byName") == 1);

                // 경로로 넣은 값이 대상을 살린다(교체로 빠진 값은 다른 경로가 없으면 회수된다).
                first.Value().Reset();
                second.Value().Reset();
                REQUIRE(objects.Collect(access));
                CHECK(objects.Resolve<RefMapTarget>(access, one).IsOk());
                CHECK(objects.Resolve<RefMapDerived>(access, two).IsOk());

                // 비우기.
                const PropertyPathBuffer clearPath = fixture.Parse("byString");
                REQUIRE(PathClear(objects, access, fixture.holder.Handle(), clearPath.View()));
                CHECK(fixture.Count("byString") == 0);
                fixture.holder.Reset();
                fixture.lease = Result<TypeLease>::Failure(ErrorCode::NotFound);
            }

            other.Value().Reset();
            REQUIRE(objects.Collect(access));
            CHECK(objects.Stats().liveObjects == 0);
            REQUIRE(objects.Shutdown(access));
        }

        REQUIRE(registration.Unregister());
    }

    CHECK(names.Shutdown());
    CHECK(types.Shutdown());
}

// 순회(View().ForEach) 중이거나 owner가 encode 고정 중이면 ObjectRefMap 경로 쓰기·지우기·비우기는
// Busy이고(Registry가 판정한다 — 고정 없이 Registry API를 쓰는 갈래) 읽기·조회는 된다.
TEST(ObjectRefMapPath_IterationAndEncodePinMakeWritesBusy) {
    SystemAllocator allocator;
    SystemAllocator values;
    TypeRegistry types(allocator);
    NamePool names(allocator);
    REQUIRE(names.Initialize());
    {
        ModuleRegistration registration(types, allocator);
        REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(registration));
        {
            ObjectRegistry objects(allocator, types, {0x524d50, 2}, WithNames(names));
            const auto access = objects.OwnerAccess();
            auto first = objects.NewObject<RefMapTarget>(access, TypeIdOf<RefMapTarget>());
            REQUIRE(first);
            const ObjectHandle one = first.Value().Handle();
            {
                PathFixture fixture(types, objects);
                REQUIRE(fixture.Write("byNumber[4]", Reference(values, one)) == ErrorCode::None);
                ErrorCode results[4] = {};
                bool readable = false;
                {
                    auto borrowed = objects.Resolve<RefMapHolder>(access, fixture.holder.Handle());
                    REQUIRE(borrowed);
                    RefMapHolder* holder = borrowed.Value().Get().Value();
                    holder->byNumber.View().ForEach(
                        [&](const uint32&, const ObjectHandle&) noexcept {
                            results[0] =
                                fixture.Write("byNumber[4]", Reference(values, ObjectHandle{}));
                            results[1] = fixture.Write("byNumber[5]", Reference(values, one));
                            results[2] = fixture.Erase("byNumber[4]");
                            const PropertyPathBuffer clearPath = fixture.Parse("byNumber");
                            results[3] = ErrorOf(PathClear(objects, access, fixture.holder.Handle(),
                                                           clearPath.View()));
                            FieldValue out(values);
                            readable = fixture.Read("byNumber[4]", out) == ErrorCode::None &&
                                       out.reference == one && fixture.Contains("byNumber[4]");
                        });
                }

                for (ErrorCode result : results) {
                    CHECK(result == ErrorCode::Busy);
                }

                CHECK(readable);
                {
                    auto pin = objects.PinForEncode(access, fixture.holder.Handle());
                    REQUIRE(pin);
                    CHECK(fixture.Write("byNumber[5]", Reference(values, one)) == ErrorCode::Busy);
                    CHECK(fixture.Erase("byNumber[4]") == ErrorCode::Busy);
                    CHECK(fixture.Count("byNumber") == 1);
                }

                REQUIRE(fixture.Erase("byNumber[4]") == ErrorCode::None);
                fixture.holder.Reset();
                fixture.lease = Result<TypeLease>::Failure(ErrorCode::NotFound);
            }

            first.Value().Reset();
            REQUIRE(objects.Collect(access));
            CHECK(objects.Stats().liveObjects == 0);
            REQUIRE(objects.Shutdown(access));
        }

        REQUIRE(registration.Unregister());
    }

    CHECK(names.Shutdown());
    CHECK(types.Shutdown());
}
