#include <ReflectionTestFixtures/ClassDefaultsFixture.generated.h>
#include <ReflectionTestFixtures/ReflectionTestFixtures.module.generated.h>
#include <Core/Memory/SystemAllocator.h>
#include <Object/ModuleRegistration.h>
#include <Object/PropertyPath.h>
#include <ObjectSerialization/ClassDefaults.h>
#include <ObjectSerialization/ObjectGraph.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>
#include "GraphTestSupport.h"

// 클래스 기본값 되돌리기 시험이다(Docs/Design/RuntimeClassMetadata.ko.md 'A5b2 decode 명세',
// fixture ReflectionTestFixtures/ClassDefaultsFixture.h): 단일 값 각
// 종류(상속·EditorOnly·bitfield·Text· Name None·참조 null·InstancedStruct)를 되돌리면 그래프 저장이
// 레코드와 byte가 같음, NaN payload· -0.0 비트 그대로, struct 통째(비Atomic·Atomic·두 단계)와 멤버,
// Optional 단일 값·Optional<struct> (기준 set·unset), struct 안 bitfield, non-null ClassRef 기본값,
// ReadOnly 거부(아무것도 쓰지 않음), 1차 범위 밖(컨테이너·그것을 품은 struct)·원소 경로·Atomic
// 멤버·Transient·빈 경로·레코드 없는 타입의 오류, 계획의 연산 목록, 계획 할당 실패.
using namespace rocks;
namespace class_defaults_revert_tests_file {

using reflection_fixture::DefaultsAbstractBase;
using reflection_fixture::DefaultsCounter;
using reflection_fixture::DefaultsHolder;
using reflection_fixture::DefaultsKindPick;
using reflection_fixture::DefaultsMode;
using reflection_fixture::DefaultsRevert;

namespace {

// -0.0(double)의 비트다.
constexpr uint64 NegativeZeroBits = uint64{1} << 63;

struct RevertFixture {
    explicit RevertFixture(Allocator& allocator) noexcept
        : types(allocator), registration(types, allocator) {
        REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(registration));
    }

    ~RevertFixture() noexcept {
        CHECK(registration.Unregister());
        CHECK(types.Shutdown());
    }

    TypeLease Lease(TypeId type) noexcept {
        auto acquired = types.Acquire(type);
        if (!acquired) {
            platform::Fatal("test type lease failed");
        }

        return Move(acquired.Value());
    }

    // type 스키마로 해석한 경로다(해석 실패는 시험 오류).
    PropertyPathBuffer Path(TypeId type, StringView text) noexcept {
        const TypeLease lease = Lease(type);
        PropertyPathBuffer path;
        if (!ParsePropertyPath(lease, text, path)) {
            platform::Fatal("test path parse failed");
        }

        return path;
    }

    TypeRegistry types;
    ModuleRegistration registration;
};

// 노드 하나짜리 저장 그래프의 노드 Record다(NodeTable을 건너뛴다 — GraphTestSupport.h).
Span<const uint8> NodeRecord(Span<const uint8> graph) noexcept {
    return test::graph::NodeRecordAt(graph, 0);
}

// 객체 하나를 기본 정책(전부 — 기준 없음)으로 저장한 그래프다.
Array<uint8> SaveAll(Allocator& allocator, ObjectRegistry& objects, ObjectHandle handle) noexcept {
    auto saved = SaveObjectGraph(allocator, objects, objects.OwnerAccess(), handle, {});
    if (!saved) {
        platform::Fatal("test graph save failed");
    }

    return Move(saved.Value());
}

// 객체의 네이티브 값을 바로 읽고 고친다(edit에 T&).
template <class T, class Edit>
void EditNative(ObjectRegistry& objects, ObjectHandle handle, Edit&& edit) noexcept {
    auto borrowed = objects.Resolve<T>(objects.OwnerAccess(), handle);
    REQUIRE(borrowed);
    auto value = borrowed.Value().Get();
    REQUIRE(value);
    edit(*value.Value());
}

// 그 객체의 저장 Record가 클래스 기본값 editor 레코드와 byte가 같은지다.
bool MatchesDefaults(Allocator& allocator, RevertFixture& fixture, ObjectRegistry& objects,
                     ObjectHandle handle, TypeId type) noexcept {
    const TypeLease lease = fixture.Lease(type);
    auto defaults = AcquireClassDefaults(lease, allocator);
    if (!defaults) {
        return false;
    }

    const Array<uint8> graph = SaveAll(allocator, objects, handle);
    return test::graph::SameBytes(defaults.Value()->Record(ClassDefaultsConfiguration::Editor),
                                  NodeRecord(graph.View()));
}

Result<void> Revert(Allocator& allocator, RevertFixture& fixture, ObjectRegistry& objects,
                    ObjectHandle handle, TypeId type, StringView text) noexcept {
    const PropertyPathBuffer path = fixture.Path(type, text);
    return RevertToDefault(objects, objects.OwnerAccess(), handle, path, allocator);
}

} // namespace

