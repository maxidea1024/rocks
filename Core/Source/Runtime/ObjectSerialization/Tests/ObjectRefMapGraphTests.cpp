#include <ReflectionTestFixtures/ObjectRefMapFixture.generated.h>
#include <ReflectionTestFixtures/ReflectionTestFixtures.module.generated.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/NamePool.h>
#include <Object/ConstructionContext.h>
#include <ObjectSerialization/ObjectGraph.h>
#include <Serialization/Archive.h>
#include <Serialization/Record.h>
#include <TestSupport/Test.h>
#include "GraphTestSupport.h"

// 리플렉션 B9f4 ObjectRefMap PROPERTY의 생성 코드·그래프 저장·복원 시험이다(Docs/Design/
// ReflectionExtensions.ko.md 7.4절, fixture ReflectionTestFixtures/ObjectRefMapFixture.h): 생성
// 스키마
// ({Map, 0}·값 ObjectReference·FieldKind::ObjectReference·키 형식)와 형질(인라인 struct의
// ContainsReferenceArray), 키 종류별 왕복·삽입 순서·null 값·재저장 byte 동일, 생성 Trace가 값과
// 인라인 struct 안 ObjectRefMap을 방문(값으로만 닿는 객체가 수집에서 산다), Required 값의 null(저장
// 거부·로드 진단 — 경로 [f[i]]), MaxElements 저장 거부, 손상 입력(같은 키 둘·대상 타입 불일치·범위
// 밖 local ID).
using namespace rocks;
using reflection_fixture::RefMapBundle;
using reflection_fixture::RefMapDerived;
using reflection_fixture::RefMapHolder;
using reflection_fixture::RefMapOther;
using reflection_fixture::RefMapSlot;
using reflection_fixture::RefMapTarget;

static_assert(GeneratedType<RefMapBundle>::ContainsStrongReference);
static_assert(GeneratedType<RefMapBundle>::ContainsReferenceArray);

namespace {

const FieldSchema* FindField(const TypeSchema& schema, StringView name) noexcept {
    for (const FieldSchema& field : schema.fields) {
        if (StringView(field.name.Data(), field.name.Size()) == name) {
            return &field;
        }
    }

    return nullptr;
}

// 진단을 기록한다(경로 첫 단계만 복사한다).
class Diagnostics final : public FieldCodecHooks {
public:
    Result<void> Report(const FieldDiagnostic& diagnostic) noexcept override {
        if (count < 8) {
            Entry& entry = entries[count++];
            entry.kind = diagnostic.kind;
            entry.value = diagnostic.value;
            entry.depth = diagnostic.path.Size();
            if (diagnostic.path.Size() != 0) {
                entry.first = diagnostic.path[0];
            }
        }

        return Result<void>::Success();
    }

    struct Entry {
        FieldDiagnosticKind kind = FieldDiagnosticKind::UnknownField;
        int64 value = 0;
        usize depth = 0;
        PathStep first{};
    };

