#include "MapTestSupport.h"

#include <TestSupport/NFailAllocator.h>

// 리플렉션 B9f3 key 경로의 보충 시험이다(Docs/Design/ReflectionExtensions.ko.md 1.5·7.3·10.4절,
// B9f3 리뷰 반영): Flags enum 키 표기(`A+B`·`A|B`·빈 값), 값 종류별 removed·previous(String·Soft
// 이동·복사, enum type), ClassRef 값·Set MaxElements, Optional<값 struct> 안 Map, 있는 키는 넣기
// 준비(Name intern·ClassRef 등록 검사)를 건너뛴다, 순회 중 key 편집 Busy, Name 키 조회의 할당 0,
// 가득 찬 컨테이너의 모든 할당 실패 지점, 여러 key의 byte 합계 한도, Soft·ClassRef key 텍스트 hook.
using namespace rocks;
namespace map_path_edge_tests_file {

using namespace rocks::test::map;

namespace {

template <class T>
ErrorCode CodeOf(const Result<T>& result) noexcept {
    return result ? ErrorCode::None : result.Error();
}

ErrorCode WriteAt(ObjectRegistry& objects, ObjectHandle handle, const TypeLease& lease,
                  StringView path, const FieldValue& value, FieldValue* previous = nullptr,
                  PropertyPathFailure* failure = nullptr) noexcept {
    const PropertyPathBuffer parsed = PathText(lease, path);
    return ErrorOf(
        PathWrite(objects, objects.OwnerAccess(), handle, parsed.View(), value, previous, failure));
}

ErrorCode EraseAt(ObjectRegistry& objects, ObjectHandle handle, const TypeLease& lease,
                  StringView path, FieldValue* removed = nullptr) noexcept {
    const PropertyPathBuffer parsed = PathText(lease, path);
    return ErrorOf(
        PathEraseKey(objects, objects.OwnerAccess(), handle, parsed.View(), removed, nullptr));
}

Result<bool> InsertAt(ObjectRegistry& objects, ObjectHandle handle, const TypeLease& lease,
                      StringView path, PropertyPathFailure* failure = nullptr) noexcept {
    const PropertyPathBuffer parsed = PathText(lease, path);
    return PathInsertKey(objects, objects.OwnerAccess(), handle, parsed.View(), failure);
}

bool ContainsAt(ObjectRegistry& objects, ObjectHandle handle, const TypeLease& lease,
                StringView path) noexcept {
    const PropertyPathBuffer parsed = PathText(lease, path);
    auto found = PathContains(objects, objects.OwnerAccess(), handle, parsed.View());
    CHECK(found);
    return found && found.Value();
}

// 형식화한 문자열 경로가 expected인지다.
bool Formats(const TypeLease& lease, Allocator& allocator, PropertyPath path, StringView expected,
             PathKeyTextHook* hook = nullptr) noexcept {
    String text(allocator);
    return FormatPropertyPath(lease, path, text, hook).IsOk() && text.View() == expected;
}

} // namespace

// Flags enum 키: 입력은 `A+B`·`A|B`(대소문자 무시)와 빈 값(0)이고 출력은 선언 순 `+`다. 빈 이름
// 조각·모르는 이름은 InvalidKey, 이름 있는 열거자 조합으로 나타낼 수 없는 비트는 Format이
// Unsupported다.
TEST(MapPath_FlagsKeysUseCombinedNames) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(allocator, types, {0x4d5045, 1});
        auto lease = types.Acquire(TypeIdOf<MapPathHolder>());
        REQUIRE(lease);
        const TypeLease& type = lease.Value();
        RootRef<MapPathHolder> holder = New<MapPathHolder>(objects);
        const ObjectHandle handle = holder.Handle();
        REQUIRE(WriteAt(objects, handle, type, "byAccess[write|read]",
                        Scalar(allocator, ValueKind::UInt32, 3)) == ErrorCode::None);
        REQUIRE(WriteAt(objects, handle, type, "byAccess[]",
                        Scalar(allocator, ValueKind::UInt32, 0)) == ErrorCode::None);
        FieldValue out(allocator);
        const PropertyPathBuffer both = PathText(type, "byAccess[Read+Write]");
        REQUIRE(PathRead(objects, objects.OwnerAccess(), handle, both.View(), out));
        CHECK(out.unsignedInteger == 3);
        CHECK(Formats(type, allocator, both.View(), "byAccess[Read+Write]"));
        CHECK(Formats(type, allocator, PathText(type, "byAccess[EXEC|read]").View(),
                      "byAccess[Read+Exec]"));
        CHECK(Formats(type, allocator, PathText(type, "byAccess[]").View(), "byAccess[]"));
        With<MapPathHolder>(objects, handle, [](MapPathHolder& value) noexcept {
            CHECK(value.byAccess.Count() == 2);
            const uint32* combined = value.byAccess.Find(static_cast<MapAccess>(3));
            const uint32* zero = value.byAccess.Find(static_cast<MapAccess>(0));
            CHECK(combined && *combined == 3 && zero && *zero == 0);
        });

