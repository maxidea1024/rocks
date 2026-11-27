#include <ReflectionTestFixtures/EventFixture.generated.h>
#include <ReflectionTestFixtures/FunctionFixture.generated.h>
#include <ReflectionTestFixtures/ReflectionTestFixtures.module.generated.h>
#include <Base/Compiler.h>
#include <Core/Memory/SystemAllocator.h>
#include <Object/EventOps.h>
#include <Object/ModuleRegistration.h>
#include <Object/PropertyPath.h>
#include <ObjectSerialization/ClassDefaults.h>
#include <ObjectSerialization/ObjectGraph.h>
#include <Platform/Fatal.h>
#include <TestSupport/Test.h>
#include "GraphTestSupport.h"

// 이벤트 필드의 값 경로 시험이다(Docs/Design/EventReflection.ko.md 3.1절 — EV2b, fixture
// ReflectionTestFixtures/EventFixture.h): 값 필드와 이벤트 필드가 섞인 클래스의 그래프 저장·로드
// 왕복(이벤트는 값으로 저장되지 않고 바인딩은 로드한 객체에 없다, 다시 저장한 byte 같음), 바인딩이
// 있어도 저장 byte가 같음, 클래스 기본값 레코드·바뀐 필드 판정, 속성 경로가 이벤트 필드를 찾지
// 않음.
using namespace rocks;
using reflection_fixture::CallTarget;
using reflection_fixture::EventPublisher;
using reflection_fixture::EventPublisherChild;

namespace event_field_graph_tests_file {

namespace {

struct Registries {
    explicit Registries(Allocator& allocator) noexcept
        : types(allocator), registration(types, allocator),
          source(allocator, types, {0x45564701, 1}), target(allocator, types, {0x45564701, 2}) {
        if (!generated::ReflectionTestFixtures::RegisterModuleTypes(registration)) {
            platform::Fatal("fixture registration failed");
        }
    }

    TypeRegistry types;
    ModuleRegistration registration;
    ObjectRegistry source;
    ObjectRegistry target;
};

template <class T>
RootRef<T> New(ObjectRegistry& objects) noexcept {
    auto created = objects.NewObject<T>(objects.OwnerAccess(), TypeIdOf<T>());
    if (!created) {
        platform::Fatal("test object creation failed");
    }

    return Move(created.Value());
}

template <class T, class Edit>
void With(ObjectRegistry& objects, ObjectHandle handle, Edit&& edit) noexcept {
    auto borrowed = objects.ResolveForWrite<T>(objects.OwnerAccess(), handle);
    REQUIRE(borrowed);
    auto value = borrowed.Value().Get();
    REQUIRE(value);
    edit(*value.Value());
}

Guid FunctionIdOf(TypeRegistry& types, const char* name) noexcept {
    auto function = types.FindFunctionByName(TypeIdOf<CallTarget>(), name);
    return function ? function.Value().Schema().id : Guid{};
}

// 진단을 모은다(종류·값).
struct Collected final : FieldCodecHooks {
    Result<void> Report(const FieldDiagnostic& diagnostic) noexcept override {
        if (count < Max) {
            kinds[count] = diagnostic.kind;
            values[count] = diagnostic.value;
        }

        ++count;
        return Remember(diagnostic);
    }

    Result<void> Remember(const FieldDiagnostic& diagnostic) noexcept {
        if (diagnostic.kind == FieldDiagnosticKind::EventBindingDropped && !dropped) {
            dropped = true;
            droppedIndex = diagnostic.objectIndex;
            droppedField = diagnostic.field;
            droppedFunction = diagnostic.valueType;
            droppedPathSteps = diagnostic.path.Size();
            droppedType = diagnostic.objectType;
        }

        return Result<void>::Success();
    }

    bool Has(FieldDiagnosticKind kind, int64 value) const noexcept {
        for (usize index = 0; index < count && index < Max; ++index) {
            if (kinds[index] == kind && values[index] == value) {
                return true;
            }
        }

        return false;
    }

