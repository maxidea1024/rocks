#include <ReflectionTestFixtures/ReflectionTestFixtures.module.generated.h>
#include <Core/Memory/SystemAllocator.h>
#include <Object/ConstructionContext.h>
#include <ObjectSerialization/ObjectGraph.h>
#include <TestSupport/Test.h>

// MetaCompiler 생성 코드(B4b)로 등록한 리플렉션 struct의 그래프 저장·복원 시험이다(Docs/Design/
// ReflectionFoundations.ko.md 4.6절). StructGraphTests가 손으로 쓴 특수화로 검증한 모양을 생성
// 특수화와 모듈 집계 등록(enum → struct → 클래스)으로 다시 확인한다: 3단 중첩, 참조 struct의 생성
// Trace(struct 안 참조로만 닿는 객체가 수집에서 산다), 재저장 byte 동일, 생성 형질·깊이, 타입
// 마이그레이션 연결(StructLeaf::Migrate).
using namespace rocks;
using reflection_fixture::StructHolder;
using reflection_fixture::StructLeaf;
using reflection_fixture::StructMiddle;
using reflection_fixture::StructOwner;
using reflection_fixture::TextureGroup;

// 생성 형질과 깊이(모델 값 — 생성 코드가 멤버 struct의 값과 static_assert로 대조한다).
static_assert(!GeneratedType<StructLeaf>::ContainsStrongReference);
static_assert(!GeneratedType<StructMiddle>::ContainsStrongReference);
static_assert(GeneratedType<StructHolder>::ContainsStrongReference);
static_assert(!GeneratedType<StructHolder>::RequiresWorldOnlyOwner);
static_assert(GeneratedType<StructLeaf>::Depth == 1);
static_assert(GeneratedType<StructMiddle>::Depth == 2);
static_assert(GeneratedType<StructHolder>::Depth == 3);

namespace {

constexpr uint32 EditorOnly = static_cast<uint32>(FieldFlags::EditorOnly);

// A(값을 채움)와 A.holder.target이 가리키는 B를 만들고 A의 root를 돌려준다.
Result<RootRef<Object>> MakeGraph(ObjectRegistry& objects) noexcept {
    ConstructionContext transaction(objects, objects.OwnerAccess());
    auto a = transaction.Create<StructOwner>(TypeIdOf<StructOwner>());
    auto b = transaction.Create<StructOwner>(TypeIdOf<StructOwner>());
    if (!a || !b) {
        return Result<RootRef<Object>>::Failure(!a ? a.Error() : b.Error());
    }

    auto owner = transaction.Get<StructOwner>(a.Value());
    if (!owner) {
        return Result<RootRef<Object>>::Failure(owner.Error());
    }

    StructOwner& value = *owner.Value();
    value.plain = 7;
    value.holder.flag = 3;
    value.holder.middle.offset = -5;
    value.holder.middle.note = 11;
    value.holder.middle.cache = 99;
    value.holder.middle.leaf.count = 42;
    value.holder.middle.leaf.group = TextureGroup::EightBitData;
    value.extra.count = 9;
    value.scratch.count = 1234;
    if (!value.holder.middle.leaf.label.TryAssign("중첩") || !value.extra.label.TryAssign("편집")) {
        return Result<RootRef<Object>>::Failure(ErrorCode::OutOfMemory);
    }

    if (auto linked = transaction.SetReference(a.Value(), value.holder.target, b.Value());
        !linked) {
        return Result<RootRef<Object>>::Failure(linked.Error());
    }

    return transaction.Commit();
}

Result<Array<uint8>> SaveFixture(Allocator& allocator, ObjectRegistry& objects,
                                 ObjectGraphSaveOptions options = {}) noexcept {
    auto root = MakeGraph(objects);
    if (!root) {
        return Result<Array<uint8>>::Failure(root.Error());
    }

    return SaveObjectGraph(allocator, objects, objects.OwnerAccess(), root.Value().Handle(),
                           options);
}

// StructLeaf 관찰 hook이다. 관찰 decode는 생성 Ops의 construct로 만든 scratch 값에 채워지고
// destroy로 없어진다 — 등장마다 count와 label("중첩"인지)을 기록한다.
class LeafObserver final : public FieldCodecHooks {
public:
    Span<const TypeId> ObservedStructs() noexcept override {
        return Span<const TypeId>(ids, 1);
    }

