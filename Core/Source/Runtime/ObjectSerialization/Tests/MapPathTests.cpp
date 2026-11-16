#include "MapTestSupport.h"

#include <TestSupport/NFailAllocator.h>

// 리플렉션 B9f3 경로 key 단계의 Map·Set 경로 연산 시험이다(Docs/Design/ReflectionExtensions.ko.md
// 1.5·7.3절, 17절 B9f 경로 항목): key 읽기·쓰기(없으면 끝에 넣기, 있으면 자리 유지 교체)와 옛 값,
// 지우기·조회·원소 수·비우기, 값 struct의 PathInsertKey(기본 값 넣기·초기화)와 멤버 경로, Set 원소,
// Name 키의 할당 없는 조회와 삽입 intern, ClassRef·Soft 키, key·모양 오류와 ReadOnly, 할당 실패 시
// 컨테이너 불변.
using namespace rocks;
using namespace rocks::test::map;

namespace {

// MapHolder 하나를 root로 쥐는 시험 문맥이다(객체 allocator는 Registry allocator).
struct PathFixture {
    PathFixture(TypeRegistry& types, ObjectRegistry& registry) noexcept
        : objects(registry), lease(types.Acquire(TypeIdOf<MapHolder>())), holder(New(registry)) {
        REQUIRE(lease);
    }

    ObjectHandle Handle() const noexcept {
        return holder.Handle();
    }

    const TypeLease& Lease() const noexcept {
        return lease.Value();
    }

    ObjectRegistry& objects;
    Result<TypeLease> lease;
    RootRef<MapHolder> holder;
};

ErrorCode WriteCode(PathFixture& fixture, StringView path, const FieldValue& value,
                    FieldValue* previous = nullptr,
                    PropertyPathFailure* failure = nullptr) noexcept {
    const PropertyPathBuffer parsed = PathText(fixture.Lease(), path);
    return ErrorOf(PathWrite(fixture.objects, fixture.objects.OwnerAccess(), fixture.Handle(),
                             parsed.View(), value, previous, failure));
}

ErrorCode ReadCode(PathFixture& fixture, StringView path, FieldValue& out,
                   PropertyPathFailure* failure = nullptr) noexcept {
    const PropertyPathBuffer parsed = PathText(fixture.Lease(), path);
    return ErrorOf(PathRead(fixture.objects, fixture.objects.OwnerAccess(), fixture.Handle(),
                            parsed.View(), out, failure));
}

ErrorCode EraseCode(PathFixture& fixture, StringView path, FieldValue* removed = nullptr,
                    PropertyPathFailure* failure = nullptr) noexcept {
    const PropertyPathBuffer parsed = PathText(fixture.Lease(), path);
    return ErrorOf(PathEraseKey(fixture.objects, fixture.objects.OwnerAccess(), fixture.Handle(),
                                parsed.View(), removed, failure));
}

// PathContains·PathInsertKey 결과다(오류면 시험 실패와 거짓).
bool Contains(PathFixture& fixture, StringView path) noexcept {
    const PropertyPathBuffer parsed = PathText(fixture.Lease(), path);
    auto found = PathContains(fixture.objects, fixture.objects.OwnerAccess(), fixture.Handle(),
                              parsed.View());
    CHECK(found);
    return found && found.Value();
}

Result<bool> InsertKey(PathFixture& fixture, StringView path,
                       PropertyPathFailure* failure = nullptr) noexcept {
    const PropertyPathBuffer parsed = PathText(fixture.Lease(), path);
    return PathInsertKey(fixture.objects, fixture.objects.OwnerAccess(), fixture.Handle(),
                         parsed.View(), failure);
}

usize CountOf(PathFixture& fixture, StringView path) noexcept {
    const PropertyPathBuffer parsed = PathText(fixture.Lease(), path);
    auto count =
        PathCount(fixture.objects, fixture.objects.OwnerAccess(), fixture.Handle(), parsed.View());
    CHECK(count);
    return count ? count.Value() : NumericLimits<usize>::Max();
}

void Finish(ObjectRegistry& objects, PathFixture& fixture) noexcept {
    fixture.holder.Reset();
    fixture.lease = Result<TypeLease>::Failure(ErrorCode::NotFound);
    REQUIRE(objects.Collect(objects.OwnerAccess()));
    REQUIRE(objects.Shutdown(objects.OwnerAccess()));
}

} // namespace