    Entry entries[8];
    usize count = 0;
};

RegistryOptions WithNames(NamePool& names) noexcept {
    RegistryOptions options;
    options.names = &names;
    return options;
}

struct GraphOptions {
    bool requiredNull = false;
    uint32 limitedCount = 2;
};

// RefMapHolder 하나와 값으로만 닿는 대상 넷(t1·t3 RefMapTarget, t2 RefMapDerived, o1 RefMapOther)을
// 한 트랜잭션으로 만들어 공개하고 holder root를 돌려준다(값은 staged AssignMapReferences로 채운다).
Result<RootRef<Object>> MakeGraph(ObjectRegistry& objects, NamePool& names,
                                  const GraphOptions& options = {}) noexcept {
    Allocator& allocator = objects.ObjectAllocator();
    ConstructionContext transaction(objects, objects.OwnerAccess());
    auto holder = transaction.Create<RefMapHolder>(TypeIdOf<RefMapHolder>());
    auto t1 = transaction.Create<RefMapTarget>(TypeIdOf<RefMapTarget>());
    auto t2 = transaction.Create<RefMapDerived>(TypeIdOf<RefMapDerived>());
    auto t3 = transaction.Create<RefMapTarget>(TypeIdOf<RefMapTarget>());
    auto o1 = transaction.Create<RefMapOther>(TypeIdOf<RefMapOther>());
    if (!holder || !t1 || !t2 || !t3 || !o1) {
        return Result<RootRef<Object>>::Failure(ErrorCode::OutOfMemory);
    }

    auto pointer = transaction.Get<RefMapHolder>(holder.Value());
    auto socket = names.TryIntern("Socket");
    if (!pointer || !socket) {
        return Result<RootRef<Object>>::Failure(ErrorCode::OutOfMemory);
    }

    RefMapHolder& value = *pointer.Value();
    const ObjectHandle owner = holder.Value();
    String strings[3] = {String(allocator), String(allocator), String(allocator)};
    SoftObjectRef<> softs[1] = {SoftObjectRef<>(allocator)};
    bool ok = strings[0].TryAssign("zeta").IsOk() && strings[1].TryAssign("alpha").IsOk() &&
              strings[2].TryAssign("mid").IsOk() &&
              softs[0].TryAssign(AssetId{Guid{7, 7}}, "Mesh.Socket").IsOk();
    const ObjectHandle stringTargets[] = {t1.Value(), {}, t2.Value()};
    const uint32 numbers[] = {9, 2};
    const ObjectHandle numberTargets[] = {o1.Value(), t1.Value()};
    const RefMapSlot slots[] = {RefMapSlot::Feet, RefMapSlot::Head};
    const ObjectHandle slotTargets[] = {t3.Value(), t1.Value()};
    const ClassRef<RefMapTarget> classes[] = {ClassRef<RefMapTarget>(TypeIdOf<RefMapDerived>()),
                                              ClassRef<RefMapTarget>(TypeIdOf<RefMapTarget>())};
    const ObjectHandle classTargets[] = {t2.Value(), {}};
    const ObjectHandle softTargets[] = {t3.Value()};
    const uint32 requiredKeys[] = {1};
    const ObjectHandle requiredTargets[] = {options.requiredNull ? ObjectHandle{} : t1.Value()};
    const uint32 limitedKeys[] = {10, 20, 30};
    const ObjectHandle limitedTargets[] = {t1.Value(), t2.Value(), t3.Value()};
    const Name nameKeys[] = {socket.Value()};
    const ObjectHandle nameTargets[] = {t3.Value()};
    ok = ok &&
         transaction.AssignMapReferences(owner, value.byString, Span<const String>(strings, 3),
                                         Span<const ObjectHandle>(stringTargets)) &&
         transaction.AssignMapReferences(owner, value.byNumber, Span<const uint32>(numbers),
                                         Span<const ObjectHandle>(numberTargets)) &&
         transaction.AssignMapReferences(owner, value.bySlot, Span<const RefMapSlot>(slots),
                                         Span<const ObjectHandle>(slotTargets)) &&
         transaction.AssignMapReferences(owner, value.byClass,
                                         Span<const ClassRef<RefMapTarget>>(classes),
                                         Span<const ObjectHandle>(classTargets)) &&
         transaction.AssignMapReferences(owner, value.bySoft, Span<const SoftObjectRef<>>(softs, 1),
                                         Span<const ObjectHandle>(softTargets)) &&
         transaction.AssignMapReferences(owner, value.required, Span<const uint32>(requiredKeys),
                                         Span<const ObjectHandle>(requiredTargets)) &&
         transaction.AssignMapReferences(
             owner, value.limited, Span<const uint32>(limitedKeys, options.limitedCount),
             Span<const ObjectHandle>(limitedTargets, options.limitedCount)) &&
         transaction.AssignMapReferences(owner, value.bundle.byName, Span<const Name>(nameKeys),
                                         Span<const ObjectHandle>(nameTargets));
    if (!ok) {
        return Result<RootRef<Object>>::Failure(ErrorCode::OutOfMemory);
    }

    value.bundle.weight = 5;
    return transaction.Commit();
}

// 로드한 holder의 값이 MakeGraph가 넣은 그대로인지 본다(키·순서·null·대상 타입, 같은 대상은 같은
// handle). 값으로 닿는 대상 넷을 reached에 돌려준다(t1·t2·t3·o1).
bool CheckFilled(ObjectRegistry& objects, NamePool& names, const RefMapHolder& value,
                 ObjectHandle (&reached)[4]) noexcept {
    const auto access = objects.OwnerAccess();
    const StringView order[] = {"zeta", "alpha", "mid"};
    usize position = 0;
    for (auto entry : value.byString.View()) {
        if (position >= 3 || entry.key.View() != order[position++]) {
            return false;
        }
    }

    const ObjectHandle* zeta = value.byString.Find(StringView("zeta"));
    const ObjectHandle* alpha = value.byString.Find(StringView("alpha"));
    const ObjectHandle* mid = value.byString.Find(StringView("mid"));
    if (position != 3 || !zeta || !alpha || !mid || *alpha != ObjectHandle{}) {
        return false;
    }

    reached[0] = *zeta;
    reached[1] = *mid;
    const ObjectHandle* feet = value.bySlot.Find(RefMapSlot::Feet);
    const ObjectHandle* nine = value.byNumber.Find(uint32{9});
    if (!feet || !nine) {
        return false;
    }

    reached[2] = *feet;
    reached[3] = *nine;
    SoftObjectRefView soft{AssetId{Guid{7, 7}}, StringView("Mesh.Socket")};
    auto socket = names.TryFind(StringView("Socket"));
    const ObjectHandle* two = value.byNumber.Find(uint32{2});
    const ObjectHandle* head = value.bySlot.Find(RefMapSlot::Head);
    const ObjectHandle* derived =
        value.byClass.Find(ClassRef<RefMapTarget>(TypeIdOf<RefMapDerived>()));
    const ObjectHandle* base = value.byClass.Find(ClassRef<RefMapTarget>(TypeIdOf<RefMapTarget>()));
    const ObjectHandle* softValue = value.bySoft.Find(soft);
    const ObjectHandle* one = value.required.Find(uint32{1});
    const ObjectHandle* named = socket ? value.bundle.byName.Find(socket.Value()) : nullptr;
    const bool ok = two && *two == reached[0] && head && *head == reached[0] && derived &&
                    *derived == reached[1] && base && *base == ObjectHandle{} && softValue &&
                    *softValue == reached[2] && one && *one == reached[0] && named &&
                    *named == reached[2] && value.limited.Count() == 2 &&
                    value.bundle.weight == 5 &&
                    objects.Resolve<RefMapDerived>(access, reached[1]).IsOk() &&
                    objects.Resolve<RefMapTarget>(access, reached[2]).IsOk() &&
                    objects.Resolve<RefMapOther>(access, reached[3]).IsOk() &&
                    !objects.Resolve<RefMapDerived>(access, reached[0]).IsOk();
    return ok && reached[0] != reached[2];
}

} // namespace