    static constexpr usize Max = 16;
    FieldDiagnosticKind kinds[Max] = {};
    int64 values[Max] = {};
    usize count = 0;
    bool dropped = false;
    uint32 droppedIndex = 0;
    Guid droppedField{};
    Guid droppedFunction{};
    usize droppedPathSteps = 0;
    TypeId droppedType{};
};

int32 TotalOf(ObjectRegistry& objects, ObjectHandle handle) noexcept {
    auto borrow = objects.Resolve<CallTarget>(objects.OwnerAccess(), handle);
    if (!borrow) {
        return -1;
    }

    auto value = borrow.Value().Get();
    return value ? value.Value()->total : -1;
}

// 그래프 안 listener(publisher의 Inner)를 만든다.
RootRef<CallTarget> NewListener(ObjectRegistry& objects, ObjectHandle outer) noexcept {
    auto created = objects.NewObject<CallTarget>(objects.OwnerAccess(), TypeIdOf<CallTarget>(),
                                                 outer, "listener");
    if (!created) {
        platform::Fatal("test listener creation failed");
    }

    return Move(created.Value());
}

// 바이트 열에서 needle(16 byte Guid 표현)을 모두 replacement로 바꾸고 바꾼 수를 돌려준다.
usize PatchGuid(Array<uint8>& bytes, Guid needle, Guid replacement) noexcept {
    uint8 from[sizeof(Guid)];
    uint8 to[sizeof(Guid)];
    for (usize index = 0; index < 8; ++index) {
        from[index] = static_cast<uint8>(needle.high >> (8 * index));
        from[8 + index] = static_cast<uint8>(needle.low >> (8 * index));
        to[index] = static_cast<uint8>(replacement.high >> (8 * index));
        to[8 + index] = static_cast<uint8>(replacement.low >> (8 * index));
    }

    usize patched = 0;
    for (usize at = 0; at + sizeof(Guid) <= bytes.Size(); ++at) {
        bool same = true;
        for (usize index = 0; index < sizeof(Guid) && same; ++index) {
            same = bytes[at + index] == from[index];
        }

        if (same) {
            for (usize index = 0; index < sizeof(Guid); ++index) {
                bytes[at + index] = to[index];
            }

            ++patched;
        }
    }

    return patched;
}

// 발행자와 그 Inner listener를 한 그래프로 저장한다(NodeTable·Include — listener는 강한 참조로 닿지
// 않으므로 노드로 명시한다).
Result<Array<uint8>> SaveSet(Allocator& allocator, ObjectRegistry& objects, ObjectHandle publisher,
                             ObjectHandle listener, ObjectGraphSaveOptions options = {}) noexcept {
    options.writeNodeTable = true;
    options.externals = GraphExternalPolicy::Include;
    const ObjectHandle nodes[] = {publisher, listener};
    auto saved = SaveObjectGraphSet(allocator, objects, objects.OwnerAccess(),
                                    Span<const ObjectHandle>(nodes, 2), options);
    if (!saved) {
        return Result<Array<uint8>>::Failure(saved.Error());
    }

    return Result<Array<uint8>>::Success(saved.Value().TakeBytes());
}

// 단계형 복원으로 두 노드 그래프를 읽고 두 노드를 root로 Commit한 뒤 이벤트 바인딩을 만든다
// (LoadObjectGraph는 노드 1에서 강한 참조로 닿지 않는 Inner listener를 손상으로 거부한다).
struct LoadedSet {
    RootRef<Object> roots[2];
    usize bound = 0;
};

Result<LoadedSet> LoadSet(Allocator& allocator, ObjectRegistry& objects, TypeRegistry& types,
                          Span<const uint8> bytes, ObjectGraphLoadOptions options = {}) noexcept {
    ObjectGraphStage stage(allocator, objects, types, objects.OwnerAccess(), options);
    ObjectGraphInput input;
    input.bytes = bytes;
    if (auto validated = stage.Validate(input); !validated) {
        return Result<LoadedSet>::Failure(validated.Error());
    }

    ConstructionContext transaction(objects, objects.OwnerAccess());
    Result<void> step = stage.CreateObjects(transaction);
    if (step) {
        step = stage.ApplyFields(transaction);
    }

    if (step) {
        step = stage.CheckRequired(transaction);
    }

    LoadedSet loaded;
    if (step) {
        step = transaction.Commit(stage.Nodes(), Span<RootRef<Object>>(loaded.roots, 2));
    }

    if (!step) {
        return Result<LoadedSet>::Failure(step.Error());
    }

    auto bound = stage.BindEvents();
    if (!bound) {
        return Result<LoadedSet>::Failure(bound.Error());
    }

    loaded.bound = bound.Value();
    return Result<LoadedSet>::Success(Move(loaded));
}

// 기록 항목 schema Guid 바로 뒤의 판(u32 LE)을 version으로 바꾸고 바꾼 수를 돌려준다.
usize PatchRecordVersion(Array<uint8>& bytes, Guid schema, uint32 version) noexcept {
    uint8 key[sizeof(Guid)];
    for (usize index = 0; index < 8; ++index) {
        key[index] = static_cast<uint8>(schema.high >> (8 * index));
        key[8 + index] = static_cast<uint8>(schema.low >> (8 * index));
    }

    usize patched = 0;
    for (usize at = 0; at + sizeof(Guid) + sizeof(uint32) <= bytes.Size(); ++at) {
        bool same = true;
        for (usize index = 0; index < sizeof(Guid) && same; ++index) {
            same = bytes[at + index] == key[index];
        }

        if (same) {
            for (usize index = 0; index < sizeof(uint32); ++index) {
                bytes[at + sizeof(Guid) + index] = static_cast<uint8>(version >> (8 * index));
            }

            ++patched;
        }
    }

    return patched;
}

Result<Array<uint8>> CopyBytes(Allocator& allocator, Span<const uint8> bytes) noexcept {
    Array<uint8> copy(allocator);
    if (auto reserved = copy.TryReserve(bytes.Size()); !reserved) {
        return Result<Array<uint8>>::Failure(reserved.Error());
    }

    for (uint8 byte : bytes) {
        (void)copy.TryPushBack(byte);
    }

    return Result<Array<uint8>>::Success(Move(copy));
}

} // namespace

// 그래프 안 대상의 리플렉션 바인딩은 저장·로드 뒤 로드한 객체 사이에 다시 만들어진다(ED19–ED21).
// Transient 이벤트·native 구독은 저장하지 않는다. 다시 저장한 byte가 같다.
TEST(EventFieldGraph_RoundTripRebindsEvents) {
    SystemAllocator allocator;
    {
        Registries registries(allocator);
        ObjectRegistry& source = registries.source;
        const ObjectAccessContext access = source.OwnerAccess();
        RootRef<EventPublisherChild> publisher = New<EventPublisherChild>(source);
        RootRef<CallTarget> listener = NewListener(source, publisher.Handle());
        const Guid add = FunctionIdOf(registries.types, "Add");
        const Guid bump = FunctionIdOf(registries.types, "Bump");
        // 바인딩이 없으면 이벤트 필드를 쓰지 않는다(바인딩이 생겼다 사라지면 byte가 처음과 같다).
        auto plain = SaveSet(allocator, source, publisher.Handle(), listener.Handle());
        REQUIRE(plain);
        With<EventPublisherChild>(
            source, publisher.Handle(), [&](EventPublisherChild& value) noexcept {
                value.count = 7;
                REQUIRE(value.changed.TryBind(source, access, listener.Handle(), add));
                // Transient 이벤트(extra)의 리플렉션 바인딩은 저장하지 않는다.
                REQUIRE(value.extra.TryBind(source, access, listener.Handle(), add));
                REQUIRE(value.pinged.TryBind(source, access, listener.Handle(), bump));
                REQUIRE(value.changed.TryAddLambda([](int32) noexcept {}));
            });
        auto bound = SaveSet(allocator, source, publisher.Handle(), listener.Handle());
        REQUIRE(bound);
        CHECK(!test::graph::SameBytes(plain.Value().View(), bound.Value().View()));

        ObjectRegistry& target = registries.target;
        auto loaded = LoadSet(allocator, target, registries.types, bound.Value().View());
        REQUIRE(loaded);
        CHECK(loaded.Value().bound == 2);
        const ObjectHandle copy = loaded.Value().roots[0].Handle();
        const ObjectHandle copiedListener = loaded.Value().roots[1].Handle();
        // 발행 전에 다시 저장하면 같은 byte다(바인딩 순서·대상 번호가 같다).
        auto again = SaveSet(allocator, target, copy, copiedListener);
        REQUIRE(again);
        CHECK(test::graph::SameBytes(bound.Value().View(), again.Value().View()));

        With<EventPublisherChild>(target, copy, [&](EventPublisherChild& value) noexcept {
            CHECK(value.count == 7);
            CHECK(value.changed.BindingCount() == 1 && value.extra.BindingCount() == 0 &&
                  value.pinged.BindingCount() == 1);
            // native 구독은 저장되지 않는다: 리플렉션 바인딩 하나만 불린다.
            REQUIRE(
                value.changed.TryBroadcast(EventContext{target, target.OwnerAccess(), copy}, 5));
            REQUIRE(value.pinged.TryBroadcast(EventContext{target, target.OwnerAccess(), copy}));
        });
        CHECK(TotalOf(target, copiedListener) == 105);
        CHECK(TotalOf(source, listener.Handle()) == 0);

        // 바인딩을 모두 지우면 처음 byte로 돌아간다.
        With<EventPublisherChild>(source, publisher.Handle(),
                                  [&](EventPublisherChild& value) noexcept {
                                      value.count = 0;
                                      value.changed.Clear();
                                      value.extra.Clear();
                                      value.pinged.Clear();
                                  });
        auto cleared = SaveSet(allocator, source, publisher.Handle(), listener.Handle());
        REQUIRE(cleared);
        CHECK(test::graph::SameBytes(plain.Value().View(), cleared.Value().View()));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 그래프 밖 대상의 바인딩은 null로 저장되고(ExternalReference 2) 로드가 버린다. 저장 정책 flag:
// Transient 이벤트는 쓰지 않고, 세이브 게임 filter(SaveGame)는 SaveGame 이벤트만 쓴다.
TEST(EventFieldGraph_PoliciesAndOutsideTargets) {
    SystemAllocator allocator;
    {
        Registries registries(allocator);
        ObjectRegistry& source = registries.source;
        const ObjectAccessContext access = source.OwnerAccess();
        RootRef<EventPublisher> publisher = New<EventPublisher>(source);
        RootRef<CallTarget> inside = NewListener(source, publisher.Handle());
        RootRef<CallTarget> outside = New<CallTarget>(source);
        const Guid add = FunctionIdOf(registries.types, "Add");
        const Guid bump = FunctionIdOf(registries.types, "Bump");
        With<EventPublisher>(source, publisher.Handle(), [&](EventPublisher& value) noexcept {
            REQUIRE(value.changed.TryBind(source, access, outside.Handle(), add));
            REQUIRE(value.pinged.TryBind(source, access, inside.Handle(), bump));
        });

        Collected saveDiagnostics;
        ObjectGraphSaveOptions options;
        options.hooks = &saveDiagnostics;
        auto saved = SaveSet(allocator, source, publisher.Handle(), inside.Handle(), options);
        REQUIRE(saved);
        CHECK(saveDiagnostics.Has(FieldDiagnosticKind::ExternalReference, 2));
        auto loaded = LoadSet(allocator, registries.target, registries.types, saved.Value().View());
        REQUIRE(loaded);
        With<EventPublisher>(registries.target, loaded.Value().roots[0].Handle(),
                             [](EventPublisher& value) noexcept {
                                 CHECK(value.changed.BindingCount() == 0 &&
                                       value.pinged.BindingCount() == 1);
                             });

        // 세이브 게임 filter: SaveGame 이벤트(pinged)만 쓴다 — changed는 필드째 빠진다.
        ObjectGraphSaveOptions saveGame;
        saveGame.policy.includeFlags = static_cast<uint32>(FieldFlags::SaveGame);
        auto filtered = SaveSet(allocator, source, publisher.Handle(), inside.Handle(), saveGame);
        REQUIRE(filtered);
        ObjectRegistry third(allocator, registries.types, {0x45564701, 3});
        ObjectGraphLoadOptions loadSaveGame;
        loadSaveGame.policy.includeFlags = static_cast<uint32>(FieldFlags::SaveGame);
        auto restored =
            LoadSet(allocator, third, registries.types, filtered.Value().View(), loadSaveGame);
        REQUIRE(restored);
        With<EventPublisher>(
            third, restored.Value().roots[0].Handle(), [](EventPublisher& value) noexcept {
                CHECK(value.changed.BindingCount() == 0 && value.pinged.BindingCount() == 1);
            });
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 로드 뒤 바인딩을 만들지 못하면(함수 없음) 바인딩을 버리고 EventBindingDropped 진단을 낸다 —
// 로드는 성공한다. 기록 schema가 다르면 CorruptData다.
TEST(EventFieldGraph_DroppedBindingsAndCorruptRecords) {
    SystemAllocator allocator;
    {
        Registries registries(allocator);
        ObjectRegistry& source = registries.source;
        const ObjectAccessContext access = source.OwnerAccess();
        RootRef<EventPublisher> publisher = New<EventPublisher>(source);
        RootRef<CallTarget> listener = NewListener(source, publisher.Handle());
        const Guid add = FunctionIdOf(registries.types, "Add");
        With<EventPublisher>(source, publisher.Handle(), [&](EventPublisher& value) noexcept {
            REQUIRE(value.changed.TryBind(source, access, listener.Handle(), add));
        });
        auto saved = SaveSet(allocator, source, publisher.Handle(), listener.Handle());
        REQUIRE(saved);

        // FunctionId를 없는 ID로 바꾼다.
        auto missing = CopyBytes(allocator, saved.Value().View());
        REQUIRE(missing);
        REQUIRE(PatchGuid(missing.Value(), add, Guid{0x1234, 0x5678}) == 1);
        Collected diagnostics;
        ObjectGraphLoadOptions options;
        options.hooks = &diagnostics;
        auto loaded = LoadSet(allocator, registries.target, registries.types,
                              missing.Value().View(), options);
        REQUIRE(loaded);
        CHECK(diagnostics.Has(FieldDiagnosticKind::EventBindingDropped,
                              EventBindingDroppedFunctionMissing));
        // 진단 내용: 발행자 노드·타입, 이벤트 필드 경로 한 단계, 버린 FunctionId.
        CHECK(diagnostics.dropped && diagnostics.droppedIndex == 0 &&
              diagnostics.droppedType == TypeIdOf<EventPublisher>() &&
              diagnostics.droppedPathSteps == 1 &&
              diagnostics.droppedFunction == (Guid{0x1234, 0x5678}));
        With<EventPublisher>(
            registries.target, loaded.Value().roots[0].Handle(),
            [](EventPublisher& value) noexcept { CHECK(value.changed.BindingCount() == 0); });

        // 서명이 다른 함수(Bump — 인자 없음)로 바꾼다.
        auto mismatched = CopyBytes(allocator, saved.Value().View());
        REQUIRE(mismatched);
        REQUIRE(PatchGuid(mismatched.Value(), add, FunctionIdOf(registries.types, "Bump")) == 1);
        Collected mismatch;
        ObjectGraphLoadOptions mismatchOptions;
        mismatchOptions.hooks = &mismatch;
        ObjectRegistry third(allocator, registries.types, {0x45564701, 3});
        auto reloaded =
            LoadSet(allocator, third, registries.types, mismatched.Value().View(), mismatchOptions);
        REQUIRE(reloaded);
        CHECK(mismatch.Has(FieldDiagnosticKind::EventBindingDropped,
                           EventBindingDroppedSignatureMismatch));

        // 기록 항목 schema를 바꾸면 읽은 byte의 손상이다.
        auto corrupt = CopyBytes(allocator, saved.Value().View());
        REQUIRE(corrupt);
        REQUIRE(PatchGuid(corrupt.Value(), Guid{0x4556424e44524543, 0x0000000000000001},
                          Guid{0x4556424e44524543, 0x0000000000000009}) == 1);
        const Guid record{0x4556424e44524543, 0x0000000000000001};
        auto badVersion = CopyBytes(allocator, saved.Value().View());
        REQUIRE(badVersion);
        REQUIRE(PatchRecordVersion(badVersion.Value(), record, 2) == 1);
        auto zeroFunction = CopyBytes(allocator, saved.Value().View());
        REQUIRE(zeroFunction);
        REQUIRE(PatchGuid(zeroFunction.Value(), add, Guid{}) == 1);
        auto unknownMember = CopyBytes(allocator, saved.Value().View());
        REQUIRE(unknownMember);
        REQUIRE(PatchGuid(unknownMember.Value(), Guid{0x4556424e44524543, 0x0000000000000101},
                          Guid{0x4556424e44524543, 0x0000000000000199}) == 1);
        const Array<uint8>* variants[] = {&badVersion.Value(), &zeroFunction.Value(),
                                          &unknownMember.Value()};
        uint32 next = 10;
        for (const Array<uint8>* variant : variants) {
            ObjectRegistry scratch(allocator, registries.types, {0x45564701, next++});
            auto bad = LoadSet(allocator, scratch, registries.types, variant->View());
            CHECK(!bad && bad.Error() == ErrorCode::CorruptData);
        }

        ObjectRegistry fourth(allocator, registries.types, {0x45564701, 4});
        auto rejected = LoadSet(allocator, fourth, registries.types, corrupt.Value().View());
        CHECK(!rejected && rejected.Error() == ErrorCode::CorruptData);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// LoadObjectGraph는 Commit 뒤 스스로 다시 바인딩한다. 바인딩 대상이 강한 참조(partner)로 나중에
// 집합에 들어오면 Include 지연 덮어쓰기가 기록 안 자리를 고친다.
TEST(EventFieldGraph_LoadObjectGraphRebindsThroughStrongReference) {
    SystemAllocator allocator;
    {
        Registries registries(allocator);
        ObjectRegistry& source = registries.source;
        const ObjectAccessContext access = source.OwnerAccess();
        RootRef<EventPublisher> publisher = New<EventPublisher>(source);
        RootRef<CallTarget> listener = New<CallTarget>(source);
        With<EventPublisher>(source, publisher.Handle(), [&](EventPublisher& value) noexcept {
            REQUIRE(
                source.SetReference(access, publisher.Handle(), value.partner, listener.Handle()));
            REQUIRE(value.changed.TryBind(source, access, listener.Handle(),
                                          FunctionIdOf(registries.types, "Add")));
        });
        Collected saveDiagnostics;
        ObjectGraphSaveOptions options;
        options.hooks = &saveDiagnostics;
        auto saved = SaveObjectGraph(allocator, source, access, publisher.Handle(), options);
        REQUIRE(saved);
        CHECK(!saveDiagnostics.Has(FieldDiagnosticKind::ExternalReference, 2));

        ObjectRegistry& target = registries.target;
        auto loaded = LoadObjectGraph(allocator, target, registries.types, target.OwnerAccess(),
                                      saved.Value().View());
        REQUIRE(loaded);
        const ObjectHandle copy = loaded.Value().Handle();
        ObjectHandle partner{};
        With<EventPublisher>(target, copy, [&](EventPublisher& value) noexcept {
            CHECK(value.changed.BindingCount() == 1);
            partner = value.partner.Handle();
            REQUIRE(
                value.changed.TryBroadcast(EventContext{target, target.OwnerAccess(), copy}, 9));
        });
        CHECK(TotalOf(target, partner) == 9);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// Commit과 BindEvents 사이에 대상이 파괴 요청되면 TargetGone 진단, 발행자가 파괴 요청되면 진단 없이
// 건너뛴다.
TEST(EventFieldGraph_DestroyedBetweenCommitAndBind) {
    SystemAllocator allocator;
    {
        Registries registries(allocator);
        ObjectRegistry& source = registries.source;
        const ObjectAccessContext access = source.OwnerAccess();
        RootRef<EventPublisher> publisher = New<EventPublisher>(source);
        RootRef<CallTarget> listener = NewListener(source, publisher.Handle());
        With<EventPublisher>(source, publisher.Handle(), [&](EventPublisher& value) noexcept {
            REQUIRE(value.changed.TryBind(source, access, listener.Handle(),
                                          FunctionIdOf(registries.types, "Add")));
        });
        auto saved = SaveSet(allocator, source, publisher.Handle(), listener.Handle());
        REQUIRE(saved);

        for (int pass = 0; pass < 2; ++pass) {
            const bool destroyPublisher = pass == 1;
            ObjectRegistry target(allocator, registries.types,
                                  {0x45564702, destroyPublisher ? 2u : 1u});
            Collected diagnostics;
            ObjectGraphLoadOptions options;
            options.hooks = &diagnostics;
            ObjectGraphStage stage(allocator, target, registries.types, target.OwnerAccess(),
                                   options);
            ObjectGraphInput input;
            input.bytes = saved.Value().View();
            REQUIRE(stage.Validate(input));
            ConstructionContext transaction(target, target.OwnerAccess());
            REQUIRE(stage.CreateObjects(transaction));
            REQUIRE(stage.ApplyFields(transaction));
            REQUIRE(stage.CheckRequired(transaction));
            RootRef<Object> roots[2];
            REQUIRE(transaction.Commit(stage.Nodes(), Span<RootRef<Object>>(roots, 2)));
            REQUIRE(target.RequestDestroy(target.OwnerAccess(),
                                          roots[destroyPublisher ? 0 : 1].Handle()));
            auto bound = stage.BindEvents();
            REQUIRE(bound);
            CHECK(bound.Value() == 0);
            CHECK(diagnostics.Has(FieldDiagnosticKind::EventBindingDropped,
                                  EventBindingDroppedTargetGone) == !destroyPublisher);
            CHECK(diagnostics.dropped == !destroyPublisher);
        }
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 단계형 복원은 호출자가 Commit한 뒤 BindEvents를 부른다(Commit 전·두 번째 호출은 InvalidArgument).
TEST(EventFieldGraph_StageBindsAfterCommit) {
    SystemAllocator allocator;
    {
        Registries registries(allocator);
        ObjectRegistry& source = registries.source;
        const ObjectAccessContext access = source.OwnerAccess();
        RootRef<EventPublisher> publisher = New<EventPublisher>(source);
        RootRef<CallTarget> listener = NewListener(source, publisher.Handle());
        With<EventPublisher>(source, publisher.Handle(), [&](EventPublisher& value) noexcept {
            REQUIRE(value.changed.TryBind(source, access, listener.Handle(),
                                          FunctionIdOf(registries.types, "Add")));
        });
        auto saved = SaveSet(allocator, source, publisher.Handle(), listener.Handle());
        REQUIRE(saved);

        ObjectRegistry& target = registries.target;
        ObjectGraphStage stage(allocator, target, registries.types, target.OwnerAccess());
        ObjectGraphInput input;
        input.bytes = saved.Value().View();
        REQUIRE(stage.Validate(input));
        ConstructionContext transaction(target, target.OwnerAccess());
        REQUIRE(stage.CreateObjects(transaction));
        REQUIRE(stage.ApplyFields(transaction));
        REQUIRE(stage.CheckRequired(transaction));
        auto early = stage.BindEvents();
        CHECK(!early && early.Error() == ErrorCode::InvalidArgument);
        auto root = transaction.Commit();
        REQUIRE(root);
        auto bound = stage.BindEvents();
        REQUIRE(bound);
        CHECK(bound.Value() == 1);
        auto twice = stage.BindEvents();
        CHECK(!twice && twice.Error() == ErrorCode::InvalidArgument);
        With<EventPublisher>(target, root.Value().Handle(), [](EventPublisher& value) noexcept {
            CHECK(value.changed.BindingCount() == 1);
        });
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 에디터 로드(keepUnresolvedEventBindings)는 풀리지 않은 바인딩(함수 없음·서명 다름)을 미해결
// 레코드로 보존하고 EventBindingUnresolved로 알리며, 다시 저장한 byte가 읽은 byte와 같다(풀리지
// 않은 바인딩이 살아 있는 바인딩 앞에 있어도 순서가 지켜진다). 게임 로드는 버린다
// (EventBindingDropped). 쿠킹 저장은 미해결 레코드를 EventBindingNotCookable(2) 뒤
// InvalidArgument로 거부한다(ED24).
TEST(EventFieldGraph_EditorKeepsUnresolvedAndCookRejects) {
    SystemAllocator allocator;
    {
        Registries registries(allocator);
        ObjectRegistry& source = registries.source;
        const ObjectAccessContext access = source.OwnerAccess();
        RootRef<EventPublisher> publisher = New<EventPublisher>(source);
        RootRef<CallTarget> listener = NewListener(source, publisher.Handle());
        const Guid add = FunctionIdOf(registries.types, "Add");
        const Guid bump = FunctionIdOf(registries.types, "Bump");
        const Guid relay = FunctionIdOf(registries.types, "Relay");
        const Guid absent{0x1234, 0x5678};
        const Guid gone{0x1234, 0x5679};
        // pinged는 [Bump, Relay] 순서이고 앞의 Bump를 없는 함수로 바꾼다(순서 보존 확인).
        With<EventPublisher>(source, publisher.Handle(), [&](EventPublisher& value) noexcept {
            REQUIRE(value.changed.TryBind(source, access, listener.Handle(), add));
            REQUIRE(value.pinged.TryBind(source, access, listener.Handle(), bump));
            REQUIRE(value.pinged.TryBind(source, access, listener.Handle(), relay));
        });
        auto saved = SaveSet(allocator, source, publisher.Handle(), listener.Handle());
        REQUIRE(saved);
        auto missing = CopyBytes(allocator, saved.Value().View());
        REQUIRE(missing);
        REQUIRE(PatchGuid(missing.Value(), add, absent) == 1);
        REQUIRE(PatchGuid(missing.Value(), bump, gone) == 1);

        // 게임 로드(기본): 버리고 EventBindingDropped다.
        Collected gameDiagnostics;
        ObjectGraphLoadOptions game;
        game.hooks = &gameDiagnostics;
        auto dropped =
            LoadSet(allocator, registries.target, registries.types, missing.Value().View(), game);
        REQUIRE(dropped);
        CHECK(gameDiagnostics.Has(FieldDiagnosticKind::EventBindingDropped,
                                  EventBindingDroppedFunctionMissing));
        CHECK(!gameDiagnostics.Has(FieldDiagnosticKind::EventBindingUnresolved,
                                   EventBindingDroppedFunctionMissing));
        With<EventPublisher>(
            registries.target, dropped.Value().roots[0].Handle(),
            [](EventPublisher& value) noexcept {
                CHECK(value.changed.BindingCount() == 0 && value.changed.UnresolvedCount() == 0 &&
                      value.pinged.BindingCount() == 1 && value.pinged.UnresolvedCount() == 0);
            });

        // 에디터 로드: 보존하고 EventBindingUnresolved로 알린다(버림 진단은 없다). 보존한 레코드는
        // 발행되지 않는다.
        ObjectRegistry editor(allocator, registries.types, {0x45564701, 5});
        Collected diagnostics;
        ObjectGraphLoadOptions keep;
        keep.keepUnresolvedEventBindings = true;
        keep.hooks = &diagnostics;
        auto kept = LoadSet(allocator, editor, registries.types, missing.Value().View(), keep);
        REQUIRE(kept);
        CHECK(kept.Value().bound == 1);
        CHECK(diagnostics.Has(FieldDiagnosticKind::EventBindingUnresolved,
                              EventBindingDroppedFunctionMissing));
        CHECK(diagnostics.count == 2 && !diagnostics.dropped);
        const ObjectHandle copy = kept.Value().roots[0].Handle();
        const ObjectHandle copiedListener = kept.Value().roots[1].Handle();

        // 발행 전에 다시 저장하면 읽은 byte 그대로다(미해결 레코드가 같은 자리 — pinged의 앞 — 에
        // 쓰인다).
        auto resaved = SaveSet(allocator, editor, copy, copiedListener);
        REQUIRE(resaved);
        CHECK(test::graph::SameBytes(missing.Value().View(), resaved.Value().View()));

        With<EventPublisher>(editor, copy, [&](EventPublisher& value) noexcept {
            CHECK(value.changed.BindingCount() == 0 && value.changed.UnresolvedCount() == 1 &&
                  value.pinged.BindingCount() == 1 && value.pinged.UnresolvedCount() == 1);
            REQUIRE(
                value.changed.TryBroadcast(EventContext{editor, editor.OwnerAccess(), copy}, 3));
            // Relay만 불린다(1).
            REQUIRE(value.pinged.TryBroadcast(EventContext{editor, editor.OwnerAccess(), copy}));
        });
        CHECK(TotalOf(editor, copiedListener) == 1);

        // 쿠킹 저장은 미해결 레코드를 거부한다.
        Collected cookDiagnostics;
        ObjectGraphSaveOptions cook;
        cook.policy.excludedFlags = static_cast<uint32>(FieldFlags::EditorOnly);
        cook.hooks = &cookDiagnostics;
        auto rejected = SaveSet(allocator, editor, copy, copiedListener, cook);
        CHECK(!rejected && rejected.Error() == ErrorCode::InvalidArgument);
        CHECK(cookDiagnostics.Has(FieldDiagnosticKind::EventBindingNotCookable,
                                  EventBindingNotCookableUnresolved));

        // 편집자가 미해결 레코드를 지우면 쿠킹할 수 있고, 쿠킹 로드가 나머지를 다시 바인딩한다.
        With<EventPublisher>(editor, copy, [&](EventPublisher& value) noexcept {
            CHECK(value.changed.Unbind(copiedListener, absent));
            CHECK(value.pinged.Unbind(copiedListener, gone));
        });
        auto cooked = SaveSet(allocator, editor, copy, copiedListener, cook);
        REQUIRE(cooked);
        ObjectRegistry played(allocator, registries.types, {0x45564701, 6});
        ObjectGraphLoadOptions cookedLoad;
        cookedLoad.policy.excludedFlags = static_cast<uint32>(FieldFlags::EditorOnly);
        auto restored =
            LoadSet(allocator, played, registries.types, cooked.Value().View(), cookedLoad);
        REQUIRE(restored);
        CHECK(restored.Value().bound == 1);

        // 서명이 다른 함수(Add 자리에 Bump — 인자 없음)도 보존한다(진단 이유 2).
        auto mismatched = CopyBytes(allocator, saved.Value().View());
        REQUIRE(mismatched);
        REQUIRE(PatchGuid(mismatched.Value(), add, bump) == 1);
        ObjectRegistry third(allocator, registries.types, {0x45564701, 9});
        Collected mismatch;
        ObjectGraphLoadOptions keepMismatch;
        keepMismatch.keepUnresolvedEventBindings = true;
        keepMismatch.hooks = &mismatch;
        auto reloaded =
            LoadSet(allocator, third, registries.types, mismatched.Value().View(), keepMismatch);
        REQUIRE(reloaded);
        CHECK(mismatch.Has(FieldDiagnosticKind::EventBindingUnresolved,
                           EventBindingDroppedSignatureMismatch));
        With<EventPublisher>(
            third, reloaded.Value().roots[0].Handle(),
            [](EventPublisher& value) noexcept { CHECK(value.changed.UnresolvedCount() == 1); });
        auto mismatchResaved = SaveSet(allocator, third, reloaded.Value().roots[0].Handle(),
                                       reloaded.Value().roots[1].Handle());
        REQUIRE(mismatchResaved);
        CHECK(test::graph::SameBytes(mismatched.Value().View(), mismatchResaved.Value().View()));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

#if ROCKS_WITH_EDITOR_DATA
// EditorOnly 이벤트(edited)는 쿠킹 저장이 쓰지 않고, 그 기록이 있는 에디터 데이터를 쿠킹 로드하면
// CorruptData다(값 필드와 같다).
TEST(EventFieldGraph_EditorOnlyEventIsNotCooked) {
    SystemAllocator allocator;
    {
        Registries registries(allocator);
        ObjectRegistry& source = registries.source;
        const ObjectAccessContext access = source.OwnerAccess();
        RootRef<EventPublisher> publisher = New<EventPublisher>(source);
        RootRef<CallTarget> listener = NewListener(source, publisher.Handle());
        const Guid add = FunctionIdOf(registries.types, "Add");
        auto plainCooked = [&]() noexcept {
            ObjectGraphSaveOptions cook;
            cook.policy.excludedFlags = static_cast<uint32>(FieldFlags::EditorOnly);
            return SaveSet(allocator, source, publisher.Handle(), listener.Handle(), cook);
        };
        auto before = plainCooked();
        REQUIRE(before);
        With<EventPublisher>(source, publisher.Handle(), [&](EventPublisher& value) noexcept {
            REQUIRE(value.edited.TryBind(source, access, listener.Handle(), add));
        });

        // 쿠킹 저장은 EditorOnly 이벤트를 필드째 뺀다(바인딩이 있어도 byte가 같다).
        auto after = plainCooked();
        REQUIRE(after);
        CHECK(test::graph::SameBytes(before.Value().View(), after.Value().View()));

        // 에디터 저장은 쓰고, 에디터 로드는 다시 바인딩한다.
        auto editorBytes = SaveSet(allocator, source, publisher.Handle(), listener.Handle());
        REQUIRE(editorBytes);
        auto editorLoaded =
            LoadSet(allocator, registries.target, registries.types, editorBytes.Value().View());
        REQUIRE(editorLoaded);
        With<EventPublisher>(
            registries.target, editorLoaded.Value().roots[0].Handle(),
            [](EventPublisher& value) noexcept { CHECK(value.edited.BindingCount() == 1); });

        // 쿠킹 로드는 EditorOnly 이벤트 기록을 손상으로 거부한다.
        ObjectRegistry game(allocator, registries.types, {0x45564701, 7});
        ObjectGraphLoadOptions cookedLoad;
        cookedLoad.policy.excludedFlags = static_cast<uint32>(FieldFlags::EditorOnly);
        auto rejected =
            LoadSet(allocator, game, registries.types, editorBytes.Value().View(), cookedLoad);
        CHECK(!rejected && rejected.Error() == ErrorCode::CorruptData);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}
#endif

// 쿠킹 저장은 EditorOnly 함수를 가리키는 바인딩을 EventBindingNotCookable(1) 뒤 InvalidArgument로
// 거부한다 — 에디터 저장은 쓴다(ED24). 시험 빌드는 ROCKS_WITH_EDITOR가 꺼져 fixture의 EditorOnly
// 함수(Reset)가 없으므로, 생성 schema를 복사해 Add에 EditorOnly flag를 붙인 CallTarget을 따로 된
// TypeRegistry에 등록한다.
TEST(EventFieldGraph_CookRejectsEditorOnlyFunction) {
    SystemAllocator allocator;
    {
        // 등록한 schema 표가 TypeRegistry보다 오래 산다.
        Array<FunctionSchema> functions(allocator);
        TypeRegistry types(allocator);
        REQUIRE(GeneratedType<reflection_fixture::CallMode>::Register(types));
        REQUIRE(GeneratedType<reflection_fixture::CallPoint>::Register(types));
        const TypeSchema& generated = GeneratedType<CallTarget>::Schema();
        Guid add{};
        for (const FunctionSchema& function : generated.functions) {
            REQUIRE(functions.TryPushBack(function));
            if (StringView(function.name.Data(), function.name.Size()) == StringView("Add")) {
                functions[functions.Size() - 1].flags |=
                    static_cast<uint32>(FunctionFlags::EditorOnly);
                add = function.id;
            }
        }

        REQUIRE(add != Guid{});
        TypeSchema edited = generated;
        edited.functions = functions.View();
        auto target = types.Register(edited, GeneratedType<CallTarget>::Ops());
        REQUIRE(target);
        REQUIRE(GeneratedType<EventPublisher>::Register(types));

        ObjectRegistry source(allocator, types, {0x45564701, 8});
        const ObjectAccessContext access = source.OwnerAccess();
        RootRef<EventPublisher> publisher = New<EventPublisher>(source);
        RootRef<CallTarget> listener = NewListener(source, publisher.Handle());
        With<EventPublisher>(source, publisher.Handle(), [&](EventPublisher& value) noexcept {
            REQUIRE(value.changed.TryBind(source, access, listener.Handle(), add));
        });
        auto editorBytes = SaveSet(allocator, source, publisher.Handle(), listener.Handle());
        REQUIRE(editorBytes);

        Collected diagnostics;
        ObjectGraphSaveOptions cook;
        cook.policy.excludedFlags = static_cast<uint32>(FieldFlags::EditorOnly);
        cook.hooks = &diagnostics;
        auto rejected = SaveSet(allocator, source, publisher.Handle(), listener.Handle(), cook);
        CHECK(!rejected && rejected.Error() == ErrorCode::InvalidArgument);
        CHECK(diagnostics.Has(FieldDiagnosticKind::EventBindingNotCookable,
                              EventBindingNotCookableEditorOnly));

        // 바인딩을 지우면 쿠킹된다.
        With<EventPublisher>(source, publisher.Handle(),
                             [&](EventPublisher& value) noexcept { value.changed.Clear(); });
        CHECK(SaveSet(allocator, source, publisher.Handle(), listener.Handle(), cook));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 클래스 기본값 레코드가 만들어지고(이벤트 필드는 레코드에 없다), 바뀐 필드 판정은 값 필드만 본다.
// 속성 경로는 이벤트 필드 이름을 찾지 않는다.
TEST(EventFieldGraph_ClassDefaultsAndPathsSkipEvents) {
    SystemAllocator allocator;
    {
        Registries registries(allocator);
        ObjectRegistry& objects = registries.source;
        const ObjectAccessContext access = objects.OwnerAccess();
        auto lease = registries.types.Acquire(TypeIdOf<EventPublisher>());
        REQUIRE(lease);
        ClassDefaultsDiagnostic why;
        auto defaults = AcquireClassDefaults(lease.Value(), allocator, &why);
        REQUIRE(defaults);

        RootRef<EventPublisher> publisher = New<EventPublisher>(objects);
        RootRef<CallTarget> listener = New<CallTarget>(objects);
        With<EventPublisher>(objects, publisher.Handle(), [&](EventPublisher& value) noexcept {
            value.count = 3;
            REQUIRE(value.changed.TryBind(objects, access, listener.Handle(),
                                          FunctionIdOf(registries.types, "Add")));
        });
        Array<PropertyPathBuffer> changed(allocator);
        REQUIRE(FindChangedFields(objects, access, publisher.Handle(), allocator, changed));
        CHECK(changed.Size() == 1);

        PropertyPathBuffer path;
        CHECK(!ParsePropertyPath(objects, access, publisher.Handle(), "changed", path));
        CHECK(ParsePropertyPath(objects, access, publisher.Handle(), "count", path));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

} // namespace event_field_graph_tests_file