// 단일 값 Map의 key 쓰기는 없던 키를 끝에 넣고(previous는 present = false) 있던 키는 자리를 지키며
// 바꾼다(previous는 옛 값). 읽기는 그 값, 없는 키는 NotFound(KeyNotFound)이고 out은 그대로다.
// 지우기는 옛 값을 removed로 돌려주고 남은 순서를 지키며, 지운 뒤 다시 넣은 키는 끝에 붙는다. 넣은
// 뒤 원소 수가 MaxElements를 넘으면 CapacityExceeded(TooManyElements)이고 Map은 그대로다. String
// 키·값은 객체 allocator로 만든다. weak 값·enum 키도 같은 경로다. 비우기 뒤 원소 수는 0이다. 단일
// 값 Map에 PathInsertKey는 InvalidArgument(InvalidValue)다.
TEST(MapPath_KeyReadWriteAndEraseKeepOrder) {
    SystemAllocator allocator;
    SystemAllocator values;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(allocator, types, {0x4d5041, 1});
        PathFixture fixture(types, objects);
        PropertyPathFailure failure;
        FieldValue previous(values);
        REQUIRE(WriteCode(fixture, "counts[30]", Scalar(values, ValueKind::Int32, -3), &previous) ==
                ErrorCode::None);
        CHECK(previous.kind == ValueKind::Int32 && !previous.present);
        REQUIRE(WriteCode(fixture, "counts[10]", Scalar(values, ValueKind::Int32, -1)) ==
                ErrorCode::None);
        REQUIRE(WriteCode(fixture, "counts[20]", Scalar(values, ValueKind::Int32, -2)) ==
                ErrorCode::None);
        REQUIRE(WriteCode(fixture, "counts[10]", Scalar(values, ValueKind::Int32, -10),
                          &previous) == ErrorCode::None);
        CHECK(previous.kind == ValueKind::Int32 && previous.present &&
              previous.signedInteger == -1);

        FieldValue out(values);
        REQUIRE(ReadCode(fixture, "counts[20]", out) == ErrorCode::None);
        CHECK(out.kind == ValueKind::Int32 && out.signedInteger == -2);
        CHECK(ReadCode(fixture, "counts[99]", out, &failure) == ErrorCode::NotFound);
        CHECK(failure.reason == PropertyPathError::KeyNotFound && out.signedInteger == -2);
        CHECK(Contains(fixture, "counts[30]") && !Contains(fixture, "counts[99]"));
        CHECK(CountOf(fixture, "counts") == 3);

        FieldValue removed(values);
        REQUIRE(EraseCode(fixture, "counts[30]", &removed) == ErrorCode::None);
        CHECK(removed.kind == ValueKind::Int32 && removed.signedInteger == -3);
        CHECK(EraseCode(fixture, "counts[30]", nullptr, &failure) == ErrorCode::NotFound);
        CHECK(failure.reason == PropertyPathError::KeyNotFound);
        REQUIRE(WriteCode(fixture, "counts[30]", Scalar(values, ValueKind::Int32, -30)) ==
                ErrorCode::None);
        With(objects, fixture.Handle(), [](MapHolder& value) noexcept {
            const uint32 keys[] = {10, 20, 30};
            const int32 stored[] = {-10, -2, -30};
            usize index = 0;
            for (auto entry : value.counts) {
                REQUIRE(index < 3);
                CHECK(entry.key == keys[index] && entry.value == stored[index]);
                ++index;
            }

            CHECK(index == 3);
        });

        // 값 종류가 다르면 TypeMismatch(InvalidValue)이고 Map은 그대로다.
        CHECK(WriteCode(fixture, "counts[40]", Scalar(values, ValueKind::UInt32, 1), nullptr,
                        &failure) == ErrorCode::TypeMismatch);
        CHECK(failure.reason == PropertyPathError::InvalidValue && CountOf(fixture, "counts") == 3);

        // MaxElements = 2: 셋째 키는 거부하고 있는 키의 교체는 받는다.
        REQUIRE(WriteCode(fixture, "limited[1]", Scalar(values, ValueKind::UInt32, 1)) ==
                ErrorCode::None);
        REQUIRE(WriteCode(fixture, "limited[2]", Scalar(values, ValueKind::UInt32, 2)) ==
                ErrorCode::None);
        CHECK(WriteCode(fixture, "limited[3]", Scalar(values, ValueKind::UInt32, 3), nullptr,
                        &failure) == ErrorCode::CapacityExceeded);
        CHECK(failure.reason == PropertyPathError::TooManyElements);
        REQUIRE(WriteCode(fixture, "limited[2]", Scalar(values, ValueKind::UInt32, 20)) ==
                ErrorCode::None);
        CHECK(CountOf(fixture, "limited") == 2 && !Contains(fixture, "limited[3]"));

        // String 값(enum 키)·String 키는 객체 allocator로 만들고, 교체의 previous는 옛 문자열이다.
        REQUIRE(WriteCode(fixture, "colorNames[Green]", TextField(values, "g")) == ErrorCode::None);
        REQUIRE(WriteCode(fixture, "colorNames[green]", TextField(values, "green"), &previous) ==
                ErrorCode::None);
        CHECK(previous.kind == ValueKind::Utf8String && previous.string.View() == "g");
        REQUIRE(WriteCode(fixture, "ratios[metal]", Scalar(values, ValueKind::Float32, 2)) ==
                ErrorCode::None);
        REQUIRE(ReadCode(fixture, "ratios[metal]", out) == ErrorCode::None);
        CHECK(out.kind == ValueKind::Float32 && out.float32 == 2.0f);
        With(objects, fixture.Handle(), [&](MapHolder& value) noexcept {
            const String* name = value.colorNames.Find(MapColor::Green);
            REQUIRE(name);
            CHECK(name->View() == "green" && &name->GetAllocator() == &objects.ObjectAllocator());
            REQUIRE(value.ratios.Count() == 1);
            for (auto entry : value.ratios) {
                CHECK(entry.key.View() == "metal" &&
                      &entry.key.GetAllocator() == &objects.ObjectAllocator());
            }
        });

        // weak 값은 같은 Registry의 생존 대상이다.
        FieldValue weak(values);
        weak.kind = ValueKind::WeakObjectReference;
        weak.reference = fixture.Handle();
        REQUIRE(WriteCode(fixture, "weak[1]", weak) == ErrorCode::None);
        REQUIRE(ReadCode(fixture, "weak[1]", out) == ErrorCode::None);
        CHECK(out.kind == ValueKind::WeakObjectReference && out.reference == fixture.Handle());

        // 단일 값 Map은 값 없이 넣을 수 없다.
        auto inserted = InsertKey(fixture, "counts[50]", &failure);
        CHECK(!inserted && inserted.Error() == ErrorCode::InvalidArgument &&
              failure.reason == PropertyPathError::InvalidValue);

        const PropertyPathBuffer counts = PathText(fixture.Lease(), "counts");
        REQUIRE(PathClear(objects, objects.OwnerAccess(), fixture.Handle(), counts.View()));
        CHECK(CountOf(fixture, "counts") == 0);
        Finish(objects, fixture);
    }

    CHECK(types.Shutdown());
}