// 모듈 집계 등록이 ObjectRefMap 필드의 생성 스키마(value {Map, 0}·값 {ObjectReference, 대상}·키
// 형식·maxElements·FieldKind::ObjectReference·Required)와 함수표(read·write·writeStaged 없음,
// codec, 경로 thunk, 키 enum native 키)를 싣고, 인라인 struct가 Registry 컨테이너 형질을 가진다.
TEST(ObjectRefMapGraph_ModuleRegistrationDescribesObjectRefMapFields) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    {
        ModuleRegistration registration(types, allocator);
        REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(registration));
        {
            auto holder = types.Acquire(TypeIdOf<RefMapHolder>());
            auto bundle = types.AcquireValueType(TypeIdOf<RefMapBundle>(), TypeKind::Struct);
            REQUIRE(holder && bundle);
            CHECK(bundle.Value().Struct().ContainsReferenceArray());
            CHECK(bundle.Value().Struct().ContainsStrongReference());
            CHECK(bundle.Value().StructOps().trace != nullptr);
            struct Expected {
                const char* name;
                ValueKind key;
                TypeId keyTarget;
                TypeId target;
                uint32 maxElements;
            };

            const Expected expected[] = {
                {"byString",
                 ValueKind::Utf8String,
                 {},
                 TypeIdOf<RefMapTarget>(),
                 DefaultMaxElements},
                {"byNumber", ValueKind::UInt32, {}, {}, DefaultMaxElements},
                {"bySlot", ValueKind::Enum, TypeIdOf<RefMapSlot>(), TypeIdOf<RefMapTarget>(),
                 DefaultMaxElements},
                {"byClass", ValueKind::ClassReference, TypeIdOf<RefMapTarget>(),
                 TypeIdOf<RefMapTarget>(), DefaultMaxElements},
                {"bySoft",
                 ValueKind::SoftObjectReference,
                 {},
                 TypeIdOf<RefMapTarget>(),
                 DefaultMaxElements},
                {"required", ValueKind::UInt32, {}, TypeIdOf<RefMapTarget>(), DefaultMaxElements},
                {"limited", ValueKind::UInt32, {}, TypeIdOf<RefMapTarget>(), 2}};
            for (const Expected& entry : expected) {
                const FieldSchema* field =
                    FindField(holder.Value().Schema(), StringView(entry.name));
                REQUIRE(field);
                CHECK(field->kind == FieldKind::ObjectReference);
                CHECK(field->value.kind == ValueKind::Map && field->value.type == TypeId{});
                CHECK(field->element.kind == ValueKind::ObjectReference &&
                      field->element.type == entry.target);
                CHECK(field->key.kind == entry.key && field->key.type == entry.keyTarget);
                CHECK(field->maxElements == entry.maxElements);
                CHECK(HasFlag(field->flags, FieldFlags::Required) ==
                      (StringView(entry.name) == "required"));
                const RuntimeFieldOps* found = nullptr;
                for (const RuntimeFieldOps& candidate : holder.Value().Ops().fields) {
                    found = candidate.fieldId == field->id ? &candidate : found;
                }

                REQUIRE(found);
                CHECK(found->read == nullptr && !found->write && found->writeStaged == nullptr);
                CHECK(found->encode != nullptr && found->decode != nullptr &&
                      found->path != nullptr);
                CHECK(found->valueNativeType == nullptr);
                CHECK(found->keyNativeType ==
                      (entry.key == ValueKind::Enum ? NativeTypeKey<RefMapSlot>() : nullptr));
            }

            const FieldSchema* byName = FindField(bundle.Value().Schema(), StringView("byName"));
            REQUIRE(byName);
            CHECK(byName->kind == FieldKind::ObjectReference &&
                  byName->key.kind == ValueKind::Name);
        }

        REQUIRE(registration.Unregister());
    }

    CHECK(types.Shutdown());
}