// 단일 값 각 종류(상속 필드·EditorOnly 필드·bitfield·enum·String·Optional 단일 값 set 기준·
// non-null로 바꾼 ClassRef·Text·Name None·강한·weak·Soft null·InstancedStruct)와 struct
// 통째(비Atomic·Atomic·두 단계·EditorOnly struct)를 되돌리면 그래프 저장 Record가 레코드와 byte가
// 같다. precise(-0.0)는 부호 비트까지 돌아온다.
TEST(ClassDefaultsRevert_SingleValuesAndStructs) {
    SystemAllocator allocator;
    RevertFixture fixture(allocator);
    {
        ObjectRegistry objects(allocator, fixture.types, {0x5256, 1});
        const TypeId type = TypeIdOf<DefaultsHolder>();
        auto created = objects.NewObject<DefaultsHolder>(objects.OwnerAccess(), type);
        REQUIRE(created);
        const ObjectHandle handle = created.Value().Handle();
        EditNative<DefaultsHolder>(objects, handle, [](DefaultsHolder& holder) noexcept {
            holder.baseValue = 100;
            holder.count = -5;
            holder.scale = 0.25f;
            holder.precise = 0.0;
            holder.enabled = false;
            holder.mode = DefaultsMode::Third;
            CHECK(holder.label.TryAssign("changed"));
            holder.point.x = 30;
            holder.point.note = 40;
            holder.pair.u = 70;
            holder.nest.inner.y = 9.0f;
            holder.nest.depth = 20;
            holder.maybe.Reset();
            holder.kind = ClassRef<DefaultsAbstractBase>::Of<DefaultsHolder>();
            holder.editorValue = 110;
            holder.editorPoint.x = 33;
            holder.lit = 0;
            holder.dim = 1;
        });
        CHECK(!MatchesDefaults(allocator, fixture, objects, handle, type));
        const StringView paths[] = {
            "baseValue", "count", "scale", "precise", "enabled",     "mode",       "label", "point",
            "pair",      "nest",  "maybe", "caption", "payload",     "tag",        "kind",  "link",
            "weak",      "soft",  "lit",   "dim",     "editorValue", "editorPoint"};
        for (StringView text : paths) {
            CHECK(Revert(allocator, fixture, objects, handle, type, text));
        }

        CHECK(MatchesDefaults(allocator, fixture, objects, handle, type));
        EditNative<DefaultsHolder>(objects, handle, [](DefaultsHolder& holder) noexcept {
            CHECK(platform::BitCast<uint64>(holder.precise) == NegativeZeroBits);
            CHECK(holder.maybe.HasValue() && holder.maybe.Value() == 9);
            CHECK(holder.kind.IsNull());
        });
        // 멤버 경로(두 단계 사슬 끝·EditorOnly 멤버)도 그 멤버만 되돌린다.
        EditNative<DefaultsHolder>(objects, handle, [](DefaultsHolder& holder) noexcept {
            holder.nest.inner.x = 50;
            holder.nest.depth = 21;
            holder.point.note = 41;
        });
        CHECK(Revert(allocator, fixture, objects, handle, type, "nest.inner.x"));
        CHECK(Revert(allocator, fixture, objects, handle, type, "point.note"));
        EditNative<DefaultsHolder>(objects, handle, [](DefaultsHolder& holder) noexcept {
            CHECK(holder.nest.inner.x == 3);
            CHECK(holder.nest.depth == 21);
            CHECK(holder.point.note == 4);
        });
        created.Value().Reset();
        CHECK(objects.Shutdown(objects.OwnerAccess()));
    }
}