// Map<K, 값 struct>: PathInsertKey가 기본 값을 끝에 넣고(true) 이미 있으면 자리를 지키며 기본
// 값으로 되돌린다(false). 멤버 경로 `points[k].x`가 그 값에 닿고 없는 키는 NotFound(KeyNotFound)다.
// 값 struct를 통째로 읽으면 Unsupported(AggregateValue), removed를 주고 지우면
// InvalidArgument(AggregateValue)다. struct 멤버 Map(`bag.points[k]`)과 원소 struct 안
// Map(`bags[0].points[k]`)도 같다.
TEST(MapPath_StructValuesInsertDefaultAndMemberPaths) {
    SystemAllocator allocator;
    SystemAllocator values;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(allocator, types, {0x4d5041, 2});
        PathFixture fixture(types, objects);
        PropertyPathFailure failure;
        auto inserted = InsertKey(fixture, "points[hero]");
        REQUIRE(inserted && inserted.Value());
        inserted = InsertKey(fixture, "points[side]");
        REQUIRE(inserted && inserted.Value());
        REQUIRE(WriteCode(fixture, "points[hero].x", Scalar(values, ValueKind::Int32, 5)) ==
                ErrorCode::None);
        REQUIRE(WriteCode(fixture, "points[hero].label", TextField(values, "h")) ==
                ErrorCode::None);
        FieldValue out(values);
        REQUIRE(ReadCode(fixture, "points[hero].x", out) == ErrorCode::None);
        CHECK(out.signedInteger == 5);
        CHECK(ReadCode(fixture, "points[ghost].x", out, &failure) == ErrorCode::NotFound);
        CHECK(failure.reason == PropertyPathError::KeyNotFound);
        CHECK(WriteCode(fixture, "points[ghost].x", Scalar(values, ValueKind::Int32, 1), nullptr,
                        &failure) == ErrorCode::NotFound);
        CHECK(failure.reason == PropertyPathError::KeyNotFound && CountOf(fixture, "points") == 2);
        CHECK(ReadCode(fixture, "points[hero]", out, &failure) == ErrorCode::Unsupported);
        CHECK(failure.reason == PropertyPathError::AggregateValue);

        // 이미 있는 키는 기본 값으로 되돌리고 자리를 지킨다.
        inserted = InsertKey(fixture, "points[hero]");
        REQUIRE(inserted && !inserted.Value());
        With(objects, fixture.Handle(), [&](MapHolder& value) noexcept {
            REQUIRE(value.points.Count() == 2);
            auto first = value.points.begin();
            CHECK((*first).key.View() == "hero" && (*first).value.x == 0 &&
                  (*first).value.label.View() == "" &&
                  &(*first).value.label.GetAllocator() == &objects.ObjectAllocator());
        });

        FieldValue removed(values);
        CHECK(EraseCode(fixture, "points[hero]", &removed, &failure) == ErrorCode::InvalidArgument);
        CHECK(failure.reason == PropertyPathError::AggregateValue);
        REQUIRE(EraseCode(fixture, "points[hero]") == ErrorCode::None);
        CHECK(CountOf(fixture, "points") == 1 && !Contains(fixture, "points[hero]"));

        // struct 멤버 Map과 원소 struct 안 Map.
        inserted = InsertKey(fixture, "bag.points[3]");
        REQUIRE(inserted && inserted.Value());
        REQUIRE(WriteCode(fixture, "bag.points[3].y", Scalar(values, ValueKind::Int32, -7)) ==
                ErrorCode::None);
        const PropertyPathBuffer bag0 = PathText(fixture.Lease(), "bags[0]");
        REQUIRE(PathInsertElement(objects, objects.OwnerAccess(), fixture.Handle(), bag0.View(),
                                  nullptr));
        inserted = InsertKey(fixture, "bags[0].points[4]");
        REQUIRE(inserted && inserted.Value());
        REQUIRE(WriteCode(fixture, "bags[0].points[4].x", Scalar(values, ValueKind::Int32, 9)) ==
                ErrorCode::None);
        inserted = InsertKey(fixture, "bags[0].words[w]");
        REQUIRE(inserted && inserted.Value());
        With(objects, fixture.Handle(), [](MapHolder& value) noexcept {
            const MapPoint* point = value.bag.points.Find(uint32{3});
            REQUIRE(point);
            CHECK(point->y == -7);
            REQUIRE(value.bags.Size() == 1);
            const MapPoint* nested = value.bags[0].points.Find(uint32{4});
            REQUIRE(nested);
            CHECK(nested->x == 9 && value.bags[0].words.Contains(StringView("w")));
        });

        Finish(objects, fixture);
    }

    CHECK(types.Shutdown());
}