// 키 종류별 값(null 포함)과 삽입 순서가 그대로 돌아오고 다른 Registry에서 다시 저장한 byte가 같다.
// 생성 Trace가 ObjectRefMap 값과 인라인 struct 안 ObjectRefMap(bundle.byName)을 방문해 값으로만
// 닿는 대상 넷이 수집에서 살고, root를 놓으면 모두 회수된다.
TEST(ObjectRefMapGraph_RoundTripKeysValuesOrderAndTrace) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    NamePool names(allocator);
    REQUIRE(names.Initialize());
    {
        ModuleRegistration registration(types, allocator);
        REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(registration));
        {
            ObjectRegistry source(allocator, types, {0x524d47, 1}, WithNames(names));
            ObjectRegistry target(allocator, types, {0x524d47, 2}, WithNames(names));
            {
                auto root = MakeGraph(source, names);
                REQUIRE(root);
                REQUIRE(source.Collect(source.OwnerAccess()));
                CHECK(source.Stats().liveObjects == 5);
                auto bytes =
                    SaveObjectGraph(allocator, source, source.OwnerAccess(), root.Value().Handle());
                REQUIRE(bytes);
                auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                              bytes.Value().View());
                REQUIRE(loaded);
                const ObjectHandle self = loaded.Value().Handle();
                ObjectHandle reached[4] = {};
                {
                    auto borrowed = target.Resolve<RefMapHolder>(target.OwnerAccess(), self);
                    REQUIRE(borrowed);
                    CHECK(CheckFilled(target, names, *borrowed.Value().Get().Value(), reached));
                }

                REQUIRE(target.Collect(target.OwnerAccess()));
                CHECK(target.Stats().liveObjects == 5);
                auto resaved = SaveObjectGraph(allocator, target, target.OwnerAccess(), self);
                REQUIRE(resaved);
                CHECK(test::graph::SameBytes(bytes.Value().View(), resaved.Value().View()));
                loaded.Value().Reset();
                root.Value().Reset();
                REQUIRE(target.Collect(target.OwnerAccess()));
                REQUIRE(source.Collect(source.OwnerAccess()));
                CHECK(target.Stats().liveObjects == 0 && source.Stats().liveObjects == 0);
            }

            REQUIRE(source.Shutdown(source.OwnerAccess()));
            REQUIRE(target.Shutdown(target.OwnerAccess()));
        }

        REQUIRE(registration.Unregister());
    }

    CHECK(names.Shutdown());
    CHECK(types.Shutdown());
}