    Result<void> ObserveStruct(const FieldCodecFrame&, TypeId type, const void* nativeKey,
                               const void* value) noexcept override {
        if (type != TypeIdOf<StructLeaf>() || nativeKey != NativeTypeKey<StructLeaf>() ||
            count == 4) {
            mismatched = true;
            return Result<void>::Success();
        }

        const StructLeaf& leaf = *static_cast<const StructLeaf*>(value);
        counts[count] = leaf.count;
        nested[count] = leaf.label.View() == StringView("중첩");
        ++count;
        return Result<void>::Success();
    }

    // 관찰한 값 중 count가 value인 것의 수다.
    usize CountOf(uint32 value) const noexcept {
        usize found = 0;
        for (usize index = 0; index < count; ++index) {
            found += counts[index] == value;
        }

        return found;
    }

    const TypeId ids[1] = {TypeIdOf<StructLeaf>()};
    uint32 counts[4] = {};
    bool nested[4] = {};
    usize count = 0;
    bool mismatched = false;
};

bool SameBytes(Span<const uint8> left, Span<const uint8> right) noexcept {
    if (left.Size() != right.Size()) {
        return false;
    }

    for (usize index = 0; index < left.Size(); ++index) {
        if (left[index] != right[index]) {
            return false;
        }
    }

    return true;
}

// 생성 특수화를 하나씩 등록하되 StructLeaf만 v1 스키마(마이그레이션 없음)로 등록한다 — 옛 빌드가 쓴
// 데이터를 만든다.
bool RegisterVersionOne(TypeRegistry& types) noexcept {
    if (!GeneratedType<TextureGroup>::Register(types)) {
        return false;
    }

    TypeSchema leaf = GeneratedType<StructLeaf>::Schema();
    leaf.version = 1;
    RuntimeStructOps leafOps = GeneratedType<StructLeaf>::Ops();
    leafOps.migrate = nullptr;
    return types.RegisterStruct(leaf, Move(leafOps)).IsOk() &&
           GeneratedType<StructMiddle>::Register(types).IsOk() &&
           GeneratedType<StructHolder>::Register(types).IsOk() &&
           GeneratedType<StructOwner>::Register(types).IsOk();
}

} // namespace