// Set 원소: PathInsertKey가 끝에 넣고(true) 이미 있으면 무동작(false), PathEraseKey는 removed에
// 지운 원소를 담는다. 원소는 키라 값 읽기·쓰기는 TypeMismatch(NotMap)다. enum 원소의 미선언 값은
// InvalidArgument(InvalidKey), bool 원소는 on·off 표기도 받는다.
TEST(MapPath_SetInsertEraseAndContains) {
    SystemAllocator allocator;
    SystemAllocator values;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(allocator, types, {0x4d5041, 3});
        PathFixture fixture(types, objects);
        PropertyPathFailure failure;
        auto inserted = InsertKey(fixture, "words[b]");
        REQUIRE(inserted && inserted.Value());
        inserted = InsertKey(fixture, "words[a]");
        REQUIRE(inserted && inserted.Value());
        inserted = InsertKey(fixture, "words[b]");
        REQUIRE(inserted && !inserted.Value());
        CHECK(Contains(fixture, "words[a]") && !Contains(fixture, "words[z]"));
        CHECK(CountOf(fixture, "words") == 2);
        FieldValue out(values);
        CHECK(ReadCode(fixture, "words[a]", out, &failure) == ErrorCode::TypeMismatch);
        CHECK(failure.reason == PropertyPathError::NotMap);
        CHECK(WriteCode(fixture, "words[a]", TextField(values, "a"), nullptr, &failure) ==
              ErrorCode::TypeMismatch);
        CHECK(failure.reason == PropertyPathError::NotMap);

        FieldValue removed(values);
        REQUIRE(EraseCode(fixture, "words[b]", &removed) == ErrorCode::None);
        CHECK(removed.kind == ValueKind::Utf8String && removed.string.View() == "b" &&
              &removed.string.GetAllocator() == &values);
        inserted = InsertKey(fixture, "words[b]");
        REQUIRE(inserted && inserted.Value());
        With(objects, fixture.Handle(), [](MapHolder& value) noexcept {
            const char* order[] = {"a", "b"};
            usize index = 0;
            for (const String& word : value.words) {
                REQUIRE(index < 2);
                CHECK(word.View() == order[index++]);
            }

            CHECK(index == 2);
        });

        inserted = InsertKey(fixture, "colors[Red]");
        REQUIRE(inserted && inserted.Value());
        CHECK(Contains(fixture, "colors[red]"));
        KeyPath undeclared(values);
        PathKeyValue color;
        color.kind = ValueKind::Enum;
        color.signedInteger = 3;
        MakeKeyPath(undeclared, HolderField("colors"), color);
        auto found = PathContains(objects, objects.OwnerAccess(), fixture.Handle(),
                                  undeclared.View(), &failure);
        CHECK(!found && found.Error() == ErrorCode::InvalidArgument &&
              failure.reason == PropertyPathError::InvalidKey);

        inserted = InsertKey(fixture, "flags[on]");
        REQUIRE(inserted && inserted.Value());
        CHECK(Contains(fixture, "flags[TRUE]") && !Contains(fixture, "flags[off]"));
        const PropertyPathBuffer words = PathText(fixture.Lease(), "words");
        REQUIRE(PathClear(objects, objects.OwnerAccess(), fixture.Handle(), words.View()));
        CHECK(CountOf(fixture, "words") == 0);
        Finish(objects, fixture);
    }

    CHECK(types.Shutdown());
}