// Required ObjectRefMap의 null 값은 rejectMissingRequired 저장에서 경로 [required[0]]의
// RequiredMissing 뒤 InvalidArgument이고 로드는 진단만 낸다(값마다 non-null — 키에는 Required가
// 없다). MaxElements = 2인 limited는 정확히 2개면 저장하고, 넘기면 저장에서 ArrayTooLong 뒤
// CapacityExceeded다.
TEST(ObjectRefMapGraph_RequiredNullValueAndMaxElements) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    NamePool names(allocator);
    REQUIRE(names.Initialize());
    {
        ModuleRegistration registration(types, allocator);
        REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(registration));
        Guid requiredId{};
        Guid limitedId{};
        {
            auto holder = types.Acquire(TypeIdOf<RefMapHolder>());
            REQUIRE(holder);
            requiredId = FindField(holder.Value().Schema(), StringView("required"))->id;
            limitedId = FindField(holder.Value().Schema(), StringView("limited"))->id;
        }

        {
            ObjectRegistry source(allocator, types, {0x524d47, 3}, WithNames(names));
            ObjectRegistry target(allocator, types, {0x524d47, 4}, WithNames(names));
            {
                GraphOptions nullRequired;
                nullRequired.requiredNull = true;
                auto root = MakeGraph(source, names, nullRequired);
                REQUIRE(root);
                Diagnostics saveHooks;
                ObjectGraphSaveOptions strict;
                strict.policy.rejectMissingRequired = true;
                strict.hooks = &saveHooks;
                CHECK(SaveObjectGraph(allocator, source, source.OwnerAccess(),
                                      root.Value().Handle(), strict)
                          .Error() == ErrorCode::InvalidArgument);
                REQUIRE(saveHooks.count == 1);
                CHECK(saveHooks.entries[0].kind == FieldDiagnosticKind::RequiredMissing);
                CHECK(saveHooks.entries[0].depth == 1);
                CHECK(saveHooks.entries[0].first.fieldId == requiredId);
                CHECK(saveHooks.entries[0].first.selector == PathSelector::Index &&
                      saveHooks.entries[0].first.index == 0);

                auto bytes =
                    SaveObjectGraph(allocator, source, source.OwnerAccess(), root.Value().Handle());
                REQUIRE(bytes);
                Diagnostics loadHooks;
                ObjectGraphLoadOptions load;
                load.hooks = &loadHooks;
                auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                              bytes.Value().View(), load);
                CHECK(static_cast<bool>(loaded));
                REQUIRE(loadHooks.count == 1);
                CHECK(loadHooks.entries[0].kind == FieldDiagnosticKind::RequiredMissing);
                CHECK(loadHooks.entries[0].first.fieldId == requiredId);
                root.Value().Reset();

                GraphOptions overLimit;
                overLimit.limitedCount = 3;
                auto longRoot = MakeGraph(source, names, overLimit);
                REQUIRE(longRoot);
                Diagnostics lengthHooks;
                ObjectGraphSaveOptions reported;
                reported.hooks = &lengthHooks;
                CHECK(SaveObjectGraph(allocator, source, source.OwnerAccess(),
                                      longRoot.Value().Handle(), reported)
                          .Error() == ErrorCode::CapacityExceeded);
                REQUIRE(lengthHooks.count == 1);
                CHECK(lengthHooks.entries[0].kind == FieldDiagnosticKind::ArrayTooLong);
                CHECK(lengthHooks.entries[0].value == 3);
                CHECK(lengthHooks.entries[0].first.fieldId == limitedId);
                longRoot.Value().Reset();
                if (loaded) {
                    loaded.Value().Reset();
                }

                REQUIRE(source.Collect(source.OwnerAccess()));
                REQUIRE(target.Collect(target.OwnerAccess()));
            }

            REQUIRE(source.Shutdown(source.OwnerAccess()));
            REQUIRE(target.Shutdown(target.OwnerAccess()));
        }

        REQUIRE(registration.Unregister());
    }

    CHECK(names.Shutdown());
    CHECK(types.Shutdown());
}

