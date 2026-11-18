#include <Base/TypeTraits.h>
#include <ReflectionTestFixtures/ReflectionTestFixtures.module.generated.h>
#include <Core/Memory/SystemAllocator.h>
#include <Object/ValueCopy.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// 리플렉션 값의 타입 무관 깊은 복사·교환 시험이다(리플렉션 B9i3c0 —
// Docs/Design/ReflectionExtensions.ko.md 10.2절 "구현(B9i3c0)에서 정한 점"): 종류별 깊은 복사(새
// 버퍼·같은 내용), 할당 실패 때 destination·source 불변, 교환의 무할당(할당 시도 계수), 생성 struct
// 복사의 NATIVE 멤버 포함과 참조 struct의 복사 없음.
using namespace rocks;
using reflection_fixture::StructHolder;
using reflection_fixture::StructLeaf;
using reflection_fixture::StructMiddle;

static_assert(detail::ValueCopy<Name>::Supported);
static_assert(detail::ValueCopy<WeakObjectRef<Object>>::Supported);
static_assert(detail::ValueCopy<Guid>::Supported);
static_assert(detail::ValueCopy<String>::Supported);
static_assert(detail::ValueCopy<Array<StructLeaf>>::Supported);
static_assert(detail::ValueCopy<OrderedMap<String, StructLeaf>>::Supported);
static_assert(detail::ValueCopy<Optional<StructLeaf>>::Supported);
static_assert(detail::ValueCopy<String[2]>::Supported);
static_assert(!detail::ValueCopy<ObjectRef<Object>>::Supported);
static_assert(!detail::ValueCopy<ObjectRefArray<Object>>::Supported);
static_assert(!detail::ValueCopy<StructHolder>::Supported);
static_assert(GeneratedType<StructMiddle>::ValueCopyable);
static_assert(!GeneratedType<StructHolder>::ValueCopyable);

namespace {

StructLeaf MakeLeaf(Allocator& allocator, uint32 count, StringView label) noexcept {
    StructLeaf leaf(allocator);
    leaf.count = count;
    CHECK(leaf.label.TryAssign(label));
    return leaf;
}

void FillMap(OrderedMap<String, StructLeaf>& map, Allocator& allocator) noexcept {
    const StringView keys[] = {"alpha", "beta", "gamma"};
    for (usize index = 0; index < 3; ++index) {
        String key(allocator);
        CHECK(key.TryAssign(keys[index]));
        CHECK(map.Insert(Move(key),
                         MakeLeaf(allocator, static_cast<uint32>(index + 1), keys[index])));
    }
}

bool SameLeaf(const StructLeaf& left, const StructLeaf& right) noexcept {
    return left.count == right.count && left.label.View() == right.label.View() &&
           left.group == right.group;
}

using LeafMap = OrderedMap<String, StructLeaf>;

// 키 순서와 값이 같은지다(StructLeaf에는 operator==가 없다).
bool SameMap(const LeafMap& left, const LeafMap& right) noexcept {
    if (left.Count() != right.Count()) {
        return false;
    }

    bool same = true;
    usize position = 0;
    left.ForEach([&](const String& key, const StructLeaf& value) noexcept {
        usize other = 0;
        bool found = false;
        right.ForEach([&](const String& rightKey, const StructLeaf& rightValue) noexcept {
            if (other++ == position) {
                found = rightKey.View() == key.View() && SameLeaf(rightValue, value);
            }
        });
        same = same && found;
        ++position;
    });
    return same;
}

} // namespace

