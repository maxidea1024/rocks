#include <TestSupport/Test.h>
#include "GraphExternalTestSupport.h"

// graph v2 외부 참조 복원 시험이다(Docs/Design/ObjectFoundations.ko.md 5.4·5.5·5.7절, 9절 Oe "복원
// imports"): SCC 두 그래프가 서로 외부 참조(allocate-all 뒤 apply-all, transaction.FindObject로
// 해석), 1단계의 importTypes·값 범위 검사, 2b imports 확인(개수·생존·선언 타입·기록한 기대 형식 —
// 쓰기 전, 실패는 트랜잭션 고정), 편의 복원의 imports, 65536 경계.
using namespace rocks;
using namespace rocks::test::external;

namespace {

// number·link만 쓴 Item Record다(link는 저장 값 그대로 — 외부 비트 포함).
Array<uint8> LinkRecord(Allocator& allocator, uint32 number, uint32 link) noexcept {
    RecordBuilder record(allocator, ItemType, 1);
    auto add = [&](Guid id, uint32 value) noexcept {
        ArchiveWriter payload(allocator);
        if (!payload.WriteU32(value) || !record.AddField(id, WireType::U32, payload.View())) {
            platform::Fatal("External load test record failed");
        }
    };
    add(NumberId, number);
    add(LinkId, link);
    auto built = record.Build();
    if (!built) {
        platform::Fatal("External load test record build failed");
    }

    return Move(built.Value());
}

// NodeTable이 있는 노드 하나짜리 그래프다(header externalCount = externals).
Array<uint8> SingleGraph(Allocator& allocator, Span<const uint8> record,
                         uint32 externals) noexcept {
    const test::graph::Node node{ItemType, 0, {}, record};
    test::graph::Header header;
    header.externalCount = externals;
    auto built = test::graph::Build(allocator, Span<const test::graph::Node>(&node, 1), header);
    if (!built) {
        platform::Fatal("External load test graph failed");
    }

    return Move(built.Value());
}

ErrorCode ValidateOnly(Allocator& allocator, ObjectRegistry& registry, TypeRegistry& types,
                       Span<const uint8> bytes, Span<const TypeId> importTypes) noexcept {
    ObjectGraphStage stage(allocator, registry, types, registry.OwnerAccess());
    ObjectGraphInput input;
    input.bytes = bytes;
    input.importTypes = importTypes;
    return ErrorOf(stage.Validate(input));
}

// 2b까지 진행한 결과다: ApplyFields의 오류, 그때 노드 1의 staged number(0이면 아무 필드도 쓰지
// 않았다), 이어서 부른 Commit의 오류(고정된 트랜잭션이면 같은 오류), 실패 상태의 Stage에 다시 부른
// ApplyFields의 오류.
struct ApplyOutcome {
    ErrorCode apply = ErrorCode::DependencyFailed;
    uint32 number = 0;
    ErrorCode commit = ErrorCode::DependencyFailed;
    ErrorCode again = ErrorCode::DependencyFailed;
};

ApplyOutcome Apply(Allocator& allocator, ObjectRegistry& registry, TypeRegistry& types,
                   Span<const uint8> bytes, Span<const TypeId> importTypes,
                   Span<const ObjectHandle> imports) noexcept {
    ApplyOutcome outcome;
    ObjectGraphStage stage(allocator, registry, types, registry.OwnerAccess());
    ObjectGraphInput input;
    input.bytes = bytes;
    input.importTypes = importTypes;
    ConstructionContext transaction(registry, registry.OwnerAccess());
    if (!stage.Validate(input) || !stage.CreateObjects(transaction)) {
        return outcome;
    }

    outcome.apply = ErrorOf(stage.ApplyFields(transaction, imports));
    outcome.number = LookStaged(transaction, stage.Nodes()[0]).number;
    const ObjectHandle roots[] = {stage.Nodes()[0]};
    RootRef<Object> held[1];
    outcome.commit = ErrorOf(transaction.Commit(roots, held));
    outcome.again = ErrorOf(stage.ApplyFields(transaction, imports));
    return outcome;
}

} // namespace