// NaN payload(float)와 -0.0(double)은 비트 그대로 돌아온다(텍스트 왕복이 아니다). Optional<struct>:
// 기준 set(preview — x 13)은 unset이든 다른 값이든 기본 값 + 멤버로, 기준 unset(absent)은
// unset으로, Optional 단일 값 기준 unset(none)은 unset으로. struct 안 bitfield는 struct 통째와
// 멤버 하나로.
TEST(ClassDefaultsRevert_BitsOptionalsAndBitfields) {
    SystemAllocator allocator;
    RevertFixture fixture(allocator);
    {
        ObjectRegistry objects(allocator, fixture.types, {0x5256, 2});
        const TypeId type = TypeIdOf<DefaultsRevert>();
        auto created = objects.NewObject<DefaultsRevert>(objects.OwnerAccess(), type);
        REQUIRE(created);
        const ObjectHandle handle = created.Value().Handle();
        EditNative<DefaultsRevert>(objects, handle, [](DefaultsRevert& value) noexcept {
            value.quiet = 1.0f;
            value.negative = 0.0;
            value.preview.Reset();
            value.absent.Emplace();
            value.absent.Value().y = 7.0f;
            value.none = Optional<int32>(5);
            value.bits.on = 0;
            value.bits.off = 1;
            value.bits.level = 0;
        });
        const StringView paths[] = {"quiet", "negative", "preview", "absent", "none", "bits"};
        for (StringView text : paths) {
            CHECK(Revert(allocator, fixture, objects, handle, type, text));
        }

        CHECK(MatchesDefaults(allocator, fixture, objects, handle, type));
        EditNative<DefaultsRevert>(objects, handle, [](DefaultsRevert& value) noexcept {
            CHECK(platform::BitCast<uint32>(value.quiet) == DefaultsRevert::QuietPayloadBits);
            CHECK(platform::BitCast<uint64>(value.negative) == NegativeZeroBits);
            CHECK(value.preview.HasValue() && value.preview.Value().x == 13);
            CHECK(!value.absent.HasValue());
            CHECK(!value.none.HasValue());
            CHECK(value.bits.on == 1 && value.bits.off == 0 && value.bits.level == 6);
            // set인 기준 위의 다른 값도 기본 값으로 바뀐다(이미 set이어도 Emplace가 기본 값).
            value.preview.Value().x = 99;
            value.preview.Value().note = 98;
            value.bits.off = 1;
            value.bits.level = 60;
        });
        CHECK(Revert(allocator, fixture, objects, handle, type, "preview"));
        CHECK(Revert(allocator, fixture, objects, handle, type, "bits.off"));
        EditNative<DefaultsRevert>(objects, handle, [](DefaultsRevert& value) noexcept {
            CHECK(value.preview.Value().x == 13 && value.preview.Value().note == 4);
            CHECK(value.bits.off == 0 && value.bits.level == 60);
        });
        created.Value().Reset();
        CHECK(objects.Shutdown(objects.OwnerAccess()));
    }
}