        PropertyPathBuffer path;
        PropertyPathFailure failure;
        CHECK(ErrorOf(ParsePropertyPath(type, "byAccess[Read+]", path, &failure)) ==
              ErrorCode::InvalidArgument);
        CHECK(failure.reason == PropertyPathError::InvalidKey);
        CHECK(ErrorOf(ParsePropertyPath(type, "byAccess[Read+Delete]", path, &failure)) ==
              ErrorCode::InvalidArgument);
        KeyPath keyed(allocator);
        PathKeyValue bits;
        bits.kind = ValueKind::Enum;
        bits.signedInteger = 8;
        MakeKeyPath(keyed, PathHolderField("byAccess"), bits);
        String text(allocator);
        CHECK(ErrorOf(FormatPropertyPath(type, keyed.View(), text)) == ErrorCode::Unsupported);
        holder.Reset();
        lease = Result<TypeLease>::Failure(ErrorCode::NotFound);
        REQUIRE(objects.Collect(objects.OwnerAccess()));
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// 값 종류별 옛 값: 단일 값 Map 항목 지우기의 removed는 String·Soft 값을 같은 allocator면
// 옮기고(버퍼가 객체 allocator 것) 다르면 복사한다. 없던 키를 넣은 previous는 enum이면 type에 그
// enum ID를 싣는다. ClassRef 값은 등록된 Base 파생이어야 하고(미등록 NotFound), Set의
// MaxElements(2)를 넘는 넣기는 CapacityExceeded(TooManyElements)다.
TEST(MapPath_RemovedAndPreviousCoverValueKinds) {
    SystemAllocator allocator;
    SystemAllocator other;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(allocator, types, {0x4d5045, 2});
        auto pathLease = types.Acquire(TypeIdOf<MapPathHolder>());
        auto holderLease = types.Acquire(TypeIdOf<MapHolder>());
        REQUIRE(pathLease && holderLease);
        const TypeLease& type = pathLease.Value();
        RootRef<MapPathHolder> holder = New<MapPathHolder>(objects);
        RootRef<MapHolder> mapHolder = New(objects);
        const ObjectHandle handle = holder.Handle();
        REQUIRE(WriteAt(objects, handle, type, "texts[1]", TextField(allocator, "one")) ==
                ErrorCode::None);
        REQUIRE(WriteAt(objects, handle, type, "texts[2]", TextField(allocator, "two")) ==
                ErrorCode::None);
        FieldValue same(objects.ObjectAllocator());
        REQUIRE(EraseAt(objects, handle, type, "texts[1]", &same) == ErrorCode::None);
        CHECK(same.kind == ValueKind::Utf8String && same.string.View() == "one" &&
              &same.string.GetAllocator() == &objects.ObjectAllocator());
        FieldValue copied(other);
        REQUIRE(EraseAt(objects, handle, type, "texts[2]", &copied) == ErrorCode::None);
        CHECK(copied.string.View() == "two" && &copied.string.GetAllocator() == &other);

        FieldValue soft(allocator);
        soft.kind = ValueKind::SoftObjectReference;
        soft.guid = Guid{0x4d5045, 7};
        REQUIRE(soft.string.TryAssign("Body.Socket"));
        REQUIRE(WriteAt(objects, handle, type, "softs[3]", soft) == ErrorCode::None);
        FieldValue removedSoft(objects.ObjectAllocator());
        REQUIRE(EraseAt(objects, handle, type, "softs[3]", &removedSoft) == ErrorCode::None);
        CHECK(removedSoft.kind == ValueKind::SoftObjectReference &&
              removedSoft.guid == (Guid{0x4d5045, 7}) &&
              removedSoft.string.View() == "Body.Socket");

        FieldValue previous(allocator);
        FieldValue color(allocator);
        color.kind = ValueKind::Enum;
        color.type = TypeIdOf<MapColor>();
        color.signedInteger = 2;
        REQUIRE(WriteAt(objects, mapHolder.Handle(), holderLease.Value(),
                        "guidColors[00000000-0000-0000-0000-000000000005]", color,
                        &previous) == ErrorCode::None);
        CHECK(previous.kind == ValueKind::Enum && !previous.present &&
              previous.type == TypeIdOf<MapColor>());

        FieldValue classValue(allocator);
        classValue.kind = ValueKind::ClassReference;
        classValue.guid = TypeIdOf<MapKeyDerived>();
        REQUIRE(WriteAt(objects, handle, type, "classValues[1]", classValue) == ErrorCode::None);
        classValue.guid = Guid{0x4d5045, 0x99};
        CHECK(WriteAt(objects, handle, type, "classValues[2]", classValue) == ErrorCode::NotFound);
        With<MapPathHolder>(objects, handle, [](MapPathHolder& value) noexcept {
            CHECK(value.classValues.Count() == 1 && value.texts.Count() == 0 &&
                  value.softs.Count() == 0);
        });

        REQUIRE(InsertAt(objects, handle, type, "limitedSet[1]"));
        REQUIRE(InsertAt(objects, handle, type, "limitedSet[2]"));
        PropertyPathFailure failure;
        auto third = InsertAt(objects, handle, type, "limitedSet[3]", &failure);
        CHECK(CodeOf(third) == ErrorCode::CapacityExceeded &&
              failure.reason == PropertyPathError::TooManyElements);
        auto again = InsertAt(objects, handle, type, "limitedSet[2]");
        CHECK(again && !again.Value());
        holder.Reset();
        mapHolder.Reset();
        pathLease = Result<TypeLease>::Failure(ErrorCode::NotFound);
        holderLease = Result<TypeLease>::Failure(ErrorCode::NotFound);
        REQUIRE(objects.Collect(objects.OwnerAccess()));
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// Optional<값 struct> 안 Map: unset이면 그 아래 key 경로는 NotFound(OptionalUnset)이고, 값을 만든
// 뒤에는 넣기·멤버 쓰기가 된다. 문자열 경로도 `maybeBag.points[k].x`로 닿는다.
TEST(MapPath_OptionalStructHoldsMaps) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(allocator, types, {0x4d5045, 3});
        auto lease = types.Acquire(TypeIdOf<MapPathHolder>());
        REQUIRE(lease);
        const TypeLease& type = lease.Value();
        RootRef<MapPathHolder> holder = New<MapPathHolder>(objects);
        const ObjectHandle handle = holder.Handle();
        PropertyPathFailure failure;
        auto unset = InsertAt(objects, handle, type, "maybeBag.points[1]", &failure);
        CHECK(CodeOf(unset) == ErrorCode::NotFound &&
              failure.reason == PropertyPathError::OptionalUnset);
        REQUIRE(PathEmplaceOptional(objects, objects.OwnerAccess(), handle,
                                    PathText(type, "maybeBag").View()));
        auto inserted = InsertAt(objects, handle, type, "maybeBag.points[1]");
        REQUIRE(inserted && inserted.Value());
        REQUIRE(WriteAt(objects, handle, type, "maybeBag.points[1].x",
                        Scalar(allocator, ValueKind::Int32, 6)) == ErrorCode::None);
        REQUIRE(InsertAt(objects, handle, type, "maybeBag.words[w]"));
        With<MapPathHolder>(objects, handle, [](MapPathHolder& value) noexcept {
            REQUIRE(value.maybeBag.HasValue());
            const MapPoint* point = value.maybeBag->points.Find(uint32{1});
            CHECK(point && point->x == 6 && value.maybeBag->words.Contains(StringView("w")));
        });

        holder.Reset();
        lease = Result<TypeLease>::Failure(ErrorCode::NotFound);
        REQUIRE(objects.Collect(objects.OwnerAccess()));
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// 있는 키의 교체·무동작 넣기는 넣기 준비를 건너뛴다: 가득 찬 pool(maxEntries)에서도 대소문자 변형
// Name 키의 교체와 이미 있는 Set 원소 넣기는 pool을 늘리지 않고 성공한다(새 이름은
// CapacityExceeded). 등록을 해제한 클래스의 ClassRef 키도 그 항목의 값 교체·읽기·지우기가 되고, 그
// 클래스로 새 항목을 넣는 것만 NotFound다.
TEST(MapPath_ExistingKeysSkipInsertionPreparation) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    NamePool names(allocator);
    NamePoolConfig config;
    config.maxEntries = 2;
    REQUIRE(names.Initialize(config));
    {
        ObjectRegistry objects(allocator, types, {0x4d5045, 4}, WithNames(&names));
        auto lease = types.Acquire(TypeIdOf<MapHolder>());
        REQUIRE(lease);
        const TypeLease& type = lease.Value();
        RootRef<MapHolder> holder = New(objects);
        const ObjectHandle handle = holder.Handle();
        REQUIRE(WriteAt(objects, handle, type, "perPlatform[Android]",
                        Scalar(allocator, ValueKind::Float32, 1)) == ErrorCode::None);
        REQUIRE(InsertAt(objects, handle, type, "tags[Hero]"));
        // pool이 가득 찼다(시험의 전제).
        REQUIRE(CodeOf(names.TryIntern("Other")) == ErrorCode::CapacityExceeded);
        const uint64 entries = names.Stats().entryCount;
        REQUIRE(WriteAt(objects, handle, type, "perPlatform[ANDROID]",
                        Scalar(allocator, ValueKind::Float32, 2)) == ErrorCode::None);
        auto present = InsertAt(objects, handle, type, "tags[HERO]");
        CHECK(present && !present.Value());
        CHECK(names.Stats().entryCount == entries);
        CHECK(WriteAt(objects, handle, type, "perPlatform[IOS]",
                      Scalar(allocator, ValueKind::Float32, 3)) == ErrorCode::CapacityExceeded);
        holder.Reset();
        lease = Result<TypeLease>::Failure(ErrorCode::NotFound);
        REQUIRE(objects.Collect(objects.OwnerAccess()));
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(names.Shutdown());
    {
        auto leaf = GeneratedType<MapKeyLeaf>::Register(types);
        REQUIRE(leaf);
        ObjectRegistry objects(allocator, types, {0x4d5045, 5});
        const auto access = objects.OwnerAccess();
        KeyPath path(allocator);
        // ClassRef 키 Map인 MapHolder::classNames로 본다(Name 값은 None만 — pool 없는 Registry).
        RootRef<MapHolder> mapHolder = New(objects);
        PathKeyValue classKey;
        classKey.kind = ValueKind::ClassReference;
        classKey.guid = TypeIdOf<MapKeyLeaf>();
        MakeKeyPath(path, HolderField("classNames"), classKey);
        FieldValue none(allocator);
        none.kind = ValueKind::Name;
        REQUIRE(PathWrite(objects, access, mapHolder.Handle(), path.View(), none));
        REQUIRE(types.Unregister(leaf.Value()));
        REQUIRE(PathWrite(objects, access, mapHolder.Handle(), path.View(), none));
        FieldValue out(allocator);
        REQUIRE(PathRead(objects, access, mapHolder.Handle(), path.View(), out));
        classKey.guid = Guid{0x4d5045, 0x77};
        KeyPath fresh(allocator);
        MakeKeyPath(fresh, HolderField("classNames"), classKey);
        CHECK(ErrorOf(PathWrite(objects, access, mapHolder.Handle(), fresh.View(), none)) ==
              ErrorCode::NotFound);
        REQUIRE(PathEraseKey(objects, access, mapHolder.Handle(), path.View()));
        mapHolder.Reset();
        REQUIRE(objects.Collect(access));
        REQUIRE(objects.Shutdown(access));
    }

    CHECK(types.Shutdown());
}

// 순회(ForEach) 중인 컨테이너의 key 쓰기(교체·넣기)·넣기·지우기·비우기는 Fatal이 아니라 Busy이고
// 상태는 그대로다. 조회·읽기는 된다.
TEST(MapPath_IterationMakesKeyEditsBusy) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(allocator, types, {0x4d5045, 6});
        auto lease = types.Acquire(TypeIdOf<MapHolder>());
        REQUIRE(lease);
        const TypeLease& type = lease.Value();
        RootRef<MapHolder> holder = New(objects);
        const ObjectHandle handle = holder.Handle();
        REQUIRE(WriteAt(objects, handle, type, "ratios[a]",
                        Scalar(allocator, ValueKind::Float32, 1)) == ErrorCode::None);
        REQUIRE(InsertAt(objects, handle, type, "words[a]"));
        ErrorCode codes[7] = {};
        bool contains = false;
        ErrorCode read = ErrorCode::DependencyFailed;
        With(objects, handle, [&](MapHolder& value) noexcept {
            value.ratios.ForEach([&](const String&, float&) noexcept {
                const FieldValue two = Scalar(allocator, ValueKind::Float32, 2);
                codes[0] = WriteAt(objects, handle, type, "ratios[a]", two);
                codes[1] = WriteAt(objects, handle, type, "ratios[n]", two);
                codes[2] = EraseAt(objects, handle, type, "ratios[a]");
                codes[3] = ErrorOf(PathClear(objects, objects.OwnerAccess(), handle,
                                             PathText(type, "ratios").View()));
                contains = ContainsAt(objects, handle, type, "ratios[a]");
                FieldValue out(allocator);
                read = ErrorOf(PathRead(objects, objects.OwnerAccess(), handle,
                                        PathText(type, "ratios[a]").View(), out));
            });
            value.words.ForEach([&](const String&) noexcept {
                codes[4] = CodeOf(InsertAt(objects, handle, type, "words[n]"));
                codes[5] = EraseAt(objects, handle, type, "words[a]");
                codes[6] = ErrorOf(PathClear(objects, objects.OwnerAccess(), handle,
                                             PathText(type, "words").View()));
            });
        });

        for (ErrorCode code : codes) {
            CHECK(code == ErrorCode::Busy);
        }

        CHECK(contains && read == ErrorCode::None);
        With(objects, handle, [](MapHolder& value) noexcept {
            REQUIRE(value.ratios.Count() == 1 && value.words.Count() == 1);
            CHECK(*value.ratios.Find(StringView("a")) == 1.0f &&
                  value.words.Contains(StringView("a")));
        });

        holder.Reset();
        lease = Result<TypeLease>::Failure(ErrorCode::NotFound);
        REQUIRE(objects.Collect(objects.OwnerAccess()));
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// pool에 없는 Name 키의 조회·읽기·지우기는 Registry·값·pool allocator 어느 것도 할당하지
// 않는다(세는 allocator로 할당 시도 0을 단언한다).
TEST(MapPath_NameLookupAllocatesNothing) {
    SystemAllocator system;
    test::NFailAllocator objectAllocator(system);
    test::NFailAllocator valueAllocator(system);
    test::NFailAllocator poolAllocator(system);
    TypeRegistry types(system);
    REQUIRE(RegisterFixture(types));
    NamePool names(poolAllocator);
    REQUIRE(names.Initialize());
    {
        ObjectRegistry objects(objectAllocator, types, {0x4d5045, 7}, WithNames(&names));
        auto lease = types.Acquire(TypeIdOf<MapHolder>());
        REQUIRE(lease);
        const TypeLease& type = lease.Value();
        RootRef<MapHolder> holder = New(objects);
        const ObjectHandle handle = holder.Handle();
        const PropertyPathBuffer missing = PathText(type, "perPlatform[Android]");
        const PropertyPathBuffer tag = PathText(type, "tags[Android]");
        FieldValue out(valueAllocator);
        FieldValue removed(valueAllocator);
        objectAllocator.DisableFailure();
        valueAllocator.DisableFailure();
        poolAllocator.DisableFailure();
        auto found = PathContains(objects, objects.OwnerAccess(), handle, missing.View());
        CHECK(found && !found.Value());
        CHECK(ErrorOf(PathRead(objects, objects.OwnerAccess(), handle, missing.View(), out)) ==
              ErrorCode::NotFound);
        CHECK(ErrorOf(PathEraseKey(objects, objects.OwnerAccess(), handle, missing.View(),
                                   &removed)) == ErrorCode::NotFound);
        auto tagged = PathContains(objects, objects.OwnerAccess(), handle, tag.View());
        CHECK(tagged && !tagged.Value());
        CHECK(objectAllocator.Attempts() == 0 && valueAllocator.Attempts() == 0 &&
              poolAllocator.Attempts() == 0);
        holder.Reset();
        lease = Result<TypeLease>::Failure(ErrorCode::NotFound);
        REQUIRE(objects.Collect(objects.OwnerAccess()));
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(names.Shutdown());
    CHECK(types.Shutdown());
}

// 가득 찬 컨테이너(용량 4에 4개 — 넣기가 저장소를 키운다)의 새 키 넣기에서 객체 allocator의 n번째
// 할당이 실패하면 OutOfMemory이고 컨테이너는 원소 수·용량·첫 항목 주소가 그대로다(단일 값 String 키
// Map·Set·값 struct Map·Soft 키 Map). 실패하지 않으면 넣는다.
TEST(MapPath_FullContainerAllocationFailureLeavesContainerUnchanged) {
    SystemAllocator system;
    test::NFailAllocator objectAllocator(system);
    TypeRegistry types(system);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(objectAllocator, types, {0x4d5045, 8});
        auto lease = types.Acquire(TypeIdOf<MapHolder>());
        REQUIRE(lease);
        const TypeLease& type = lease.Value();
        RootRef<MapHolder> probe = New(objects);
        RootRef<MapHolder> target = New(objects);
        const char* names[] = {"k0", "k1", "k2", "k3"};
        const ObjectHandle handles[] = {probe.Handle(), target.Handle()};
        for (ObjectHandle handle : handles) {
            With(objects, handle, [&](MapHolder& value) noexcept {
                Allocator& objectsAllocator = objects.ObjectAllocator();
                uint64 index = 0;
                for (const char* name : names) {
                    Put(value.ratios, MakeString(objectsAllocator, name), 1.0f);
                    REQUIRE(value.words.Add(MakeString(objectsAllocator, name)));
                    Put(value.points, MakeString(objectsAllocator, name),
                        MapPoint(objectsAllocator));
                    SoftObjectRef<> key(objectsAllocator);
                    REQUIRE(key.TryAssign(AssetId{Guid{0x4d5045, 1}}, name));
                    Put(value.softKeys, Move(key), index++);
                }
            });
        }

        struct State {
            usize count = 0;
            usize capacity = 0;
            const void* first = nullptr;
        };

        const auto stateOf = [&](ObjectHandle handle, int which) noexcept {
            State state;
            With(objects, handle, [&](MapHolder& value) noexcept {
                switch (which) {
                case 0:
                    state = {value.ratios.Count(), value.ratios.Capacity(),
                             &(*value.ratios.begin()).key};
                    break;
                case 1:
                    state = {value.words.Count(), value.words.Capacity(), &*value.words.begin()};
                    break;
                case 2:
                    state = {value.points.Count(), value.points.Capacity(),
                             &(*value.points.begin()).key};
                    break;
                default:
                    state = {value.softKeys.Count(), value.softKeys.Capacity(),
                             &(*value.softKeys.begin()).key};
                    break;
                }
            });

            return state;
        };

        KeyPath softPath(system);
        PathKeyValue soft;
        soft.kind = ValueKind::SoftObjectReference;
        soft.guid = Guid{0x4d5045, 1};
        soft.text = "fresh";
        MakeKeyPath(softPath, HolderField("softKeys"), soft);
        const auto run = [&](ObjectHandle handle, int which) noexcept {
            switch (which) {
            case 0:
                return WriteAt(objects, handle, type, "ratios[fresh]",
                               Scalar(system, ValueKind::Float32, 2));
            case 1:
                return CodeOf(InsertAt(objects, handle, type, "words[fresh]"));
            case 2:
                return CodeOf(InsertAt(objects, handle, type, "points[fresh]"));
            default:
                return ErrorOf(PathWrite(objects, objects.OwnerAccess(), handle, softPath.View(),
                                         Scalar(system, ValueKind::UInt64, 9)));
            }
        };

        for (int which = 0; which < 4; ++which) {
            REQUIRE(stateOf(probe.Handle(), which).capacity == 4);
            objectAllocator.DisableFailure();
            REQUIRE(run(probe.Handle(), which) == ErrorCode::None);
            const uint64 attempts = objectAllocator.Attempts();
            CHECK(attempts >= 2);
            const State before = stateOf(target.Handle(), which);
            for (uint64 attempt = 1; attempt <= attempts; ++attempt) {
                objectAllocator.FailOnAttempt(attempt);
                CHECK(run(target.Handle(), which) == ErrorCode::OutOfMemory);
                objectAllocator.DisableFailure();
                const State after = stateOf(target.Handle(), which);
                CHECK(after.count == before.count && after.capacity == before.capacity &&
                      after.first == before.first);
            }

            REQUIRE(run(target.Handle(), which) == ErrorCode::None);
            CHECK(stateOf(target.Handle(), which).count == 5);
        }

        probe.Reset();
        target.Reset();
        lease = Result<TypeLease>::Failure(ErrorCode::NotFound);
        REQUIRE(objects.Collect(objects.OwnerAccess()));
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// key byte 한도(MaxPropertyPathKeyBytes)는 한 경로의 key 합계다: 긴 key 둘(값 struct Map 안 Set)은
// 합계 안이면 해석되고, 넘으면 둘째 key에서 InvalidKey(offset은 그 `[`)다. key 하나는 여백
// (PathKeyTextPrefixBytes)과 함께 한도 안이어야 한다.
TEST(PropertyPath_MultipleKeysShareTheKeyByteLimit) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        auto lease = types.Acquire(TypeIdOf<MapPathHolder>());
        REQUIRE(lease);
        const auto pathOf = [&](usize first, usize second) noexcept {
            String text(allocator);
            bool built = text.TryAppend("nested[").IsOk();
            for (usize index = 0; built && index < first; ++index) {
                built = text.TryAppend("k").IsOk();
            }

            built = built && text.TryAppend("]").IsOk();
            if (second != 0) {
                built = built && text.TryAppend(".words[").IsOk();
                for (usize index = 0; built && index < second; ++index) {
                    built = text.TryAppend("w").IsOk();
                }

                built = built && text.TryAppend("]").IsOk();
            }

            CHECK(built);
            return text;
        };

        PropertyPathBuffer path;
        PropertyPathFailure failure;
        const String fits = pathOf(3000, 3000);
        REQUIRE(ParsePropertyPath(lease.Value(), fits.View(), path, &failure));
        CHECK(path.count == 2 && path.KeyCount() == 2);
        const String over = pathOf(3200, 3200);
        CHECK(ErrorOf(ParsePropertyPath(lease.Value(), over.View(), path, &failure)) ==
              ErrorCode::InvalidArgument);
        CHECK(failure.reason == PropertyPathError::InvalidKey && failure.step == 1 &&
              failure.offset == 7 + 3200 + 1 + 6);
        const usize single = MaxPropertyPathKeyBytes - PathKeyTextPrefixBytes;
        REQUIRE(ParsePropertyPath(lease.Value(), pathOf(single, 0).View(), path));
        CHECK(ErrorOf(ParsePropertyPath(lease.Value(), pathOf(single + 1, 0).View(), path,
                                        &failure)) == ErrorCode::InvalidArgument);
        CHECK(failure.reason == PropertyPathError::InvalidKey && failure.offset == 6);
    }

    CHECK(types.Shutdown());
}

namespace {

// 시험용 key 텍스트 hook이다: ClassRef 키는 "Derived"·"Base"(그 밖은 NotFound, "Wrong"은 다른
// 종류를 돌려준다), Soft 키는 고정 asset의 객체 경로 텍스트다.
struct TestKeyHook final : PathKeyTextHook {
    Result<PathKeyValue> ParseKeyText(const ValueType& keyType, StringView text) noexcept override {
        ++parses;
        PathKeyValue value;
        value.kind = keyType.kind;
        if (keyType.kind == ValueKind::SoftObjectReference) {
            value.guid = SoftAsset;
            value.text = text;
            return Result<PathKeyValue>::Success(value);
        }

        if (text == "Wrong") {
            value.kind = ValueKind::Guid;
            return Result<PathKeyValue>::Success(value);
        }

        if (text != "Derived" && text != "Base") {
            return Result<PathKeyValue>::Failure(ErrorCode::NotFound);
        }

        value.guid = text == "Derived" ? TypeIdOf<MapKeyDerived>() : TypeIdOf<MapKeyBase>();
        return Result<PathKeyValue>::Success(value);
    }

    Result<void> FormatKeyText(const ValueType& keyType, const PathKeyValue& value,
                               String& out) noexcept override {
        if (keyType.kind == ValueKind::SoftObjectReference) {
            return out.TryAssign(value.text);
        }

        return out.TryAssign(value.guid == TypeIdOf<MapKeyDerived>() ? "Derived" : "Base");
    }

    static constexpr Guid SoftAsset{0x4d5045, 0x50};
    uint32 parses = 0;
};

} // namespace

// Soft·ClassRef key 텍스트 hook: 해석·형식화가 hook을 지나 왕복하고(따옴표·escape는 경로 문법이
// 붙인다), hook의 오류는 그 코드(InvalidKey), 다른 키 종류를 돌려주면 TypeMismatch(InvalidKey),
// hook이 없으면 Unsupported(InvalidKey)다. 그 밖의 키 종류는 hook을 부르지 않는다.
TEST(PropertyPath_KeyTextHookParsesAndFormatsContextKeys) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        auto lease = types.Acquire(TypeIdOf<MapHolder>());
        REQUIRE(lease);
        const TypeLease& type = lease.Value();
        TestKeyHook hook;
        PropertyPathBuffer path;
        PropertyPathFailure failure;
        REQUIRE(ParsePropertyPath(type, "classNames[Derived]", path, &failure, &hook));
        auto key = ReadPathKey(path.Key(0));
        REQUIRE(key);
        CHECK(key.Value().kind == ValueKind::ClassReference &&
              key.Value().guid == TypeIdOf<MapKeyDerived>());
        CHECK(Formats(type, allocator, path.View(), "classNames[Derived]", &hook));
        REQUIRE(ParsePropertyPath(type, "softKeys[\"Body Socket\"]", path, &failure, &hook));
        auto soft = ReadPathKey(path.Key(0));
        REQUIRE(soft);
        CHECK(soft.Value().guid == TestKeyHook::SoftAsset && soft.Value().text == "Body Socket");
        CHECK(Formats(type, allocator, path.View(), "softKeys[\"Body Socket\"]", &hook));
        CHECK(!Formats(type, allocator, path.View(), "softKeys[\"Body Socket\"]"));

        CHECK(ErrorOf(ParsePropertyPath(type, "classNames[Missing]", path, &failure, &hook)) ==
              ErrorCode::NotFound);
        CHECK(failure.reason == PropertyPathError::InvalidKey && failure.offset == 10);
        CHECK(ErrorOf(ParsePropertyPath(type, "classNames[Wrong]", path, &failure, &hook)) ==
              ErrorCode::TypeMismatch);
        CHECK(failure.reason == PropertyPathError::InvalidKey);
        CHECK(ErrorOf(ParsePropertyPath(type, "classNames[Derived]", path, &failure)) ==
              ErrorCode::Unsupported);
        const uint32 before = hook.parses;
        REQUIRE(ParsePropertyPath(type, "counts[7]", path, &failure, &hook));
        CHECK(hook.parses == before);
    }

    CHECK(types.Shutdown());
}
} // namespace map_path_edge_tests_file