// 모듈 집계 등록이 struct를 멤버 struct 먼저 등록하고, 등록 레코드의 형질·깊이가 생성 값과 같으며,
// 중첩·참조 struct가 왕복하고(NATIVE 멤버는 저장하지 않는다) 재저장 byte가 같다.
TEST(GeneratedStructGraph_ModuleRegistrationRoundTripsNestedReferenceStructs) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    {
        ModuleRegistration registration(types, allocator);
        REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(registration));
        {
            auto holder = types.AcquireValueType(TypeIdOf<StructHolder>(), TypeKind::Struct);
            REQUIRE(holder);
            CHECK(holder.Value().Struct().Depth() == 3);
            CHECK(holder.Value().Struct().ContainsStrongReference());
            CHECK(holder.Value().Struct().ContainsRequired());
            CHECK(!holder.Value().Struct().RequiresWorldOnlyOwner());
            CHECK(holder.Value().StructOps().trace != nullptr);
            auto leaf = types.AcquireValueType(TypeIdOf<StructLeaf>(), TypeKind::Struct);
            REQUIRE(leaf);
            CHECK(leaf.Value().Schema().version == 2);
            CHECK(leaf.Value().StructOps().trace == nullptr);
            CHECK(leaf.Value().StructOps().migrate == &StructLeaf::Migrate);
            auto owner = types.Acquire(TypeIdOf<StructOwner>());
            REQUIRE(owner);
            // 클래스 struct 필드: read·writeStaged 없음, codec·struct native 키 있음.
            const TypeSchema& schema = owner.Value().Schema();
            for (usize index = 0; index < schema.fields.Size(); ++index) {
                if (schema.fields[index].value.kind != ValueKind::Struct) {
                    continue;
                }

                const RuntimeFieldOps* found = nullptr;
                for (const RuntimeFieldOps& candidate : owner.Value().Ops().fields) {
                    found = candidate.fieldId == schema.fields[index].id ? &candidate : found;
                }

                REQUIRE(found);
                const RuntimeFieldOps& ops = *found;
                CHECK(ops.read == nullptr);
                CHECK(ops.writeStaged == nullptr);
                CHECK(ops.encode != nullptr && ops.decode != nullptr);
                CHECK(!owner.Value().FieldStruct(index).IsEmpty());
                CHECK(ops.valueNativeType ==
                      owner.Value().FieldStruct(index).StructOps().nativeType);
                const bool extra = schema.fields[index].value.type == TypeIdOf<StructLeaf>();
                CHECK(extra == ((schema.fields[index].flags & EditorOnly) != 0));
            }
        }

        {
            ObjectRegistry source(allocator, types, {0x4753, 1});
            ObjectRegistry target(allocator, types, {0x4753, 2});
            {
                auto bytes = SaveFixture(allocator, source);
                REQUIRE(bytes);
                auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                              bytes.Value().View());
                REQUIRE(loaded);
                {
                    auto borrowed =
                        target.Resolve<StructOwner>(target.OwnerAccess(), loaded.Value().Handle());
                    REQUIRE(borrowed);
                    const StructOwner& owner = *borrowed.Value().Get().Value();
                    CHECK(owner.plain == 7);
                    CHECK(owner.holder.flag == 3);
                    CHECK(owner.holder.middle.offset == -5);
                    CHECK(owner.holder.middle.note == 11);
                    CHECK(owner.holder.middle.cache == 0);
                    CHECK(owner.holder.middle.leaf.count == 42);
                    CHECK(owner.holder.middle.leaf.label.View() == StringView("중첩"));
                    CHECK(owner.holder.middle.leaf.group == TextureGroup::EightBitData);
                    CHECK(owner.extra.count == 9);
                    CHECK(owner.extra.label.View() == StringView("편집"));
                    CHECK(owner.scratch.count == 0);
                    CHECK(owner.holder.target.Handle() != ObjectHandle{});
                }

                // B는 A.holder.target(struct 안 참조)으로만 도달한다 — 생성 Trace가 방문해 수집에서
                // 산다.
                REQUIRE(target.Collect(target.OwnerAccess()));
                CHECK(target.Stats().liveObjects == 2);
                auto resaved = SaveObjectGraph(allocator, target, target.OwnerAccess(),
                                               loaded.Value().Handle());
                REQUIRE(resaved);
                CHECK(SameBytes(bytes.Value().View(), resaved.Value().View()));
            }

            REQUIRE(source.Shutdown(source.OwnerAccess()));
            REQUIRE(target.Shutdown(target.OwnerAccess()));
        }

        REQUIRE(registration.Unregister());
    }

    CHECK(types.Shutdown());
}

// 관찰 hook: 생성 construct·destroy로 만든 scratch 값에 struct 등장마다 한 번 채워져 관찰된다(A의
// 두 Leaf와 B의 두 Leaf).
TEST(GeneratedStructGraph_ObservedStructsUseGeneratedConstructAndDestroy) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    {
        ModuleRegistration registration(types, allocator);
        REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(registration));
        {
            ObjectRegistry source(allocator, types, {0x4753, 5});
            ObjectRegistry target(allocator, types, {0x4753, 6});
            {
                auto bytes = SaveFixture(allocator, source);
                REQUIRE(bytes);
                LeafObserver observer;
                ObjectGraphLoadOptions load;
                load.hooks = &observer;
                auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                              bytes.Value().View(), load);
                REQUIRE(loaded);
                CHECK(!observer.mismatched);
                CHECK(observer.count == 4);
                CHECK(observer.CountOf(42) == 1);
                CHECK(observer.CountOf(9) == 1);
                CHECK(observer.CountOf(0) == 2);
                usize nested = 0;
                for (usize index = 0; index < observer.count; ++index) {
                    nested += observer.nested[index] && observer.counts[index] == 42;
                }

                CHECK(nested == 1);
            }

            REQUIRE(source.Shutdown(source.OwnerAccess()));
            REQUIRE(target.Shutdown(target.OwnerAccess()));
        }

        REQUIRE(registration.Unregister());
    }

    CHECK(types.Shutdown());
}