// non-null ClassRef 기본값은 지금 등록에서 다시 판정해 돌아온다.
TEST(ClassDefaultsRevert_NonNullClassReference) {
    SystemAllocator allocator;
    RevertFixture fixture(allocator);
    {
        ObjectRegistry objects(allocator, fixture.types, {0x5256, 3});
        const TypeId type = TypeIdOf<DefaultsKindPick>();
        auto created = objects.NewObject<DefaultsKindPick>(objects.OwnerAccess(), type);
        REQUIRE(created);
        const ObjectHandle handle = created.Value().Handle();
        EditNative<DefaultsKindPick>(objects, handle, [](DefaultsKindPick& value) noexcept {
            value.pick = ClassRef<DefaultsAbstractBase>();
        });
        CHECK(Revert(allocator, fixture, objects, handle, type, "pick"));
        EditNative<DefaultsKindPick>(objects, handle, [](DefaultsKindPick& value) noexcept {
            CHECK(value.pick.Id() == TypeIdOf<DefaultsHolder>());
        });
        CHECK(MatchesDefaults(allocator, fixture, objects, handle, type));
        created.Value().Reset();
        CHECK(objects.Shutdown(objects.OwnerAccess()));
    }
}

// 거부(사전 단계 — 아무것도 쓰지 않는다): ReadOnly 필드, ReadOnly 멤버가 있는 struct
// 통째(ReadOnly가 아닌 멤버도 그대로), 1차 범위 밖 컨테이너(Array·Map·Set·정적 배열)와 배열을 품은
// struct 통째 Unsupported. 그 struct의 단일 값 멤버·ReadOnly가 아닌 멤버 경로는 된다. 원소 경로
// Unsupported, Atomic 멤버 경로 InvalidArgument, Transient 필드(레코드에 없음) NotFound, 빈 경로
// InvalidArgument.
TEST(ClassDefaultsRevert_Rejections) {
    SystemAllocator allocator;
    RevertFixture fixture(allocator);
    {
        ObjectRegistry objects(allocator, fixture.types, {0x5256, 4});
        const TypeId revertType = TypeIdOf<DefaultsRevert>();
        auto revert = objects.NewObject<DefaultsRevert>(objects.OwnerAccess(), revertType);
        REQUIRE(revert);
        const ObjectHandle revertHandle = revert.Value().Handle();
        EditNative<DefaultsRevert>(objects, revertHandle, [](DefaultsRevert& value) noexcept {
            value.sealed = 30;
            value.guarded.open = 10;
            value.guarded.locked = 20;
            value.bag.size = 5;
            CHECK(value.bag.items.TryPushBack(1));
        });
        const auto refused = [&](StringView text, ErrorCode error) noexcept {
            auto result = Revert(allocator, fixture, objects, revertHandle, revertType, text);
            return !result && result.Error() == error;
        };
        CHECK(refused("sealed", ErrorCode::Unsupported));
        CHECK(refused("guarded", ErrorCode::Unsupported));
        CHECK(refused("guarded.locked", ErrorCode::Unsupported));
        CHECK(refused("bag", ErrorCode::Unsupported));
        CHECK(refused("bag.items", ErrorCode::Unsupported));
        EditNative<DefaultsRevert>(objects, revertHandle, [](DefaultsRevert& value) noexcept {
            CHECK(value.sealed == 30 && value.guarded.open == 10 && value.guarded.locked == 20);
            CHECK(value.bag.size == 5 && value.bag.items.Size() == 1);
        });
        CHECK(Revert(allocator, fixture, objects, revertHandle, revertType, "guarded.open"));
        CHECK(Revert(allocator, fixture, objects, revertHandle, revertType, "bag.size"));
        EditNative<DefaultsRevert>(objects, revertHandle, [](DefaultsRevert& value) noexcept {
            CHECK(value.guarded.open == 1 && value.guarded.locked == 20);
            CHECK(value.bag.size == 1 && value.bag.items.Size() == 1);
        });

        const TypeId holderType = TypeIdOf<DefaultsHolder>();
        auto holder = objects.NewObject<DefaultsHolder>(objects.OwnerAccess(), holderType);
        REQUIRE(holder);
        const ObjectHandle holderHandle = holder.Value().Handle();
        const auto holderRefused = [&](StringView text, ErrorCode error) noexcept {
            auto result = Revert(allocator, fixture, objects, holderHandle, holderType, text);
            return !result && result.Error() == error;
        };
        CHECK(holderRefused("values", ErrorCode::Unsupported));
        CHECK(holderRefused("table", ErrorCode::Unsupported));
        CHECK(holderRefused("tags", ErrorCode::Unsupported));
        CHECK(holderRefused("fixed", ErrorCode::Unsupported));
        CHECK(holderRefused("values[0]", ErrorCode::Unsupported));
        CHECK(holderRefused("fixed[1]", ErrorCode::Unsupported));
        CHECK(holderRefused("pair.u", ErrorCode::InvalidArgument));
        CHECK(holderRefused("scratch", ErrorCode::NotFound));
        const PropertyPathBuffer empty{};
        auto none = RevertToDefault(objects, objects.OwnerAccess(), holderHandle, empty, allocator);
        CHECK(!none && none.Error() == ErrorCode::InvalidArgument);
        holder.Value().Reset();
        revert.Value().Reset();
        CHECK(objects.Shutdown(objects.OwnerAccess()));
    }
}