// Name 키 조회는 NamePool::TryFind다: pool에 없는 이름이면 조회·읽기·지우기가 pool을 늘리지 않고
// 없음이다. 쓰기·넣기는 내용 규칙(NUL 거부 — InvalidArgument(InvalidKey)) 뒤 intern한다. 비교는
// Core Name 규칙이라 대소문자 변형이 같은 항목이다. pool 없는 Registry에는 None 키만 있다(다른 키
// 쓰기는 Unsupported(InvalidKey)).
TEST(MapPath_NameKeysLookupWithoutInterning) {
    SystemAllocator allocator;
    SystemAllocator values;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    NamePool names(allocator);
    REQUIRE(names.Initialize());
    {
        ObjectRegistry objects(allocator, types, {0x4d5041, 4}, WithNames(&names));
        PathFixture fixture(types, objects);
        PropertyPathFailure failure;
        const NamePoolStats before = names.Stats();
        FieldValue out(values);
        CHECK(!Contains(fixture, "perPlatform[Android]"));
        CHECK(ReadCode(fixture, "perPlatform[Android]", out, &failure) == ErrorCode::NotFound);
        CHECK(failure.reason == PropertyPathError::KeyNotFound);
        CHECK(EraseCode(fixture, "perPlatform[Android]", nullptr, &failure) == ErrorCode::NotFound);
        CHECK(!Contains(fixture, "tags[Android]"));
        const NamePoolStats after = names.Stats();
        CHECK(after.entryCount == before.entryCount && after.storageBytes == before.storageBytes);

        REQUIRE(WriteCode(fixture, "perPlatform[Android]", Scalar(values, ValueKind::Float32, 1)) ==
                ErrorCode::None);
        CHECK(names.Stats().entryCount > before.entryCount);
        REQUIRE(ReadCode(fixture, "perPlatform[ANDROID]", out) == ErrorCode::None);
        CHECK(out.float32 == 1.0f && Contains(fixture, "perPlatform[android]"));
        REQUIRE(WriteCode(fixture, "perPlatform[android]", Scalar(values, ValueKind::Float32, 3)) ==
                ErrorCode::None);
        CHECK(CountOf(fixture, "perPlatform") == 1);
        REQUIRE(ReadCode(fixture, "perPlatform[Android]", out) == ErrorCode::None);
        CHECK(out.float32 == 3.0f);

        // 내용 규칙을 어기는 Name 키(NUL)는 넣지 않는다.
        KeyPath bad(values);
        PathKeyValue nul;
        nul.kind = ValueKind::Name;
        nul.text = StringView("a\0b", 3);
        MakeKeyPath(bad, HolderField("perPlatform"), nul);
        CHECK(ErrorOf(PathWrite(objects, objects.OwnerAccess(), fixture.Handle(), bad.View(),
                                Scalar(values, ValueKind::Float32, 1), nullptr, &failure)) ==
              ErrorCode::InvalidArgument);
        CHECK(failure.reason == PropertyPathError::InvalidKey &&
              CountOf(fixture, "perPlatform") == 1);

        auto inserted = InsertKey(fixture, "tags[Hero]");
        REQUIRE(inserted && inserted.Value());
        CHECK(Contains(fixture, "tags[HERO]"));
        REQUIRE(EraseCode(fixture, "perPlatform[ANDROID]") == ErrorCode::None);
        CHECK(CountOf(fixture, "perPlatform") == 0);
        Finish(objects, fixture);
    }

    {
        // pool 없는 Registry: None 키만 쓸 수 있다.
        ObjectRegistry objects(allocator, types, {0x4d5041, 5});
        PathFixture fixture(types, objects);
        PropertyPathFailure failure;
        CHECK(!Contains(fixture, "perPlatform[Android]"));
        CHECK(WriteCode(fixture, "perPlatform[Android]", Scalar(values, ValueKind::Float32, 1),
                        nullptr, &failure) == ErrorCode::Unsupported);
        CHECK(failure.reason == PropertyPathError::InvalidKey);
        REQUIRE(WriteCode(fixture, "perPlatform[None]", Scalar(values, ValueKind::Float32, 2)) ==
                ErrorCode::None);
        CHECK(Contains(fixture, "perPlatform[]") && CountOf(fixture, "perPlatform") == 1);
        Finish(objects, fixture);
    }

    CHECK(names.Shutdown());
    CHECK(types.Shutdown());
}