namespace {

// byString 필드(키 String·값 U32 local ID)만 담은 holder 노드(local 1) 뒤에 RefMapTarget 노드(local
// 2)가 있는 그래프다. withOther면 그 사이에 RefMapOther 노드를 두어 RefMapTarget이 local 3이 된다.
// entries는 (키, local ID) 쌍이다(local ID = 노드 위치 + 1, 0은 null).
struct RawEntry {
    StringView key;
    uint32 local = 0;
};

Array<uint8> RawGraph(Allocator& allocator, Guid field, Span<const RawEntry> entries,
                      bool withOther) noexcept {
    ArchiveWriter payload(allocator);
    bool ok = payload.WriteU32(static_cast<uint32>(WireType::String)).IsOk() &&
              payload.WriteU32(static_cast<uint32>(WireType::U32)).IsOk() &&
              payload.WriteU32(static_cast<uint32>(entries.Size())).IsOk();
    for (const RawEntry& entry : entries) {
        ok = ok && payload.WriteString(entry.key).IsOk() && payload.WriteU32(entry.local).IsOk();
    }

    if (!ok) {
        platform::Fatal("raw ObjectRefMap payload failed");
    }

    auto bytes = payload.TakeBytes();
    RecordBuilder holder(allocator, TypeIdOf<RefMapHolder>(), TypeOf<RefMapHolder>().version);
    RecordBuilder other(allocator, TypeIdOf<RefMapOther>(), TypeOf<RefMapOther>().version);
    RecordBuilder target(allocator, TypeIdOf<RefMapTarget>(), TypeOf<RefMapTarget>().version);
    if (!holder.AddField(field, static_cast<uint32>(WireType::Map), bytes.View())) {
        platform::Fatal("raw ObjectRefMap record failed");
    }

    auto holderRecord = holder.Build();
    auto otherRecord = other.Build();
    auto targetRecord = target.Build();
    if (!holderRecord || !otherRecord || !targetRecord) {
        platform::Fatal("raw ObjectRefMap records failed");
    }

    const test::graph::Node withOtherNodes[] = {
        {TypeIdOf<RefMapHolder>(), 0, {}, holderRecord.Value().View()},
        {TypeIdOf<RefMapOther>(), 0, {}, otherRecord.Value().View()},
        {TypeIdOf<RefMapTarget>(), 0, {}, targetRecord.Value().View()}};
    const test::graph::Node plainNodes[] = {
        {TypeIdOf<RefMapHolder>(), 0, {}, holderRecord.Value().View()},
        {TypeIdOf<RefMapTarget>(), 0, {}, targetRecord.Value().View()}};
    auto graph =
        withOther ? test::graph::Build(allocator, Span<const test::graph::Node>(withOtherNodes, 3))
                  : test::graph::Build(allocator, Span<const test::graph::Node>(plainNodes, 2));
    if (!graph) {
        platform::Fatal("raw ObjectRefMap graph failed");
    }

    return Move(graph.Value());
}

ErrorCode LoadCode(Allocator& allocator, ObjectRegistry& objects, TypeRegistry& types,
                   Span<const uint8> graph) noexcept {
    auto loaded = LoadObjectGraph(allocator, objects, types, objects.OwnerAccess(), graph);
    return loaded ? ErrorCode::None : loaded.Error();
}

} // namespace