// 레코드 없는 타입(비결정 생성자)은 AcquireClassDefaults의 오류와 진단 이유 그대로다.
TEST(ClassDefaultsRevert_TypeWithoutRecord) {
    SystemAllocator allocator;
    RevertFixture fixture(allocator);
    {
        ObjectRegistry objects(allocator, fixture.types, {0x5256, 5});
        const TypeId type = TypeIdOf<DefaultsCounter>();
        auto created = objects.NewObject<DefaultsCounter>(objects.OwnerAccess(), type);
        REQUIRE(created);
        const PropertyPathBuffer path = fixture.Path(type, "stable");
        ClassDefaultsDiagnostic why;
        auto result = RevertToDefault(objects, objects.OwnerAccess(), created.Value().Handle(),
                                      path, allocator, nullptr, &why);
        CHECK(!result && result.Error() == ErrorCode::InvalidArgument);
        CHECK(why.reason == ClassDefaultsReason::NonDeterministic);
        created.Value().Reset();
        CHECK(objects.Shutdown(objects.OwnerAccess()));
    }
}

// 계획은 값을 바꾸지 않고 연산 목록을 만든다: 기준 set인 Optional<struct>는 EmplaceOptional 뒤 멤버
// Write 셋(x·y·EditorOnly note — Editor 레코드), 기준 unset은 ResetOptional 하나, InstancedStruct는
// ClearInstanced, 비Atomic struct는 잎 멤버 Write(경로는 필드 → 멤버). 실패한 계획은 비어 있다.
TEST(ClassDefaultsRevert_PlanOperations) {
    SystemAllocator allocator;
    RevertFixture fixture(allocator);
    {
        ObjectRegistry objects(allocator, fixture.types, {0x5256, 6});
        const TypeId type = TypeIdOf<DefaultsRevert>();
        auto created = objects.NewObject<DefaultsRevert>(objects.OwnerAccess(), type);
        REQUIRE(created);
        const ObjectHandle handle = created.Value().Handle();
        EditNative<DefaultsRevert>(objects, handle,
                                   [](DefaultsRevert& value) noexcept { value.preview.Reset(); });
        RevertPlan plan(allocator);
        const PropertyPathBuffer preview = fixture.Path(type, "preview");
        REQUIRE(PlanRevert(objects, objects.OwnerAccess(), handle, preview, allocator, plan));
        CHECK(plan.object == handle);
        REQUIRE(plan.operations.Size() == 4);
        CHECK(plan.operations[0].kind == RevertOperation::Kind::EmplaceOptional);
        CHECK(plan.operations[0].stepCount == 1);
        for (usize index = 1; index < plan.operations.Size(); ++index) {
            CHECK(plan.operations[index].kind == RevertOperation::Kind::Write);
            CHECK(plan.operations[index].stepCount == 2);
            CHECK(plan.operations[index].steps[0].fieldId == preview.steps[0].fieldId);
        }

        EditNative<DefaultsRevert>(objects, handle, [](DefaultsRevert& value) noexcept {
            CHECK(!value.preview.HasValue());
        });
        REQUIRE(ApplyRevert(objects, objects.OwnerAccess(), plan));
        EditNative<DefaultsRevert>(objects, handle, [](DefaultsRevert& value) noexcept {
            CHECK(value.preview.HasValue() && value.preview.Value().x == 13);
        });

        const PropertyPathBuffer absent = fixture.Path(type, "absent");
        REQUIRE(PlanRevert(objects, objects.OwnerAccess(), handle, absent, allocator, plan));
        REQUIRE(plan.operations.Size() == 1);
        CHECK(plan.operations[0].kind == RevertOperation::Kind::ResetOptional);

        const PropertyPathBuffer bits = fixture.Path(type, "bits");
        REQUIRE(PlanRevert(objects, objects.OwnerAccess(), handle, bits, allocator, plan));
        CHECK(plan.operations.Size() == 3);

        const PropertyPathBuffer guarded = fixture.Path(type, "guarded");
        auto refused = PlanRevert(objects, objects.OwnerAccess(), handle, guarded, allocator, plan);
        CHECK(!refused && refused.Error() == ErrorCode::Unsupported);
        CHECK(plan.operations.IsEmpty());
        created.Value().Reset();

        const TypeId holderType = TypeIdOf<DefaultsHolder>();
        auto holder = objects.NewObject<DefaultsHolder>(objects.OwnerAccess(), holderType);
        REQUIRE(holder);
        const PropertyPathBuffer payload = fixture.Path(holderType, "payload");
        REQUIRE(PlanRevert(objects, objects.OwnerAccess(), holder.Value().Handle(), payload,
                           allocator, plan));
        REQUIRE(plan.operations.Size() == 1);
        CHECK(plan.operations[0].kind == RevertOperation::Kind::ClearInstanced);
        holder.Value().Reset();
        CHECK(objects.Shutdown(objects.OwnerAccess()));
    }
}