// EditorOnly는 클래스의 struct 필드(extra)와 struct 안 멤버(StructMiddle::note) 모두 저장에서
// 빠지고, 누락 허용 플래그로만 다시 읽힌다(값은 생성자 기본값).
TEST(GeneratedStructGraph_EditorOnlyMembersAreExcludedAndAllowedMissing) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    {
        ModuleRegistration registration(types, allocator);
        REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(registration));
        {
            ObjectRegistry source(allocator, types, {0x4753, 7});
            ObjectRegistry target(allocator, types, {0x4753, 8});
            {
                ObjectGraphSaveOptions save;
                save.policy.excludedFlags = EditorOnly;
                auto bytes = SaveFixture(allocator, source, save);
                REQUIRE(bytes);
                const uint32 constructed = StructOwner::constructed;
                CHECK(LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                      bytes.Value().View())
                          .Error() == ErrorCode::NotFound);
                CHECK(StructOwner::constructed == constructed);
                ObjectGraphLoadOptions load;
                load.policy.allowMissingFlags = EditorOnly;
                auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                              bytes.Value().View(), load);
                REQUIRE(loaded);
                auto borrowed =
                    target.Resolve<StructOwner>(target.OwnerAccess(), loaded.Value().Handle());
                REQUIRE(borrowed);
                const StructOwner& owner = *borrowed.Value().Get().Value();
                CHECK(owner.holder.middle.leaf.count == 42);
                CHECK(owner.holder.middle.offset == -5);
                CHECK(owner.holder.middle.note == 0);
                CHECK(owner.extra.count == 0);
                CHECK(owner.extra.label.ByteCount() == 0);
            }

            REQUIRE(source.Shutdown(source.OwnerAccess()));
            REQUIRE(target.Shutdown(target.OwnerAccess()));
        }

        REQUIRE(registration.Unregister());
    }

    CHECK(types.Shutdown());
}

// 생성 Ops의 타입 마이그레이션 연결: StructLeaf만 Migrate를 가진다. 옛 버전(v1)으로 저장한 데이터를
// 생성 등록(v2)으로 읽으면 Migrate가 struct 등장마다 한 번 불린다.
TEST(GeneratedStructGraph_OlderStructVersionUsesGeneratedMigrate) {
    CHECK(GeneratedType<StructLeaf>::Ops().migrate == &StructLeaf::Migrate);
    CHECK(GeneratedType<StructMiddle>::Ops().migrate == nullptr);
    CHECK(GeneratedType<StructHolder>::Ops().migrate == nullptr);
    CHECK(GeneratedType<StructHolder>::Ops().trace != nullptr);
    CHECK(GeneratedType<StructMiddle>::Ops().trace == nullptr);

    SystemAllocator allocator;
    TypeRegistry oldTypes(allocator);
    TypeRegistry newTypes(allocator);
    REQUIRE(RegisterVersionOne(oldTypes));
    {
        ModuleRegistration registration(newTypes, allocator);
        REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(registration));
        {
            ObjectRegistry source(allocator, oldTypes, {0x4753, 3});
            ObjectRegistry target(allocator, newTypes, {0x4753, 4});
            {
                auto bytes = SaveFixture(allocator, source);
                REQUIRE(bytes);
                StructLeaf::migrations = 0;
                auto loaded = LoadObjectGraph(allocator, target, newTypes, target.OwnerAccess(),
                                              bytes.Value().View());
                REQUIRE(loaded);
                // A의 두 Leaf(holder.middle.leaf·extra)와 B의 두 Leaf, 각각 한 번.
                CHECK(StructLeaf::migrations == 4);
                auto borrowed =
                    target.Resolve<StructOwner>(target.OwnerAccess(), loaded.Value().Handle());
                REQUIRE(borrowed);
                const StructOwner& owner = *borrowed.Value().Get().Value();
                CHECK(owner.holder.middle.leaf.count == 42);
                CHECK(owner.holder.middle.leaf.label.View() == StringView("중첩"));
                CHECK(owner.extra.label.View() == StringView("편집"));
            }

            REQUIRE(source.Shutdown(source.OwnerAccess()));
            REQUIRE(target.Shutdown(target.OwnerAccess()));
        }

        REQUIRE(registration.Unregister());
    }

    CHECK(oldTypes.Shutdown());
    CHECK(newTypes.Shutdown());
}