// 손상 입력: 같은 키 둘은 InvalidArgument(1단계 판정), 값이 대상 타입(RefMapTarget)이 아닌 노드
// (RefMapOther)면 TypeMismatch, 범위 밖 local ID는 InvalidArgument다. 모두 객체를 만들지 않는다.
// 같은 모양의 올바른 값(null·RefMapTarget 노드)은 읽힌다.
TEST(ObjectRefMapGraph_DuplicateKeysAndBadTargetsAreRejected) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    {
        ModuleRegistration registration(types, allocator);
        REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(registration));
        Guid field{};
        {
            auto holder = types.Acquire(TypeIdOf<RefMapHolder>());
            REQUIRE(holder);
            field = FindField(holder.Value().Schema(), StringView("byString"))->id;
        }

        {
            ObjectRegistry objects(allocator, types, {0x524d47, 5});
            // 대상 노드가 값으로 닿으므로(고아가 아니다) 아래 실패는 그 입력 오류 때문이다.
            const RawEntry duplicate[] = {{"a", 2}, {"a", 0}};
            const RawEntry wrongType[] = {{"a", 3}, {"b", 2}};
            const RawEntry outOfRange[] = {{"a", 2}, {"b", 9}};
            const RawEntry valid[] = {{"a", 2}, {"b", 0}};
            CHECK(LoadCode(allocator, objects, types,
                           RawGraph(allocator, field, duplicate, false).View()) ==
                  ErrorCode::InvalidArgument);
            CHECK(LoadCode(allocator, objects, types,
                           RawGraph(allocator, field, wrongType, true).View()) ==
                  ErrorCode::TypeMismatch);
            CHECK(LoadCode(allocator, objects, types,
                           RawGraph(allocator, field, outOfRange, false).View()) ==
                  ErrorCode::InvalidArgument);
            CHECK(objects.Stats().liveObjects == 0 && objects.Stats().reservedObjects == 0);
            CHECK(LoadCode(allocator, objects, types,
                           RawGraph(allocator, field, valid, false).View()) == ErrorCode::None);
            REQUIRE(objects.Collect(objects.OwnerAccess()));
            REQUIRE(objects.Shutdown(objects.OwnerAccess()));
        }

        REQUIRE(registration.Unregister());
    }

    CHECK(types.Shutdown());
}