// ClassRef 키를 넣을 때는 값과 같이 등록된 클래스이고 스키마 Base의 파생이어야 한다(미등록
// NotFound· 무관한 클래스 InvalidArgument — 사유 InvalidKey). 조회는 등록을 보지 않는다(없으면
// 없음). Soft 키는 할당 없는 view로 찾고 넣을 때 객체 allocator로 만든다. Soft null 규칙을 어기는
// wire는 InvalidArgument(InvalidKey)다.
TEST(MapPath_ClassRefAndSoftKeys) {
    SystemAllocator allocator;
    SystemAllocator values;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    NamePool names(allocator);
    REQUIRE(names.Initialize());
    {
        ObjectRegistry objects(allocator, types, {0x4d5041, 6}, WithNames(&names));
        PathFixture fixture(types, objects);
        const auto access = objects.OwnerAccess();
        PropertyPathFailure failure;
        auto hero = names.TryIntern("Hero");
        REQUIRE(hero);
        FieldValue name(values);
        name.kind = ValueKind::Name;
        name.name = hero.Value();

        KeyPath path(values);
        PathKeyValue key;
        key.kind = ValueKind::ClassReference;
        key.guid = TypeIdOf<MapKeyDerived>();
        MakeKeyPath(path, HolderField("classNames"), key);
        REQUIRE(PathWrite(objects, access, fixture.Handle(), path.View(), name));
        FieldValue out(values);
        REQUIRE(PathRead(objects, access, fixture.Handle(), path.View(), out));
        CHECK(out.kind == ValueKind::Name && out.name == hero.Value());

        key.guid = Guid{0x4d5041, 0x99};
        MakeKeyPath(path, HolderField("classNames"), key);
        CHECK(ErrorOf(PathWrite(objects, access, fixture.Handle(), path.View(), name, nullptr,
                                &failure)) == ErrorCode::NotFound);
        CHECK(failure.reason == PropertyPathError::InvalidKey);
        CHECK(ErrorOf(PathRead(objects, access, fixture.Handle(), path.View(), out, &failure)) ==
              ErrorCode::NotFound);
        CHECK(failure.reason == PropertyPathError::KeyNotFound);
        key.guid = TypeIdOf<MapHolder>();
        MakeKeyPath(path, HolderField("classNames"), key);
        CHECK(ErrorOf(PathWrite(objects, access, fixture.Handle(), path.View(), name, nullptr,
                                &failure)) == ErrorCode::InvalidArgument);
        CHECK(failure.reason == PropertyPathError::InvalidKey);
        key.guid = Guid{};
        MakeKeyPath(path, HolderField("classNames"), key);
        REQUIRE(PathWrite(objects, access, fixture.Handle(), path.View(), name));
        CHECK(CountOf(fixture, "classNames") == 2);

        key.guid = TypeIdOf<MapKeyBase>();
        MakeKeyPath(path, HolderField("classes"), key);
        auto inserted = PathInsertKey(objects, access, fixture.Handle(), path.View());
        REQUIRE(inserted && inserted.Value());

        PathKeyValue soft;
        soft.kind = ValueKind::SoftObjectReference;
        soft.guid = Guid{0x4d5041, 3};
        soft.text = "Body.Socket";
        MakeKeyPath(path, HolderField("softKeys"), soft);
        FieldValue count(values);
        count.kind = ValueKind::UInt64;
        count.unsignedInteger = NumericLimits<uint64>::Max();
        REQUIRE(PathWrite(objects, access, fixture.Handle(), path.View(), count));
        REQUIRE(PathRead(objects, access, fixture.Handle(), path.View(), out));
        CHECK(out.kind == ValueKind::UInt64 && out.unsignedInteger == NumericLimits<uint64>::Max());
        With(objects, fixture.Handle(), [&](MapHolder& value) noexcept {
            REQUIRE(value.softKeys.Count() == 1);
            for (auto entry : value.softKeys) {
                CHECK(entry.key.objectPath.View() == "Body.Socket" &&
                      &entry.key.objectPath.GetAllocator() == &objects.ObjectAllocator());
            }

            CHECK(value.softKeys.Contains(
                SoftObjectRefView{AssetId{Guid{0x4d5041, 3}}, "Body.Socket"}));
        });

        // asset 0인데 경로가 있는 Soft 키 wire(AppendPathKey가 만들지 않는 값)는 key 해석이
        // 거부한다.
        uint8 broken[16 + 8 + 1] = {};
        broken[16] = 1;
        broken[24] = 'x';
        path.keys[0] = PathKey{ValueKind::SoftObjectReference, Span<const uint8>(broken, 25)};
        CHECK(ErrorOf(PathRead(objects, access, fixture.Handle(), path.View(), out, &failure)) ==
              ErrorCode::InvalidArgument);
        CHECK(failure.reason == PropertyPathError::InvalidKey);
        Finish(objects, fixture);
    }

    CHECK(names.Shutdown());
    CHECK(types.Shutdown());
}