// 계획의 scratch 할당 실패는 모두 OutOfMemory이고 계획은 비어 있으며 값은 그대로다(사전 단계라
// 아무것도 쓰지 않는다). 마지막 시도는 성공한다.
TEST(ClassDefaultsRevert_PlanAllocationFailures) {
    SystemAllocator allocator;
    RevertFixture fixture(allocator);
    {
        ObjectRegistry objects(allocator, fixture.types, {0x5256, 7});
        const TypeId type = TypeIdOf<DefaultsHolder>();
        auto created = objects.NewObject<DefaultsHolder>(objects.OwnerAccess(), type);
        REQUIRE(created);
        const ObjectHandle handle = created.Value().Handle();
        EditNative<DefaultsHolder>(objects, handle, [](DefaultsHolder& holder) noexcept {
            CHECK(holder.label.TryAssign("changed"));
            holder.point.x = 30;
        });
        const StringView paths[] = {"label", "point"};
        for (StringView text : paths) {
            const PropertyPathBuffer path = fixture.Path(type, text);
            uint32 failures = 0;
            for (uint64 attempt = 1;; ++attempt) {
                test::NFailAllocator failing(allocator);
                failing.FailOnAttempt(attempt);
                RevertPlan plan(failing);
                auto planned =
                    PlanRevert(objects, objects.OwnerAccess(), handle, path, failing, plan);
                const bool exhausted = failing.Attempts() < attempt;
                failing.DisableFailure();
                if (exhausted) {
                    CHECK(planned);
                    break;
                }

                CHECK(!planned && planned.Error() == ErrorCode::OutOfMemory);
                CHECK(plan.operations.IsEmpty());
                ++failures;
            }

            CHECK(failures > 0);
        }

        EditNative<DefaultsHolder>(objects, handle, [](DefaultsHolder& holder) noexcept {
            CHECK(holder.label.View() == StringView("changed"));
            CHECK(holder.point.x == 30);
        });
        created.Value().Reset();
        CHECK(objects.Shutdown(objects.OwnerAccess()));
    }
}

} // namespace class_defaults_revert_tests_file