// SCC: 두 그래프가 서로의 객체를 외부 참조로 가리킨다. 한 트랜잭션에 두 Stage의 CreateObjects를
// 먼저 하고(allocate-all), transaction.FindObject로 상대 그래프의 공개 전 객체를 찾아 imports로 준
// 뒤 두 Stage의 ApplyFields를 하고(apply-all), 다중 root Commit으로 함께 공개한다(단일 참조·배열
// 원소 외부 참조).
TEST(GraphExternalLoad_SccGraphsReferenceEachOtherInOneTransaction) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry source(allocator, types, {0x47454c, 1});
        ObjectRegistry target(allocator, types, {0x47454c, 2});
        {
            Array<RootRef<Object>> roots(allocator);
            auto pa = New(source, roots, ItemType, {}, "PA");
            auto pb = New(source, roots, ItemType, {}, "PB");
            REQUIRE(pa && pb);
            auto a1 = New(source, roots, ItemType, pa.Value(), "Body");
            auto b1 = New(source, roots, ItemType, pb.Value(), "Body");
            REQUIRE(a1 && b1);
            REQUIRE(SetLink(source, a1.Value(), b1.Value()));
            REQUIRE(AddLink(source, a1.Value(), b1.Value()));
            REQUIRE(SetLink(source, b1.Value(), a1.Value()));
            REQUIRE(SetNumber(source, a1.Value(), 11));
            REQUIRE(SetNumber(source, b1.Value(), 22));
            Admitter hooks;
            const ObjectHandle setA[] = {pa.Value(), a1.Value()};
            const ObjectHandle setB[] = {pb.Value(), b1.Value()};
            auto graphA = SaveObjectGraphSet(allocator, source, source.OwnerAccess(), setA,
                                             AdmitOptions(&hooks));
            auto graphB = SaveObjectGraphSet(allocator, source, source.OwnerAccess(), setB,
                                             AdmitOptions(&hooks));
            REQUIRE(graphA && graphB);
            REQUIRE(graphA.Value().Externals().Size() == 1 &&
                    graphA.Value().Externals()[0].target == b1.Value());
            REQUIRE(graphB.Value().Externals().Size() == 1 &&
                    graphB.Value().Externals()[0].target == a1.Value());

            const Array<TypeId> importTypes = ImportTypes(allocator, 1);
            ObjectGraphStage stageA(allocator, target, types, target.OwnerAccess());
            ObjectGraphStage stageB(allocator, target, types, target.OwnerAccess());
            ObjectGraphInput inputA;
            inputA.bytes = graphA.Value().Bytes();
            inputA.importTypes = importTypes.View();
            ObjectGraphInput inputB = inputA;
            inputB.bytes = graphB.Value().Bytes();
            REQUIRE(stageA.Validate(inputA));
            REQUIRE(stageB.Validate(inputB));
            CHECK(stageA.ExternalUses().Size() == 1 && stageA.ExternalUses()[0].occurrences == 2 &&
                  stageA.ExternalUses()[0].required);

            ConstructionContext transaction(target, target.OwnerAccess());
            REQUIRE(stageA.CreateObjects(transaction));
            REQUIRE(stageB.CreateObjects(transaction));
            auto bodyA = transaction.FindObject(stageA.Nodes()[0], "Body");
            auto bodyB = transaction.FindObject(stageB.Nodes()[0], "Body");
            REQUIRE(bodyA && bodyB);
            CHECK(bodyA.Value() == stageA.Nodes()[1] && bodyB.Value() == stageB.Nodes()[1]);
            const ObjectHandle importsA[] = {bodyB.Value()};
            const ObjectHandle importsB[] = {bodyA.Value()};
            REQUIRE(stageA.ApplyFields(transaction, importsA));
            REQUIRE(stageB.ApplyFields(transaction, importsB));
            REQUIRE(stageA.CheckRequired(transaction));
            REQUIRE(stageB.CheckRequired(transaction));
            const ObjectHandle packages[] = {stageA.Nodes()[0], stageB.Nodes()[0]};
            RootRef<Object> held[2];
            REQUIRE(transaction.Commit(packages, held));

            const Seen a = Look(target, bodyA.Value());
            const Seen b = Look(target, bodyB.Value());
            CHECK(a.number == 11 && a.link == bodyB.Value() && a.linkCount == 1 &&
                  a.links[0] == bodyB.Value());
            CHECK(b.number == 22 && b.link == bodyA.Value());
            auto found = target.FindObject(target.OwnerAccess(), held[1].Handle(), "Body");
            CHECK(found && found.Value() == bodyB.Value());
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 1단계: importTypes 수가 externalCount와 다르거나 0이 있으면 InvalidArgument, 미등록 NotFound,
// struct ID TypeMismatch, 기대 타입(Item)과 양립하지 않는 선언 타입(Other) TypeMismatch다. 선언
// 타입이 기대 타입의 native 기반(Mark)이면 통과한다(실제 대상은 2b가 판정). 외부 참조 값은 1 ≤ i ≤
// externalCount다(0x80000000·externalCount + 1 InvalidArgument). 모두 생성자 전이다.
TEST(GraphExternalLoad_ValidateChecksImportTypesAndValueRange) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry registry(allocator, types, {0x47454c, 3});
        {
            constexpr uint32 Bit = test::graph::ExternalBit;
            const Array<uint8> record = LinkRecord(allocator, 7, Bit | 2);
            const Array<uint8> graph = SingleGraph(allocator, record.View(), 2);
            const uint32 constructed = Item::Constructed;
            const TypeId items[] = {ItemType, ItemType};
            CHECK(ValidateOnly(allocator, registry, types, graph.View(), items) == ErrorCode::None);
            CHECK(ValidateOnly(allocator, registry, types, graph.View(), {}) ==
                  ErrorCode::InvalidArgument);
            const TypeId three[] = {ItemType, ItemType, ItemType};
            CHECK(ValidateOnly(allocator, registry, types, graph.View(), three) ==
                  ErrorCode::InvalidArgument);
            const TypeId zero[] = {ItemType, TypeId{}};
            CHECK(ValidateOnly(allocator, registry, types, graph.View(), zero) ==
                  ErrorCode::InvalidArgument);
            const TypeId unknown[] = {ItemType, TypeId{0x4745, 99}};
            CHECK(ValidateOnly(allocator, registry, types, graph.View(), unknown) ==
                  ErrorCode::NotFound);
            const TypeId structure[] = {ItemType, HoldType};
            CHECK(ValidateOnly(allocator, registry, types, graph.View(), structure) ==
                  ErrorCode::TypeMismatch);
            const TypeId other[] = {ItemType, OtherType};
            CHECK(ValidateOnly(allocator, registry, types, graph.View(), other) ==
                  ErrorCode::TypeMismatch);
            const TypeId base[] = {ItemType, MarkType};
            CHECK(ValidateOnly(allocator, registry, types, graph.View(), base) == ErrorCode::None);
            // 쓰이지 않는 import의 선언 타입도 등록 클래스여야 한다.
            const TypeId unusedUnknown[] = {TypeId{0x4745, 99}, ItemType};
            CHECK(ValidateOnly(allocator, registry, types, graph.View(), unusedUnknown) ==
                  ErrorCode::NotFound);

            const uint32 rejected[] = {Bit, Bit | 3, Bit | 0x7fffffffu};
            for (uint32 value : rejected) {
                const Array<uint8> bad = LinkRecord(allocator, 7, value);
                const Array<uint8> badGraph = SingleGraph(allocator, bad.View(), 2);
                CHECK(ValidateOnly(allocator, registry, types, badGraph.View(), items) ==
                      ErrorCode::InvalidArgument);
            }

            CHECK(Item::Constructed == constructed);
            CHECK(registry.Stats().liveObjects == 0 && registry.Stats().reservedObjects == 0);
        }

        REQUIRE(registry.Shutdown(registry.OwnerAccess()));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 2b imports 확인은 쓰기 전에 모두 한다: 개수 불일치 InvalidArgument, null·파괴 요청·다른
// 트랜잭션의 공개 전 객체 StaleHandle, 실제 타입이 선언 타입의 파생이 아님(Other) InvalidArgument,
// 선언 타입(Mark)은 맞지만 기록한 기대 형식(Item)을 만족하지 않음 TypeMismatch. 실패는 아무 필드도
// 쓰지 않고(staged number 0) 트랜잭션을 그 오류로 고정하며(Commit도 그 오류) Stage는 실패 상태다.
// 선언 타입이 기반(Mark)이어도 실제 대상이 Item이면, 같은 트랜잭션의 객체도 대상이 된다.
TEST(GraphExternalLoad_ImportChecksRunBeforeWrites) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry source(allocator, types, {0x47454c, 4});
        ObjectRegistry target(allocator, types, {0x47454c, 5});
        {
            Array<RootRef<Object>> roots(allocator);
            auto a = New(source, roots);
            auto x = New(source, roots);
            REQUIRE(a && x);
            REQUIRE(SetNumber(source, a.Value(), 7));
            REQUIRE(SetLink(source, a.Value(), x.Value()));
            REQUIRE(AddLink(source, a.Value(), x.Value()));
            Admitter hooks;
            const ObjectHandle set[] = {a.Value()};
            auto saved = SaveObjectGraphSet(allocator, source, source.OwnerAccess(), set,
                                            AdmitOptions(&hooks));
            REQUIRE(saved);
            const Span<const uint8> bytes = saved.Value().Bytes();

            Array<RootRef<Object>> live(allocator);
            auto item = New(target, live);
            auto mark = New(target, live, MarkType);
            auto other = New(target, live, OtherType);
            auto doomed = New(target, live);
            REQUIRE(item && mark && other && doomed);
            REQUIRE(target.RequestDestroy(target.OwnerAccess(), doomed.Value()));
            const Array<TypeId> itemTypes = ImportTypes(allocator, 1);
            const Array<TypeId> markTypes = ImportTypes(allocator, 1, MarkType);
            const uint64 liveBefore = target.Stats().liveObjects;

            struct Case {
                Span<const TypeId> importTypes;
                ObjectHandle import;
                bool empty;
                ErrorCode error;
            };

            const Case cases[] = {
                {itemTypes.View(), {}, true, ErrorCode::InvalidArgument},
                {itemTypes.View(), ObjectHandle{}, false, ErrorCode::StaleHandle},
                {itemTypes.View(), doomed.Value(), false, ErrorCode::StaleHandle},
                {itemTypes.View(), other.Value(), false, ErrorCode::InvalidArgument},
                {markTypes.View(), mark.Value(), false, ErrorCode::TypeMismatch},
                {markTypes.View(), other.Value(), false, ErrorCode::InvalidArgument}};
            for (const Case& example : cases) {
                const ObjectHandle one[] = {example.import};
                const ApplyOutcome outcome = Apply(
                    allocator, target, types, bytes, example.importTypes,
                    example.empty ? Span<const ObjectHandle>{} : Span<const ObjectHandle>(one));
                CHECK(outcome.apply == example.error);
                CHECK(outcome.number == 0);
                CHECK(outcome.commit == example.error);
                CHECK(outcome.again == ErrorCode::InvalidArgument);
                CHECK(target.Stats().liveObjects == liveBefore &&
                      target.Stats().reservedObjects == 0);
            }

            // 둘이면(개수 불일치) InvalidArgument다.
            const ObjectHandle two[] = {item.Value(), item.Value()};
            const ApplyOutcome extra =
                Apply(allocator, target, types, bytes, itemTypes.View(), two);
            CHECK(extra.apply == ErrorCode::InvalidArgument && extra.number == 0);

            // 다른 트랜잭션의 공개 전 객체는 StaleHandle이다.
            {
                ConstructionContext other(target, target.OwnerAccess());
                auto reserved = other.Create<Item>(ItemType);
                REQUIRE(reserved);
                const ObjectHandle foreign[] = {reserved.Value()};
                const ApplyOutcome outcome =
                    Apply(allocator, target, types, bytes, itemTypes.View(), foreign);
                CHECK(outcome.apply == ErrorCode::StaleHandle && outcome.number == 0);
                other.Rollback();
            }

            // 선언 타입이 기반(Mark)이어도 실제 대상이 Item이면 연결된다.
            const ObjectHandle real[] = {item.Value()};
            const ApplyOutcome based =
                Apply(allocator, target, types, bytes, markTypes.View(), real);
            CHECK(based.apply == ErrorCode::None && based.number == 7 &&
                  based.commit == ErrorCode::None);

            // 같은 트랜잭션에서 만든 객체도 대상이다(Commit이 함께 공개한다).
            {
                ObjectGraphStage stage(allocator, target, types, target.OwnerAccess());
                ObjectGraphInput input;
                input.bytes = bytes;
                input.importTypes = itemTypes.View();
                REQUIRE(stage.Validate(input));
                ConstructionContext transaction(target, target.OwnerAccess());
                REQUIRE(stage.CreateObjects(transaction));
                auto sibling = transaction.Create<Item>(ItemType);
                REQUIRE(sibling);
                const ObjectHandle imports[] = {sibling.Value()};
                REQUIRE(stage.ApplyFields(transaction, imports));
                const Seen staged = LookStaged(transaction, stage.Nodes()[0]);
                CHECK(staged.link == sibling.Value() && staged.links[0] == sibling.Value());
                const ObjectHandle published[] = {stage.Nodes()[0]};
                RootRef<Object> held[1];
                REQUIRE(transaction.Commit(published, held));
                CHECK(Look(target, held[0].Handle()).link == sibling.Value());
            }
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 편의 복원의 imports: 선언 타입은 대상의 실제 타입이다. 성공하면 대상과 잇고, 개수가
// externalCount와 다르면 생성자 전 InvalidArgument, null·파괴 요청 대상은 envelope 전에
// StaleHandle(단계형 2b와 같은 코드), 기대 형식을 만족하지 않는 대상(Mark)은 실제 타입으로 기록한
// 쌍을 판정해 생성자 전에 TypeMismatch다.
TEST(GraphExternalLoad_ConvenienceImports) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry source(allocator, types, {0x47454c, 6});
        ObjectRegistry target(allocator, types, {0x47454c, 7});
        {
            Array<RootRef<Object>> roots(allocator);
            auto a = New(source, roots);
            auto x = New(source, roots);
            REQUIRE(a && x);
            REQUIRE(SetLink(source, a.Value(), x.Value()));
            REQUIRE(SetHold(source, a.Value(), x.Value()));
            Admitter hooks;
            const ObjectHandle set[] = {a.Value()};
            auto saved = SaveObjectGraphSet(allocator, source, source.OwnerAccess(), set,
                                            AdmitOptions(&hooks));
            REQUIRE(saved);
            const Span<const uint8> bytes = saved.Value().Bytes();
            const auto access = target.OwnerAccess();

            Array<RootRef<Object>> live(allocator);
            auto item = New(target, live);
            auto mark = New(target, live, MarkType);
            auto doomed = New(target, live);
            REQUIRE(item && mark && doomed);
            {
                ObjectGraphLoadOptions options;
                const ObjectHandle imports[] = {item.Value()};
                options.imports = imports;
                auto loaded = LoadObjectGraph(allocator, target, types, access, bytes, options);
                REQUIRE(loaded);
                const Seen seen = Look(target, loaded.Value().Handle());
                CHECK(seen.link == item.Value() && seen.hold == item.Value());
            }

            REQUIRE(target.Collect(access));
            REQUIRE(target.RequestDestroy(access, doomed.Value()));
            const uint64 liveBefore = target.Stats().liveObjects;
            const uint32 constructed = Item::Constructed;
            CHECK(LoadObjectGraph(allocator, target, types, access, bytes).Error() ==
                  ErrorCode::InvalidArgument);
            ObjectGraphLoadOptions bad;
            const ObjectHandle stale[] = {ObjectHandle{}};
            bad.imports = stale;
            CHECK(LoadObjectGraph(allocator, target, types, access, bytes, bad).Error() ==
                  ErrorCode::StaleHandle);
            const ObjectHandle destroyed[] = {doomed.Value()};
            bad.imports = destroyed;
            CHECK(LoadObjectGraph(allocator, target, types, access, bytes, bad).Error() ==
                  ErrorCode::StaleHandle);
            const ObjectHandle wrong[] = {mark.Value()};
            bad.imports = wrong;
            CHECK(LoadObjectGraph(allocator, target, types, access, bytes, bad).Error() ==
                  ErrorCode::TypeMismatch);
            CHECK(Item::Constructed == constructed);
            CHECK(target.Stats().liveObjects == liveBefore && target.Stats().reservedObjects == 0);
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 65536 경계(maxExternalReferences 기본값 = AS object import 상한): 서로 다른 외부 대상 65536개는
// 저장·복원되고(ObjectRefArray 원소 — 쓰임은 import마다 1) 65537개면 저장이 CapacityExceeded다.
// 저장 Registry는 복원 전에 닫는다(같은 allocator의 생존 할당이 많을 때 회수가 느려지지 않게).
TEST(GraphExternalLoad_ExternalCountBoundary) {
    SystemAllocator allocator;
    {
        const usize Count = ObjectGraphLimits{}.maxExternalReferences;
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        Array<uint8> bytes(allocator);
        {
            ObjectRegistry source(allocator, types, {0x47454c, 8});
            {
                Array<RootRef<Object>> roots(allocator);
                REQUIRE(roots.TryReserve(Count + 2));
                auto owner = New(source, roots);
                REQUIRE(owner);
                for (usize index = 0; index < Count; ++index) {
                    auto made = New(source, roots);
                    REQUIRE(made);
                    REQUIRE(AddLink(source, owner.Value(), made.Value()));
                }

                Admitter hooks;
                const ObjectHandle set[] = {owner.Value()};
                auto saved = SaveObjectGraphSet(allocator, source, source.OwnerAccess(), set,
                                                AdmitOptions(&hooks));
                REQUIRE(saved);
                CHECK(hooks.calls == Count && saved.Value().Externals().Size() == Count);
                CHECK(test::graph::ReadU32(saved.Value().Bytes(),
                                           test::graph::ExternalCountOffset) == Count);
                bytes = saved.Value().TakeBytes();

                auto extra = New(source, roots);
                REQUIRE(extra);
                REQUIRE(AddLink(source, owner.Value(), extra.Value()));
                CHECK(SaveObjectGraphSet(allocator, source, source.OwnerAccess(), set,
                                         AdmitOptions(&hooks))
                          .Error() == ErrorCode::CapacityExceeded);
            }

            REQUIRE(source.Shutdown(source.OwnerAccess()));
        }

        ObjectRegistry target(allocator, types, {0x47454c, 9});
        {
            Array<RootRef<Object>> live(allocator);
            Array<ObjectHandle> imports(allocator);
            REQUIRE(live.TryReserve(Count) && imports.TryReserve(Count));
            for (usize index = 0; index < Count; ++index) {
                auto made = New(target, live);
                REQUIRE(made && imports.TryPushBack(made.Value()));
            }

            const Array<TypeId> importTypes = ImportTypes(allocator, Count);
            ObjectGraphStage stage(allocator, target, types, target.OwnerAccess());
            ObjectGraphInput input;
            input.bytes = bytes.View();
            input.importTypes = importTypes.View();
            REQUIRE(stage.Validate(input));
            REQUIRE(stage.ExternalUses().Size() == Count);
            CHECK(stage.ExternalUses()[0].occurrences == 1 &&
                  stage.ExternalUses()[Count - 1].occurrences == 1);
            ConstructionContext transaction(target, target.OwnerAccess());
            REQUIRE(stage.CreateObjects(transaction));
            REQUIRE(stage.ApplyFields(transaction, imports.View()));
            const ObjectHandle published[] = {stage.Nodes()[0]};
            RootRef<Object> held[1];
            REQUIRE(transaction.Commit(published, held));
            auto borrowed = target.Resolve<Item>(target.OwnerAccess(), held[0].Handle());
            REQUIRE(borrowed);
            const Item& loaded = *borrowed.Value().Get().Value();
            CHECK(loaded.links.Size() == Count && loaded.links[0] == imports[0] &&
                  loaded.links[Count - 1] == imports[Count - 1]);
        }

        REQUIRE(target.Shutdown(target.OwnerAccess()));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}