TEST(ValueCopy_DeepCopiesEachKind) {
    SystemAllocator allocator;

    String text(allocator);
    CHECK(text.TryAssign("hello"));
    String textCopy(allocator);
    CHECK(detail::TryCopyValue(textCopy, text, allocator));
    CHECK(textCopy.View() == "hello");
    CHECK(textCopy.View().Data() != text.View().Data());

    Array<String> words(allocator);
    const StringView wordTexts[] = {"a", "bc"};
    for (StringView word : wordTexts) {
        String element(allocator);
        CHECK(element.TryAssign(word));
        CHECK(words.TryPushBack(Move(element)));
    }

    Array<String> wordsCopy(allocator);
    CHECK(detail::TryCopyValue(wordsCopy, words, allocator));
    CHECK(wordsCopy.Size() == 2 && wordsCopy[1].View() == "bc");
    CHECK(wordsCopy[1].View().Data() != words[1].View().Data());

    OrderedMap<String, StructLeaf> map(allocator);
    FillMap(map, allocator);
    OrderedMap<String, StructLeaf> mapCopy(allocator);
    CHECK(detail::TryCopyValue(mapCopy, map, allocator));
    CHECK(mapCopy.Count() == 3);
    usize visited = 0;
    mapCopy.ForEach([&](const String& key, const StructLeaf& value) noexcept {
        const StructLeaf* original = map.Find(key);
        CHECK(original && SameLeaf(*original, value));
        CHECK(original->label.View().Data() != value.label.View().Data());
        ++visited;
    });
    CHECK(visited == 3);

    OrderedSet<String> set(allocator);
    String member(allocator);
    CHECK(member.TryAssign("only"));
    CHECK(set.Add(Move(member)));
    OrderedSet<String> setCopy(allocator);
    CHECK(detail::TryCopyValue(setCopy, set, allocator));
    CHECK(setCopy.Count() == 1 && setCopy.Contains(StringView("only")));

    Optional<StructLeaf> optional;
    optional.Emplace(MakeLeaf(allocator, 9, "inner"));
    Optional<StructLeaf> optionalCopy;
    CHECK(detail::TryCopyValue(optionalCopy, optional, allocator));
    CHECK(optionalCopy.HasValue() && SameLeaf(optionalCopy.Value(), optional.Value()));
    // unset 원본은 destination을 비운다.
    Optional<StructLeaf> unset;
    CHECK(detail::TryCopyValue(optionalCopy, unset, allocator));
    CHECK(!optionalCopy.HasValue());

    String pair[2] = {String(allocator), String(allocator)};
    CHECK(pair[0].TryAssign("left") && pair[1].TryAssign("right"));
    String pairCopy[2] = {String(allocator), String(allocator)};
    CHECK(detail::TryCopyValue(pairCopy, pair, allocator));
    CHECK(pairCopy[0].View() == "left" && pairCopy[1].View() == "right");

    SoftObjectRef<Object> soft(allocator);
    soft.asset = AssetId{Guid{1, 2}};
    CHECK(soft.objectPath.TryAssign("Root.Child"));
    SoftObjectRef<Object> softCopy(allocator);
    CHECK(detail::TryCopyValue(softCopy, soft, allocator));
    CHECK(softCopy.asset == soft.asset && softCopy.objectPath.View() == "Root.Child");

    // 값 연산 표: 기본 값 생성 → 복사 → 소멸.
    const RuntimeValueOps& ops = detail::ValueOpsOf<LeafMap>();
    CHECK(ops.size == sizeof(LeafMap));
    CHECK(ops.nativeType == NativeTypeKey<LeafMap>());
    CHECK(ops.copy != nullptr && ops.swap != nullptr);
    alignas(LeafMap) unsigned char storage[sizeof(LeafMap)];
    ops.construct(storage, allocator);
    CHECK(ops.copy(storage, &map, allocator));
    CHECK(SameMap(*reinterpret_cast<LeafMap*>(storage), map));
    ops.destroy(storage);
    CHECK(detail::ValueOpsOf<ObjectRefArray<Object>>().copy == nullptr);

    // 참조 struct는 복사·교환이 없다(ObjectRef는 복사·이동이 막혀 있다).
    const RuntimeStructOps holderOps = GeneratedType<StructHolder>::Ops();
    CHECK(holderOps.copy == nullptr && holderOps.swap == nullptr);
}