// 모양·key 오류: Array 필드의 key 단계는 TypeMismatch(NotKeyed), Map의 index 단계는
// TypeMismatch(NotArray), 키 종류가 다르면 TypeMismatch(InvalidKey), 정규형이 아닌 wire·미선언 enum
// 키는 InvalidArgument(InvalidKey), keys 밖의 Key 단계는 InvalidArgument, key 없는 PathContains는
// InvalidArgument(KeyRequired), key 있는 PathCount는 InvalidArgument(IndexNotAllowed), key 단계의
// PathInsertElement는 InvalidArgument(IndexRequired)다. ReadOnly 필드(struct 필드의 ReadOnly는 멤버
// Map에 전파)는 key 쓰기·넣기·지우기·비우기가 Unsupported(ReadOnly)이고 조회·원소 수는 된다.
TEST(MapPath_KeyAndShapeErrors) {
    SystemAllocator allocator;
    SystemAllocator values;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    REQUIRE(GeneratedType<reflection_fixture::MapLockedHolder>::Register(types));
    {
        ObjectRegistry objects(allocator, types, {0x4d5041, 7});
        PathFixture fixture(types, objects);
        const auto access = objects.OwnerAccess();
        const ObjectHandle handle = fixture.Handle();
        PropertyPathFailure failure;
        FieldValue out(values);
        KeyPath path(values);
        PathKeyValue key;
        key.kind = ValueKind::UInt32;
        key.unsignedInteger = 1;
        MakeKeyPath(path, HolderField("bags"), key);
        CHECK(ErrorOf(PathRead(objects, access, handle, path.View(), out, &failure)) ==
              ErrorCode::TypeMismatch);
        CHECK(failure.reason == PropertyPathError::NotKeyed);

        const PathStep indexed[] = {{HolderField("counts"), 0, PathSelector::Index}};
        CHECK(ErrorOf(PathRead(objects, access, handle, PropertyPath{indexed}, out, &failure)) ==
              ErrorCode::TypeMismatch);
        CHECK(failure.reason == PropertyPathError::NotArray);

        PathKeyValue text;
        text.kind = ValueKind::Utf8String;
        text.text = "1";
        MakeKeyPath(path, HolderField("counts"), text);
        CHECK(ErrorOf(PathRead(objects, access, handle, path.View(), out, &failure)) ==
              ErrorCode::TypeMismatch);
        CHECK(failure.reason == PropertyPathError::InvalidKey);

        const uint8 shortKey[3] = {1, 0, 0};
        path.keys[0] = PathKey{ValueKind::UInt32, Span<const uint8>(shortKey, 3)};
        CHECK(ErrorOf(PathRead(objects, access, handle, path.View(), out, &failure)) ==
              ErrorCode::InvalidArgument);
        CHECK(failure.reason == PropertyPathError::InvalidKey);

        PathKeyValue color;
        color.kind = ValueKind::Enum;
        color.signedInteger = 3;
        MakeKeyPath(path, HolderField("colorNames"), color);
        CHECK(ErrorOf(PathRead(objects, access, handle, path.View(), out, &failure)) ==
              ErrorCode::InvalidArgument);
        CHECK(failure.reason == PropertyPathError::InvalidKey);

        const PathStep outside[] = {{HolderField("counts"), 1, PathSelector::Key}};
        CHECK(ErrorOf(PathRead(objects, access, handle, PropertyPath{outside, path.keys}, out)) ==
              ErrorCode::InvalidArgument);

        const PropertyPathBuffer counts = PathText(fixture.Lease(), "counts");
        auto found = PathContains(objects, access, handle, counts.View(), &failure);
        CHECK(!found && found.Error() == ErrorCode::InvalidArgument &&
              failure.reason == PropertyPathError::KeyRequired);
        const PropertyPathBuffer entry = PathText(fixture.Lease(), "counts[1]");
        auto count = PathCount(objects, access, handle, entry.View(), &failure);
        CHECK(!count && count.Error() == ErrorCode::InvalidArgument &&
              failure.reason == PropertyPathError::IndexNotAllowed);
        const FieldValue one = Scalar(values, ValueKind::Int32, 1);
        CHECK(ErrorOf(PathInsertElement(objects, access, handle, entry.View(), &one, &failure)) ==
              ErrorCode::InvalidArgument);
        CHECK(failure.reason == PropertyPathError::IndexRequired);
        Finish(objects, fixture);
    }

    {
        ObjectRegistry objects(allocator, types, {0x4d5041, 8});
        const auto access = objects.OwnerAccess();
        auto lease = types.Acquire(TypeIdOf<reflection_fixture::MapLockedHolder>());
        REQUIRE(lease);
        auto created = objects.NewObject<reflection_fixture::MapLockedHolder>(
            access, TypeIdOf<reflection_fixture::MapLockedHolder>());
        REQUIRE(created);
        RootRef<reflection_fixture::MapLockedHolder> locked = Move(created.Value());
        const ObjectHandle handle = locked.Handle();
        PropertyPathFailure failure;
        const PropertyPathBuffer entry = PathText(lease.Value(), "locked[1]");
        CHECK(ErrorOf(PathWrite(objects, access, handle, entry.View(),
                                Scalar(values, ValueKind::UInt32, 1), nullptr, &failure)) ==
              ErrorCode::Unsupported);
        CHECK(failure.reason == PropertyPathError::ReadOnly);
        CHECK(ErrorOf(PathEraseKey(objects, access, handle, entry.View(), nullptr, &failure)) ==
              ErrorCode::Unsupported);
        CHECK(failure.reason == PropertyPathError::ReadOnly);
        const PropertyPathBuffer word = PathText(lease.Value(), "lockedWords[a]");
        auto inserted = PathInsertKey(objects, access, handle, word.View(), &failure);
        CHECK(!inserted && inserted.Error() == ErrorCode::Unsupported &&
              failure.reason == PropertyPathError::ReadOnly);
        const PropertyPathBuffer member = PathText(lease.Value(), "lockedBag.points[2]");
        inserted = PathInsertKey(objects, access, handle, member.View(), &failure);
        CHECK(!inserted && inserted.Error() == ErrorCode::Unsupported &&
              failure.reason == PropertyPathError::ReadOnly);
        const PropertyPathBuffer all = PathText(lease.Value(), "locked");
        CHECK(ErrorOf(PathClear(objects, access, handle, all.View(), &failure)) ==
              ErrorCode::Unsupported);
        CHECK(failure.reason == PropertyPathError::ReadOnly);
        auto found = PathContains(objects, access, handle, entry.View());
        CHECK(found && !found.Value());
        auto count = PathCount(objects, access, handle, all.View());
        CHECK(count && count.Value() == 0);
        locked.Reset();
        lease = Result<TypeLease>::Failure(ErrorCode::NotFound);
        REQUIRE(objects.Collect(access));
        REQUIRE(objects.Shutdown(access));
    }

    CHECK(types.Shutdown());
}