TEST(ValueCopy_FailureLeavesDestinationUnchanged) {
    SystemAllocator system;
    test::NFailAllocator failing(system);

    OrderedMap<String, StructLeaf> source(system);
    FillMap(source, system);
    OrderedMap<String, StructLeaf> destination(system);
    String key(system);
    CHECK(key.TryAssign("old"));
    CHECK(destination.Insert(Move(key), MakeLeaf(system, 42, "kept")));

    // 몇 번째 할당이든 실패하면 destination·source가 그대로이고, 실패가 없을 때까지 늘려 간다.
    bool succeeded = false;
    for (uint64 attempt = 1; attempt < 64 && !succeeded; ++attempt) {
        failing.FailOnAttempt(attempt);
        Result<void> copied = detail::TryCopyValue(destination, source, failing);
        if (copied) {
            succeeded = true;
            break;
        }

        CHECK(copied.Error() == ErrorCode::OutOfMemory);
        CHECK(destination.Count() == 1);
        const StructLeaf* kept = destination.Find(StringView("old"));
        CHECK(kept && kept->count == 42 && kept->label.View() == "kept");
        CHECK(source.Count() == 3);
    }

    CHECK(succeeded);
    failing.DisableFailure();
    CHECK(SameMap(destination, source));

    // 생성 struct 복사도 같다(멤버 String 할당 실패).
    StructMiddle from(system);
    from.leaf = MakeLeaf(system, 5, "from");
    from.cache = 11;
    StructMiddle into(system);
    into.leaf = MakeLeaf(system, 6, "into");
    into.cache = 12;
    failing.FailOnAttempt(1);
    CHECK(GeneratedType<StructMiddle>::CopyValue(into, from, failing).Error() ==
          ErrorCode::OutOfMemory);
    CHECK(into.leaf.label.View() == "into" && into.cache == 12 && into.leaf.count == 6);
    failing.DisableFailure();
}

TEST(ValueCopy_SwapDoesNotAllocate) {
    SystemAllocator system;
    test::NFailAllocator counting(system);

    OrderedMap<String, StructLeaf> first(counting);
    FillMap(first, counting);
    OrderedMap<String, StructLeaf> second(counting);
    String key(counting);
    CHECK(key.TryAssign("solo"));
    CHECK(second.Insert(Move(key), MakeLeaf(counting, 7, "solo")));

    const uint64 before = counting.Attempts();
    detail::ValueOpsOf<OrderedMap<String, StructLeaf>>().swap(&first, &second);
    CHECK(counting.Attempts() == before);
    CHECK(first.Count() == 1 && second.Count() == 3);
    CHECK(first.Find(StringView("solo")) != nullptr);

    StructMiddle left(counting);
    left.leaf = MakeLeaf(counting, 1, "left");
    left.cache = 3;
    StructMiddle right(counting);
    right.leaf = MakeLeaf(counting, 2, "right");
    right.cache = 4;
    const RuntimeStructOps middleOps = GeneratedType<StructMiddle>::Ops();
    CHECK(middleOps.swap != nullptr && middleOps.copy != nullptr);
    const uint64 beforeStruct = counting.Attempts();
    middleOps.swap(&left, &right);
    CHECK(counting.Attempts() == beforeStruct);
    CHECK(left.leaf.label.View() == "right" && left.cache == 4);
    CHECK(right.leaf.label.View() == "left" && right.cache == 3);

    String pair[2] = {String(counting), String(counting)};
    String other[2] = {String(counting), String(counting)};
    CHECK(pair[0].TryAssign("p") && other[1].TryAssign("q"));
    const uint64 beforeArray = counting.Attempts();
    detail::SwapValues(pair, other);
    CHECK(counting.Attempts() == beforeArray);
    CHECK(pair[1].View() == "q" && other[0].View() == "p");
}

TEST(ValueCopy_GeneratedStructCopiesNativeMembers) {
    SystemAllocator allocator;
    StructMiddle source(allocator);
    source.leaf = MakeLeaf(allocator, 8, "leaf");
    source.offset = -3;
    source.note = 2;
    source.cache = 99;

    // 등록 함수표의 copy(타입 무관)로 복사한다 — NATIVE 멤버 cache도 복사된다.
    const RuntimeStructOps ops = GeneratedType<StructMiddle>::Ops();
    StructMiddle copy(allocator);
    CHECK(ops.copy(&copy, &source, allocator));
    CHECK(SameLeaf(copy.leaf, source.leaf));
    CHECK(copy.leaf.label.View().Data() != source.leaf.label.View().Data());
    CHECK(copy.offset == -3 && copy.note == 2 && copy.cache == 99);

    // 등록한 레코드도 같은 copy·swap을 싣는다.
    TypeRegistry types(allocator);
    ModuleRegistration registration(types, allocator);
    REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(registration));
    {
        auto lease = types.AcquireStructByNative(NativeTypeKey<StructMiddle>());
        REQUIRE(lease);
        CHECK(lease.Value().StructOps().copy == ops.copy);
        CHECK(lease.Value().StructOps().swap == ops.swap);
    }

    CHECK(registration.Unregister());
}