// 할당 실패: 없던 키를 넣는 경로 쓰기(String 키 Map — 키 복사·용량 예약), 값 String을 만드는 쓰기
// (enum 키 Map), Set 원소 넣기에서 객체 allocator의 n번째 할당이 실패하면 그 연산은 OutOfMemory이고
// 컨테이너는 내용·순서가 그대로다. 실패하지 않으면 넣는다.
TEST(MapPath_AllocationFailureLeavesContainerUnchanged) {
    SystemAllocator allocator;
    SystemAllocator values;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    test::NFailAllocator objectAllocator(allocator);
    {
        ObjectRegistry objects(objectAllocator, types, {0x4d5041, 9});
        PathFixture probe(types, objects);
        PathFixture target(types, objects);
        // 두 객체를 같은 상태로 만든다(probe가 할당 횟수를 재고 target이 실패를 받는다).
        PathFixture* fixtures[] = {&probe, &target};
        for (PathFixture* fixture : fixtures) {
            REQUIRE(WriteCode(*fixture, "ratios[a]", Scalar(values, ValueKind::Float32, 1)) ==
                    ErrorCode::None);
            REQUIRE(WriteCode(*fixture, "colorNames[Red]", TextField(values, "r")) ==
                    ErrorCode::None);
            REQUIRE(InsertKey(*fixture, "words[a]"));
        }

        struct Case {
            const char* path;
            bool set;
        };

        const Case cases[] = {
            {"ratios[metal]", false}, {"colorNames[Blue]", false}, {"words[metal]", true}};
        for (const Case& item : cases) {
            const auto run = [&](PathFixture& fixture) noexcept {
                if (item.set) {
                    auto inserted = InsertKey(fixture, item.path);
                    return inserted ? ErrorCode::None : inserted.Error();
                }

                const FieldValue value = item.path[0] == 'r' ? Scalar(values, ValueKind::Float32, 2)
                                                             : TextField(values, "blue");
                return WriteCode(fixture, item.path, value);
            };

            objectAllocator.DisableFailure();
            REQUIRE(run(probe) == ErrorCode::None);
            const uint64 attempts = objectAllocator.Attempts();
            CHECK(attempts >= 1);
            for (uint64 attempt = 1; attempt <= attempts; ++attempt) {
                objectAllocator.FailOnAttempt(attempt);
                CHECK(run(target) == ErrorCode::OutOfMemory);
                objectAllocator.DisableFailure();
                CHECK(!Contains(target, item.path));
                With(objects, target.Handle(), [](MapHolder& value) noexcept {
                    CHECK(value.ratios.Count() == 1 && value.ratios.Contains(StringView("a")));
                    CHECK(value.colorNames.Count() == 1 &&
                          value.colorNames.Contains(MapColor::Red));
                    CHECK(value.words.Count() == 1 && value.words.Contains(StringView("a")));
                });
            }

            objectAllocator.DisableFailure();
            REQUIRE(run(target) == ErrorCode::None);
            CHECK(Contains(target, item.path));
            // 다음 사례를 위해 target을 처음 상태로 돌린다(probe도 같다).
            REQUIRE(EraseCode(target, item.path) == ErrorCode::None);
            REQUIRE(EraseCode(probe, item.path) == ErrorCode::None);
        }

        target.holder.Reset();
        target.lease = Result<TypeLease>::Failure(ErrorCode::NotFound);
        Finish(objects, probe);
    }

    CHECK(types.Shutdown());
}
