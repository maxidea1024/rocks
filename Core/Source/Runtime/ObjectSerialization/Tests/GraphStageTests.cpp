#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>
#include "GraphStageTestSupport.h"

// 단계형 복원(ObjectGraphStage) 시험이다(Docs/Design/ObjectFoundations.ko.md 5.4절, 9절 Od2): 노드
// 표(A1 모의 — GraphNodeDesc)·노드 출처, 노드 표 정체성 규칙의 생성 전 거부(형제 중복·대표 자식·
// 패키지·WorldOnly·경로), 2a 사전 확인(기준 Outer 요약 대조·실제 요약 재판정·이름 점유·정책 사본),
// 한 트랜잭션의 그래프 둘과 ConstructionContext::FindObject, 호출자 prepare(Transient 필드)와
// rollback, 할당 실패. 상태 기계·스레드·트랜잭션 고정은 GraphStageStateTests.cpp가 본다.
using namespace rocks;
using namespace rocks::test::stage;

// 노드 표 출처: NodeTable이 없는 그래프는 호출자 노드 설명(A1 Exports 모의)으로 복원한다.
// NodeTable과 노드 설명이 둘 다 있거나 둘 다 없음, 개수 불일치, 타입 0, BaseOuter 비트와 hasBase
// 불일치, externalCount ≠ 0은 InvalidArgument이고, 노드 타입이 Record schema와 다르면 생성자 전에
// InvalidArgument다. StoredVersion은 Validate 뒤 Record 버전이다.
TEST(GraphStage_NodeDescriptorsAndInputSource) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry registry(allocator, types, {0x475331a, 1});
        {
            const Array<uint8> root = MakeRecord(allocator, NodeType, 1, 2);
            const Array<uint8> child = MakeRecord(allocator, NodeType, 2, 1);
            const test::graph::Node nodes[] = {{NodeType, 0, {}, root.View()},
                                               {NodeType, 1, "Child", child.View()}};
            GraphNodeDesc descs[2];
            Describe(nodes, descs);
            const Array<uint8> bare = MakeGraph(allocator, nodes, false);
            const Array<uint8> table = MakeGraph(allocator, nodes, true);
            ObjectGraphInput input;
            input.bytes = bare.View();
            input.nodes = descs;
            {
                ObjectGraphStage stage(allocator, registry, types, registry.OwnerAccess());
                CHECK(stage.StoredVersion(0).Error() == ErrorCode::InvalidArgument);
                REQUIRE(stage.Validate(input));
                CHECK(stage.StoredVersion(0).IsOk() && stage.StoredVersion(0).Value() == 1);
                CHECK(stage.StoredVersion(2).Error() == ErrorCode::InvalidArgument);
                CHECK(stage.Nodes().Size() == 0);
                ConstructionContext transaction(registry, registry.OwnerAccess());
                REQUIRE(stage.CreateObjects(transaction));
                REQUIRE(stage.Nodes().Size() == 2);
                REQUIRE(stage.ApplyFields(transaction));
                REQUIRE(stage.CheckRequired(transaction));
                const ObjectHandle roots[] = {stage.Nodes()[0]};
                RootRef<Object> held[1];
                REQUIRE(transaction.Commit(roots, held));
                const Seen first = Look(registry, held[0].Handle());
                CHECK(first.number == 1 && first.link == stage.Nodes()[1]);
                CHECK(Look(registry, first.link).link == held[0].Handle());
                auto outer = registry.GetOuter(registry.OwnerAccess(), first.link);
                CHECK(outer && outer.Value() == held[0].Handle());
                CHECK(NameIs(registry, first.link, "Child", allocator));
            }

            REQUIRE(registry.Collect(registry.OwnerAccess()));
            CHECK(registry.Stats().liveObjects == 0);
            // 노드 출처 위반은 모두 InvalidArgument다.
            ObjectGraphInput both = input;
            both.bytes = table.View();
            CHECK(ValidateOnly(allocator, registry, types, both) == ErrorCode::InvalidArgument);
            ObjectGraphInput neither;
            neither.bytes = bare.View();
            CHECK(ValidateOnly(allocator, registry, types, neither) == ErrorCode::InvalidArgument);
            ObjectGraphInput fewer = input;
            fewer.nodes = Span<const GraphNodeDesc>(descs, 1);
            CHECK(ValidateOnly(allocator, registry, types, fewer) == ErrorCode::InvalidArgument);
            GraphNodeDesc zero[2] = {descs[0], descs[1]};
            zero[1].type = TypeId{};
            ObjectGraphInput untyped = input;
            untyped.nodes = zero;
            CHECK(ValidateOnly(allocator, registry, types, untyped) == ErrorCode::InvalidArgument);
            ObjectGraphInput tableInput;
            tableInput.bytes = table.View();
            tableInput.hasBase = true;
            CHECK(ValidateOnly(allocator, registry, types, tableInput) ==
                  ErrorCode::InvalidArgument);
            const Array<uint8> based = MakeGraph(allocator, nodes, true, true);
            tableInput.bytes = based.View();
            tableInput.hasBase = false;
            CHECK(ValidateOnly(allocator, registry, types, tableInput) ==
                  ErrorCode::InvalidArgument);
            Array<uint8> external(allocator);
            REQUIRE(external.TryResize(bare.Size()));
            for (usize index = 0; index < bare.Size(); ++index) {
                external[index] = bare[index];
            }

            test::graph::PatchU32(external, test::graph::ExternalCountOffset, 1);
            ObjectGraphInput imported = input;
            imported.bytes = external.View();
            CHECK(ValidateOnly(allocator, registry, types, imported) == ErrorCode::InvalidArgument);
            // 노드 설명의 타입이 Record schema와 다르면 생성자 전에 InvalidArgument다.
            GraphNodeDesc mismatched[2] = {descs[0], descs[1]};
            mismatched[1].type = WorldType;
            ObjectGraphInput wrongType = input;
            wrongType.nodes = mismatched;
            const uint32 constructed = ConstructedNodes;
            CHECK(RunStage(allocator, registry, types, wrongType) == ErrorCode::InvalidArgument);
            CHECK(ConstructedNodes == constructed);
            CHECK(registry.Stats().liveObjects == 0 && registry.Stats().reservedObjects == 0);
            // 기준 Outer 요약의 pathBytes가 4 KiB를 넘으면 노드 출처 단계의
            // InvalidArgument다(uint32 최대값 근처에서도 경로 길이 덧셈이 넘쳐 통과하지 않는다).
            // 정확히 4 KiB면 이름 있는 노드가 경로 규칙(CapacityExceeded)에 걸린다.
            const test::graph::Node baseNode[] = {{NodeType, 0, "n", root.View()}};
            GraphNodeDesc baseDesc[1];
            Describe(baseNode, baseDesc);
            const Array<uint8> baseGraph = MakeGraph(allocator, baseNode, false);
            ObjectGraphInput huge;
            huge.bytes = baseGraph.View();
            huge.nodes = baseDesc;
            huge.hasBase = true;
            huge.base.isOutermost = false;
            const uint32 hugePaths[] = {NumericLimits<uint32>::Max(),
                                        NumericLimits<uint32>::Max() - 15,
                                        static_cast<uint32>(MaxObjectPathBytes) + 1};
            for (uint32 pathBytes : hugePaths) {
                huge.base.pathBytes = pathBytes;
                CHECK(ValidateOnly(allocator, registry, types, huge) == ErrorCode::InvalidArgument);
            }

            huge.base.pathBytes = static_cast<uint32>(MaxObjectPathBytes);
            CHECK(ValidateOnly(allocator, registry, types, huge) == ErrorCode::CapacityExceeded);
            // StoredVersion은 Record의 저장 버전이다(현재 스키마 1보다 높은 2를
            // LoadNewerSkippingUnknown으로 읽는다).
            const Array<uint8> newer = MakeRecord(allocator, NodeType, 4, 0, false, 2);
            const test::graph::Node newerNode[] = {{NodeType, 0, {}, newer.View()}};
            const Array<uint8> newerGraph = MakeGraph(allocator, newerNode, true);
            ThreadDiagnostics diagnostics;
            ObjectGraphLoadOptions newerOptions;
            newerOptions.hooks = &diagnostics;
            newerOptions.policy.newerVersion = NewerVersionPolicy::LoadNewerSkippingUnknown;
            ObjectGraphStage newerStage(allocator, registry, types, registry.OwnerAccess(),
                                        newerOptions);
            ObjectGraphInput newerInput;
            newerInput.bytes = newerGraph.View();
            REQUIRE(newerStage.Validate(newerInput));
            CHECK(newerStage.StoredVersion(0).IsOk() && newerStage.StoredVersion(0).Value() == 2);
            CHECK(diagnostics.count == 1 &&
                  diagnostics.kinds[0] == FieldDiagnosticKind::NewerVersion);
        }

        REQUIRE(registry.Shutdown(registry.OwnerAccess()));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 노드 표의 이름·대표 자식·경로·패키지·WorldOnly 규칙과 그래프 안 형제 중복은 1단계가 생성자 전에
// 거부한다(형제 중복은 파일 손상이라 InvalidArgument — Od1의 Create AlreadyExists를 바꿨다). 편의
// 복원도 같다.
TEST(GraphStage_NodeTableRulesRejectBeforeConstructors) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry plain(allocator, types, {0x475332a, 1});
        ObjectRegistry packaged(allocator, types, {0x475332a, 2}, PackagePolicy());
        {
            const Array<uint8> node = MakeRecord(allocator);
            const Array<uint8> pack = MakeRecord(allocator, PackType);
            const Array<uint8> world = MakeRecord(allocator, WorldType);
            uint32 constructed = ConstructedNodes;
            struct Case {
                test::graph::Node nodes[3];
                usize count;
                ErrorCode expected;
            };
            const Case plainCases[] = {
                // 같은 Outer의 대소문자만 다른 이름(형제 중복).
                {{{NodeType, 0, {}, node.View()},
                  {NodeType, 1, "Body", node.View()},
                  {NodeType, 1, "BODY", node.View()}},
                 3,
                 ErrorCode::InvalidArgument},
                // 대표 자식 둘(빈 비교 키 중복).
                {{{NodeType, 0, {}, node.View()},
                  {NodeType, 1, {}, node.View()},
                  {NodeType, 1, {}, node.View()}},
                 3,
                 ErrorCode::InvalidArgument},
                // 대표 자식이 있는 Outer의 이름 있는 자식과 그 반대.
                {{{NodeType, 0, {}, node.View()},
                  {NodeType, 1, {}, node.View()},
                  {NodeType, 1, "A", node.View()}},
                 3,
                 ErrorCode::InvalidArgument},
                {{{NodeType, 0, {}, node.View()},
                  {NodeType, 1, "A", node.View()},
                  {NodeType, 1, {}, node.View()}},
                 3,
                 ErrorCode::InvalidArgument},
                // outermost가 아닌 Outer의 빈 이름.
                {{{NodeType, 0, {}, node.View()},
                  {NodeType, 1, "A", node.View()},
                  {NodeType, 2, {}, node.View()}},
                 3,
                 ErrorCode::InvalidArgument},
                // 이름 형식.
                {{{NodeType, 0, {}, node.View()}, {NodeType, 1, "a.b", node.View()}},
                 2,
                 ErrorCode::InvalidArgument},
                // 최상위 이름 공간의 형제 중복.
                {{{NodeType, 0, "Top", node.View()}, {NodeType, 0, "tOP", node.View()}},
                 2,
                 ErrorCode::InvalidArgument}};
            for (const Case& entry : plainCases) {
                const Span<const test::graph::Node> nodes(entry.nodes, entry.count);
                const Array<uint8> graph = MakeGraph(allocator, nodes, true);
                ObjectGraphInput input;
                input.bytes = graph.View();
                CHECK(RunStage(allocator, plain, types, input) == entry.expected);
            }

            // 편의 복원도 같다: 노드 1 → 2 → 3으로 모두 닿는(도달 검사를 통과하는) 형제 중복
            // NodeTable은 생성자 전에 InvalidArgument다(Od1은 Create의 AlreadyExists였다).
            {
                const Array<uint8> first = MakeRecord(allocator, NodeType, 0, 2);
                const Array<uint8> second = MakeRecord(allocator, NodeType, 0, 3);
                const test::graph::Node nodes[] = {{NodeType, 0, {}, first.View()},
                                                   {NodeType, 1, "Body", second.View()},
                                                   {NodeType, 1, "BODY", node.View()}};
                const Array<uint8> graph = MakeGraph(allocator, nodes, true);
                CHECK(LoadObjectGraph(allocator, plain, types, plain.OwnerAccess(), graph.View())
                          .Error() == ErrorCode::InvalidArgument);
                const test::graph::Node distinct[] = {{NodeType, 0, {}, first.View()},
                                                      {NodeType, 1, "Body", second.View()},
                                                      {NodeType, 1, "Arm", node.View()}};
                const Array<uint8> positive = MakeGraph(allocator, distinct, true);
                const uint32 before = ConstructedNodes;
                {
                    auto loaded = LoadObjectGraph(allocator, plain, types, plain.OwnerAccess(),
                                                  positive.View());
                    REQUIRE(loaded);
                }

                CHECK(ConstructedNodes == before + 3);
                REQUIRE(plain.Collect(plain.OwnerAccess()));
                constructed += 3;
            }

            // 다른 Outer의 같은 이름은 허용한다(양성 대조).
            {
                const test::graph::Node nodes[] = {{NodeType, 0, {}, node.View()},
                                                   {NodeType, 1, "A", node.View()},
                                                   {NodeType, 2, "A", node.View()}};
                const Array<uint8> graph = MakeGraph(allocator, nodes, true);
                ObjectGraphInput input;
                input.bytes = graph.View();
                ObjectGraphStage stage(allocator, plain, types, plain.OwnerAccess());
                CHECK(stage.Validate(input));
            }

            // 패키지 정책: WorldOnly Unsupported, 기준 Outer 없는 비패키지·이름 있는 패키지·패키지
            // 아래 패키지 InvalidArgument. 앞에 유효한 패키지 노드를 두어 1단계 거부임을 생성자
            // 수로 구별한다(1단계가 없으면 노드 1이 생성된다).
            const Case packageCases[] = {
                {{{PackType, 0, {}, pack.View()}, {WorldType, 1, {}, world.View()}},
                 2,
                 ErrorCode::Unsupported},
                {{{PackType, 0, {}, pack.View()}, {NodeType, 0, {}, node.View()}},
                 2,
                 ErrorCode::InvalidArgument},
                {{{PackType, 0, {}, pack.View()}, {PackType, 0, "Named", pack.View()}},
                 2,
                 ErrorCode::InvalidArgument},
                {{{PackType, 0, {}, pack.View()}, {PackType, 1, {}, pack.View()}},
                 2,
                 ErrorCode::InvalidArgument}};
            for (const Case& entry : packageCases) {
                const Span<const test::graph::Node> nodes(entry.nodes, entry.count);
                const Array<uint8> graph = MakeGraph(allocator, nodes, true);
                ObjectGraphInput input;
                input.bytes = graph.View();
                CHECK(RunStage(allocator, packaged, types, input) == entry.expected);
            }

            // 노드 설명(A1 모의 — NodeTable 없는 그래프)도 같은 1단계 규칙이다: 형제 중복·정책
            // 위반.
            auto runDescribed = [&](ObjectRegistry& registry, const Case& entry) noexcept {
                GraphNodeDesc descs[3];
                for (usize index = 0; index < entry.count; ++index) {
                    descs[index] = {entry.nodes[index].type, entry.nodes[index].outer,
                                    entry.nodes[index].name};
                }

                const Array<uint8> graph = MakeGraph(
                    allocator, Span<const test::graph::Node>(entry.nodes, entry.count), false);
                ObjectGraphInput input;
                input.bytes = graph.View();
                input.nodes = Span<const GraphNodeDesc>(descs, entry.count);
                return RunStage(allocator, registry, types, input);
            };
            CHECK(runDescribed(plain, plainCases[0]) == ErrorCode::InvalidArgument);
            for (const Case& entry : packageCases) {
                CHECK(runDescribed(packaged, entry) == entry.expected);
            }

            // 기준 Outer 형제의 누적: outer 0 대표 노드와 이름 있는 노드를 섞으면(두 순서) 1단계가
            // 거부한다.
            Array<RootRef<Object>> roots(allocator);
            auto base = New(plain, roots);
            REQUIRE(base);
            ++constructed; // 기준 Outer 객체
            const test::graph::Node mixed[][2] = {
                {{NodeType, 0, {}, node.View()}, {NodeType, 0, "A", node.View()}},
                {{NodeType, 0, "A", node.View()}, {NodeType, 0, {}, node.View()}}};
            for (const auto& order : mixed) {
                const Array<uint8> graph = MakeGraph(allocator, order, true, true);
                ObjectGraphInput input;
                input.bytes = graph.View();
                input.hasBase = true;
                input.base = OuterSummary::ForNewOutermost(false);
                CHECK(RunStage(allocator, plain, types, input, base.Value()) ==
                      ErrorCode::InvalidArgument);
            }

            CHECK(ConstructedNodes == constructed);
            CHECK(plain.Stats().reservedObjects == 0 && plain.Stats().liveObjects == 1);
            CHECK(packaged.Stats().liveObjects == 0 && packaged.Stats().reservedObjects == 0);

            // 객체 경로 4 KiB 정확한 경계: 64 byte 이름 63단계(4094 byte) 아래 1 byte 이름은
            // 4096(성공), 2 byte 이름은 4097(1단계 CapacityExceeded). 기준 Outer 요약의 pathBytes를
            // 거치는 경로도 같다(4094 + '.' + 이름).
            char longName[64];
            for (char& character : longName) {
                character = 'n';
            }

            Array<test::graph::Node> chain(allocator);
            REQUIRE(chain.TryPushBack(test::graph::Node{NodeType, 0, {}, node.View()}));
            for (uint32 depth = 1; depth <= 63; ++depth) {
                REQUIRE(chain.TryPushBack(
                    test::graph::Node{NodeType, depth, StringView(longName, 64), node.View()}));
            }

            REQUIRE(chain.TryPushBack(test::graph::Node{NodeType, 64, "ab", node.View()}));
            const Array<uint8> over = MakeGraph(allocator, chain.View(), true);
            ObjectGraphInput deepInput;
            deepInput.bytes = over.View();
            CHECK(ValidateOnly(allocator, plain, types, deepInput) == ErrorCode::CapacityExceeded);
            chain[64].name = "a";
            const Array<uint8> exact = MakeGraph(allocator, chain.View(), true);
            deepInput.bytes = exact.View();
            RootRef<Object> held;
            REQUIRE(RunStage(allocator, plain, types, deepInput, {}, {}, &held) == ErrorCode::None);
            CHECK(plain.Stats().liveObjects == 66);
            const test::graph::Node baseNames[][1] = {{{NodeType, 0, "a", node.View()}},
                                                      {{NodeType, 0, "ab", node.View()}}};
            for (usize index = 0; index < 2; ++index) {
                const Array<uint8> graph = MakeGraph(allocator, baseNames[index], true, true);
                ObjectGraphInput input;
                input.bytes = graph.View();
                input.hasBase = true;
                input.base.pathBytes = static_cast<uint32>(MaxObjectPathBytes) - 2;
                CHECK(ValidateOnly(allocator, plain, types, input) ==
                      (index == 0 ? ErrorCode::None : ErrorCode::CapacityExceeded));
            }

            held.Reset();
            for (RootRef<Object>& root : roots) {
                root.Reset();
            }

            REQUIRE(plain.Collect(plain.OwnerAccess()));
        }

        REQUIRE(plain.Shutdown(plain.OwnerAccess()));
        REQUIRE(packaged.Shutdown(packaged.OwnerAccess()));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 2a 사전 확인: 기준 Outer 요약(isOutermost·outermostIsPackage·pathBytes) 불일치 InvalidArgument,
// 실제 Inner 상태로 대표 자식 규칙 재판정(InvalidArgument), 이름 점유(기준 Outer 아래·대표 자식·
// 다른 트랜잭션의 Reserved·최상위 이름 공간 — AlreadyExists), baseOuter 유무 불일치, 다른
// Registry의 트랜잭션(정책이 다른 Registry·같은 Registry 모두) WrongContext. 사전 확인 실패는
// 아무것도 만들지 않고 트랜잭션을 고정하지 않는다 — 그래서 실패 뒤에도 같은 트랜잭션에서 Create가
// 된다(사전 확인이 없으면 Create 실패가 트랜잭션을 고정하므로 이것으로 구별한다). 이름 점유는 앞에
// 유효한 노드를 둔 2노드 그래프로도 본다(사전 확인이 없으면 노드 1이 생성된다).
TEST(GraphStage_CreatePreconditionsCheckRegistryState) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry registry(allocator, types, {0x475333a, 1});
        ObjectRegistry other(allocator, types, {0x475333a, 2}, PackagePolicy());
        ObjectRegistry twin(allocator, types, {0x475333a, 3});
        {
            Array<RootRef<Object>> roots(allocator);
            const Array<uint8> node = MakeRecord(allocator);
            const test::graph::Node representative[] = {{NodeType, 0, {}, node.View()}};
            const test::graph::Node named[] = {{NodeType, 0, "Body", node.View()}};
            const test::graph::Node pair[] = {{NodeType, 0, "Ok", node.View()},
                                              {NodeType, 0, "Body", node.View()}};
            GraphNodeDesc representativeDesc[1];
            GraphNodeDesc namedDesc[1];
            GraphNodeDesc pairDesc[2];
            Describe(representative, representativeDesc);
            Describe(named, namedDesc);
            Describe(pair, pairDesc);
            const Array<uint8> representativeGraph = MakeGraph(allocator, representative, false);
            const Array<uint8> namedGraph = MakeGraph(allocator, named, false);
            const Array<uint8> pairGraph = MakeGraph(allocator, pair, false);
            enum class Shape : uint8 {
                Representative,
                Named,
                Pair
            };
            auto input = [&](Shape shape) noexcept {
                ObjectGraphInput value;
                switch (shape) {
                case Shape::Representative:
                    value.bytes = representativeGraph.View();
                    value.nodes = representativeDesc;
                    break;
                case Shape::Named:
                    value.bytes = namedGraph.View();
                    value.nodes = namedDesc;
                    break;
                case Shape::Pair:
                    value.bytes = pairGraph.View();
                    value.nodes = pairDesc;
                    break;
                }

                value.hasBase = true;
                value.base = OuterSummary::ForNewOutermost(false);
                return value;
            };
            // CreateObjects까지 진행해 그 결과를 돌려준다. 실패하면 생성자 수가 그대로이고
            // Nodes()가 비며 트랜잭션이 고정되지 않아(CheckOpen 성공) 같은 트랜잭션에서 Create가
            // 되어야 한다 (아니면 DependencyFailed).
            auto create = [&](const ObjectGraphInput& value, ObjectHandle baseOuter,
                              ConstructionContext* shared = nullptr) noexcept -> ErrorCode {
                ObjectGraphStage stage(allocator, registry, types, registry.OwnerAccess());
                if (auto valid = stage.Validate(value); !valid) {
                    return valid.Error();
                }

                const uint32 constructed = ConstructedNodes;
                ConstructionContext local(registry, registry.OwnerAccess());
                ConstructionContext& transaction = shared != nullptr ? *shared : local;
                auto created = stage.CreateObjects(transaction, baseOuter);
                if (created) {
                    return ErrorCode::None;
                }

                if (ConstructedNodes != constructed || stage.Nodes().Size() != 0 ||
                    !transaction.CheckOpen() || !transaction.Create<Object>(NodeType)) {
                    return ErrorCode::DependencyFailed;
                }

                return created.Error();
            };

            auto empty = New(registry, roots);
            REQUIRE(empty);
            // 양성 대조: 빈 outermost 아래 대표 노드·이름 있는 노드·이름 있는 노드 둘.
            CHECK(create(input(Shape::Representative), empty.Value()) == ErrorCode::None);
            CHECK(create(input(Shape::Named), empty.Value()) == ErrorCode::None);
            CHECK(create(input(Shape::Pair), empty.Value()) == ErrorCode::None);
            CHECK(registry.Stats().reservedObjects == 0);

            // 요약 불일치: outermost가 아닌 기준 Outer(isOutermost·pathBytes), 경로 길이, 패키지
            // 여부, 대표 자식인 기준 Outer(isOutermost만 다름 — pathBytes는 둘 다 0).
            auto middle = New(registry, roots, empty.Value(), "Mid");
            REQUIRE(middle);
            CHECK(create(input(Shape::Named), middle.Value()) == ErrorCode::InvalidArgument);
            ObjectGraphInput longer = input(Shape::Named);
            longer.base.pathBytes = 3;
            CHECK(create(longer, empty.Value()) == ErrorCode::InvalidArgument);
            ObjectGraphInput packaged = input(Shape::Named);
            packaged.base.outermostIsPackage = true;
            CHECK(create(packaged, empty.Value()) == ErrorCode::InvalidArgument);
            ObjectGraphInput truthful = input(Shape::Named);
            truthful.base.isOutermost = false;
            truthful.base.pathBytes = 3;
            CHECK(create(truthful, middle.Value()) == ErrorCode::None);
            auto solo = New(registry, roots);
            REQUIRE(solo);
            auto soloAsset = New(registry, roots, solo.Value());
            REQUIRE(soloAsset);
            CHECK(create(input(Shape::Named), soloAsset.Value()) == ErrorCode::InvalidArgument);
            ObjectGraphInput underAsset = input(Shape::Named);
            underAsset.base.isOutermost = false;
            CHECK(create(underAsset, soloAsset.Value()) == ErrorCode::None);

            // 실제 Inner 상태로 재판정: 이름 있는 Inner가 있는 기준 Outer의 대표 노드, 대표 자식이
            // 있는 기준 Outer의 이름 있는 노드는 InvalidArgument다(1단계 요약에는 Inner가 없었다).
            CHECK(create(input(Shape::Representative), empty.Value()) ==
                  ErrorCode::InvalidArgument);
            auto holder = New(registry, roots);
            REQUIRE(holder);
            REQUIRE(New(registry, roots, holder.Value()));
            CHECK(create(input(Shape::Named), holder.Value()) == ErrorCode::InvalidArgument);
            // 점유: 이미 대표 자식이 있으면 대표 노드는 AlreadyExists(빈 비교 키), 같은
            // 이름(대소문자 무시)도 AlreadyExists다(2노드 그래프의 둘째 노드 포함).
            CHECK(create(input(Shape::Representative), holder.Value()) == ErrorCode::AlreadyExists);
            auto owner = New(registry, roots);
            REQUIRE(owner);
            REQUIRE(New(registry, roots, owner.Value(), "BODY"));
            CHECK(create(input(Shape::Named), owner.Value()) == ErrorCode::AlreadyExists);
            CHECK(create(input(Shape::Pair), owner.Value()) == ErrorCode::AlreadyExists);
            // 다른 트랜잭션의 공개 전 객체도 이름을 점유한다.
            auto fresh = New(registry, roots);
            REQUIRE(fresh);
            {
                ConstructionContext pending(registry, registry.OwnerAccess());
                REQUIRE(pending.Create<Object>(NodeType, fresh.Value(), "body"));
                CHECK(create(input(Shape::Pair), fresh.Value()) == ErrorCode::AlreadyExists);
            }

            CHECK(create(input(Shape::Pair), fresh.Value()) == ErrorCode::None);
            // 기준 Outer가 없으면 outer 0 이름 있는 노드의 최상위 이름 공간을 본다.
            REQUIRE(New(registry, roots, {}, "Body"));
            ObjectGraphInput top = input(Shape::Pair);
            top.hasBase = false;
            top.base = {};
            CHECK(create(top, {}) == ErrorCode::AlreadyExists);
            // baseOuter 유무가 hasBase와 다르면 InvalidArgument, 생존 객체·이 트랜잭션 객체가
            // 아니면 StaleHandle이다.
            CHECK(create(top, empty.Value()) == ErrorCode::InvalidArgument);
            CHECK(create(input(Shape::Named), {}) == ErrorCode::InvalidArgument);
            {
                ConstructionContext pending(registry, registry.OwnerAccess());
                auto reserved = pending.Create<Object>(NodeType);
                REQUIRE(reserved);
                CHECK(create(input(Shape::Named), reserved.Value()) == ErrorCode::StaleHandle);
                // 같은 트랜잭션의 공개 전 객체는 기준 Outer가 될 수 있고, 같은 트랜잭션에서
                // 실패한 사전 확인은 그 트랜잭션을 고정하지 않는다.
                CHECK(create(input(Shape::Representative), reserved.Value(), &pending) ==
                      ErrorCode::None);
                CHECK(create(input(Shape::Representative), reserved.Value(), &pending) ==
                      ErrorCode::AlreadyExists);
                CHECK(pending.CheckOpen().IsOk());
            }

            CHECK(registry.Stats().reservedObjects == 0);
            // 다른 Registry의 트랜잭션은 WrongContext다(정책이 다른 Registry, 정책이 같은 Registry
            // 모두).
            for (ObjectRegistry* foreign : {&other, &twin}) {
                ObjectGraphStage stage(allocator, registry, types, registry.OwnerAccess());
                REQUIRE(stage.Validate(top));
                ConstructionContext transaction(*foreign, foreign->OwnerAccess());
                CHECK(stage.CreateObjects(transaction).Error() == ErrorCode::WrongContext);
            }
        }

        REQUIRE(registry.Shutdown(registry.OwnerAccess()));
        REQUIRE(other.Shutdown(other.OwnerAccess()));
        REQUIRE(twin.Shutdown(twin.OwnerAccess()));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// SCC 로드 모의: 패키지 둘을 한 트랜잭션에 만들고 그래프 둘을 각 패키지 아래 올린 뒤(allocate-all),
// ConstructionContext::FindObject로 같은 트랜잭션의 공개 전 객체와 생존 객체를 찾고(다른 트랜잭션의
// 공개 전 객체는 보이지 않는다), 적용·Required 검사 뒤 호출자가 Transient 필드를 쓰고(prepare) 두
// 패키지를 다중 root로 함께 Commit한다. 그래프 사이 참조는 A0-Oe다. prepare 실패 시 Rollback하면
// 아무것도 공개되지 않는다.
TEST(GraphStage_TwoGraphsShareOneTransactionAndFindObject) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry registry(allocator, types, {0x475336a, 1}, PackagePolicy());
        {
            Array<RootRef<Object>> roots(allocator);
            // 이미 생존한 패키지 L(대표 자식 아래 Shared).
            auto live = New(registry, roots, {}, {}, PackType);
            REQUIRE(live);
            auto liveAsset = New(registry, roots, live.Value());
            REQUIRE(liveAsset);
            auto shared = New(registry, roots, liveAsset.Value(), "Shared");
            REQUIRE(shared);

            const Array<uint8> asset = MakeRecord(allocator, NodeType, 1, 2);
            const Array<uint8> part = MakeRecord(allocator, NodeType, 2, 1);
            const test::graph::Node first[] = {{NodeType, 0, {}, asset.View()},
                                               {NodeType, 1, "Body", part.View()}};
            const test::graph::Node second[] = {{NodeType, 0, {}, asset.View()},
                                                {NodeType, 1, "Arm", part.View()}};
            GraphNodeDesc firstDesc[2];
            GraphNodeDesc secondDesc[2];
            Describe(first, firstDesc);
            Describe(second, secondDesc);
            const Array<uint8> firstBytes = MakeGraph(allocator, first, false);
            const Array<uint8> secondBytes = MakeGraph(allocator, second, false);
            auto input = [](Span<const uint8> bytes, Span<const GraphNodeDesc> nodes) noexcept {
                ObjectGraphInput value;
                value.bytes = bytes;
                value.nodes = nodes;
                value.hasBase = true;
                value.base = OuterSummary::ForNewOutermost(true);
                return value;
            };
            const uint32 liveBefore = registry.Stats().liveObjects;
            for (bool failPrepare : {true, false}) {
                // worker에서 해도 되는 1단계를 트랜잭션 전에 끝낸다.
                ObjectGraphStage stageA(allocator, registry, types, registry.OwnerAccess());
                ObjectGraphStage stageB(allocator, registry, types, registry.OwnerAccess());
                REQUIRE(stageA.Validate(input(firstBytes.View(), firstDesc)));
                REQUIRE(stageB.Validate(input(secondBytes.View(), secondDesc)));
                ConstructionContext transaction(registry, registry.OwnerAccess());
                auto packageA = transaction.Create<Object>(PackType);
                auto packageB = transaction.Create<Object>(PackType);
                REQUIRE(packageA && packageB);
                REQUIRE(stageA.CreateObjects(transaction, packageA.Value()));
                REQUIRE(stageB.CreateObjects(transaction, packageB.Value()));
                // import 해석: 같은 트랜잭션의 공개 전 객체(SCC 형제)와 생존 객체를 객체 경로로
                // 찾는다.
                CHECK(Finds(transaction.FindObject(packageA.Value(), "Body"), stageA.Nodes()[1]));
                CHECK(Finds(transaction.FindObject(packageB.Value(), ""), stageB.Nodes()[0]));
                CHECK(Finds(transaction.FindObject(packageB.Value(), "aRM"), stageB.Nodes()[1]));
                CHECK(Finds(transaction.FindObject(live.Value(), "Shared"), shared.Value()));
                CHECK(transaction.FindObject(packageA.Value(), "Arm").Error() ==
                      ErrorCode::NotFound);
                // Registry 조회와 다른 트랜잭션은 이 트랜잭션의 공개 전 객체를 보지 않는다.
                CHECK(
                    registry.FindObject(registry.OwnerAccess(), packageA.Value(), "Body").Error() ==
                    ErrorCode::StaleHandle);
                {
                    ConstructionContext otherTransaction(registry, registry.OwnerAccess());
                    CHECK(otherTransaction.FindObject(packageA.Value(), "Body").Error() ==
                          ErrorCode::StaleHandle);
                    auto extra =
                        otherTransaction.Create<Object>(NodeType, liveAsset.Value(), "Extra");
                    REQUIRE(extra);
                    CHECK(Finds(otherTransaction.FindObject(live.Value(), "Extra"), extra.Value()));
                    CHECK(transaction.FindObject(live.Value(), "Extra").Error() ==
                          ErrorCode::NotFound);
                }

                REQUIRE(stageA.ApplyFields(transaction));
                REQUIRE(stageB.ApplyFields(transaction));
                REQUIRE(stageA.CheckRequired(transaction));
                REQUIRE(stageB.CheckRequired(transaction));
                // prepare(호출자): Commit 전에 staged 객체의 Transient 필드를 쓴다.
                auto body = transaction.GetForWrite<StageNode>(stageA.Nodes()[1]);
                REQUIRE(body);
                body.Value()->cache = 77;
                if (failPrepare) {
                    // prepare가 실패하면 호출자가 버린다 — 부분 공개 없음.
                    transaction.Rollback();
                    CHECK(registry.Stats().reservedObjects == 0);
                    CHECK(registry.Stats().liveObjects == liveBefore);
                    continue;
                }

                const ObjectHandle packages[] = {packageA.Value(), packageB.Value()};
                RootRef<Object> held[2];
                REQUIRE(transaction.Commit(packages, held));
                auto bodyAfter =
                    registry.FindObject(registry.OwnerAccess(), held[0].Handle(), "Body");
                REQUIRE(bodyAfter);
                const Seen seen = Look(registry, bodyAfter.Value());
                CHECK(seen.number == 2 && seen.cache == 77);
                CHECK(seen.link == stageA.Nodes()[0]);
                auto armAfter =
                    registry.FindObject(registry.OwnerAccess(), held[1].Handle(), "Arm");
                CHECK(Finds(armAfter, stageB.Nodes()[1]));
                CHECK(registry.Stats().liveObjects == liveBefore + 6);
                // 패키지 정책: 패키지 RootRef를 놓기 전에 Discard한다.
                REQUIRE(registry.Discard(registry.OwnerAccess(), held[0].Handle()));
                REQUIRE(registry.Discard(registry.OwnerAccess(), held[1].Handle()));
            }

            REQUIRE(registry.Discard(registry.OwnerAccess(), live.Value()));
            for (RootRef<Object>& held : roots) {
                held.Reset();
            }
        }

        REQUIRE(registry.Collect(registry.OwnerAccess()));
        CHECK(registry.Stats().liveObjects == 0);
        CHECK(registry.Stats().retiredWithoutDiscard == 0);
        REQUIRE(registry.Shutdown(registry.OwnerAccess()));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 편의 복원의 prepare가 Transient 필드를 Commit 전에 쓰고, 실패하면 그 오류로 rollback한다.
TEST(GraphStage_ConveniencePrepareSetsTransientFields) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry registry(allocator, types, {0x475337a, 1});
        {
            const Array<uint8> node = MakeRecord(allocator, NodeType, 3);
            const test::graph::Node nodes[] = {{NodeType, 0, {}, node.View()}};
            const Array<uint8> graph = MakeGraph(allocator, nodes, true);
            struct State {
                ErrorCode fail = ErrorCode::None;
            } state;
            ObjectGraphLoadOptions options;
            options.prepareUser = &state;
            options.prepare = [](ConstructionContext& transaction, Span<const ObjectHandle> handles,
                                 void* user) noexcept -> Result<void> {
                auto value = transaction.GetForWrite<StageNode>(handles[0]);
                if (!value) {
                    return Result<void>::Failure(value.Error());
                }

                value.Value()->cache = 41;
                const ErrorCode fail = static_cast<State*>(user)->fail;
                return fail == ErrorCode::None ? Result<void>::Success()
                                               : Result<void>::Failure(fail);
            };
            {
                auto loaded = LoadObjectGraph(allocator, registry, types, registry.OwnerAccess(),
                                              graph.View(), options);
                REQUIRE(loaded);
                const Seen seen = Look(registry, loaded.Value().Handle());
                CHECK(seen.number == 3 && seen.cache == 41);
            }

            REQUIRE(registry.Collect(registry.OwnerAccess()));
            state.fail = ErrorCode::Busy;
            CHECK(LoadObjectGraph(allocator, registry, types, registry.OwnerAccess(), graph.View(),
                                  options)
                      .Error() == ErrorCode::Busy);
            CHECK(registry.Stats().liveObjects == 0 && registry.Stats().reservedObjects == 0);
        }

        REQUIRE(registry.Shutdown(registry.OwnerAccess()));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// Stage allocator의 모든 할당 실패(비ASCII 이름의 비교 키, Required 검사 버퍼 포함)는
// OutOfMemory이고, 할당·lease·객체를 원상태로 돌린다. 트랜잭션에 객체를 넣은 뒤의
// 실패(ApplyFields·CheckRequired)는 트랜잭션을 그 오류로 고정해 Commit이 실패하고 소멸자가
// 되돌린다.
TEST(GraphStage_EveryAllocationFailureRestoresState) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry registry(allocator, types, {0x475338a, 1});
        {
            Array<RootRef<Object>> roots(allocator);
            auto base = New(registry, roots);
            REQUIRE(base);
            const Array<uint8> root = MakeRecord(allocator, NodeType, 1, 2);
            const Array<uint8> child = MakeRecord(allocator, NodeType, 2, 1);
            // 비ASCII 이름(Ä·한글)은 비교 키를 만들 때 정규화 저장소를 할당한다.
            const test::graph::Node nodes[] = {{NodeType, 0, "\xC3\x84rm", root.View()},
                                               {NodeType, 1, "\xEB\xAA\xB8", child.View()}};
            GraphNodeDesc descs[2];
            Describe(nodes, descs);
            const Array<uint8> graph = MakeGraph(allocator, nodes, false);
            ObjectGraphInput input;
            input.bytes = graph.View();
            input.nodes = descs;
            input.hasBase = true;
            input.base = OuterSummary::ForNewOutermost(false);
            test::NFailAllocator failure(allocator);
            // hooks가 있어 Required 검사(2c)도 Stage allocator로 할당한다.
            ThreadDiagnostics diagnostics;
            ObjectGraphLoadOptions options;
            options.hooks = &diagnostics;
            // 생성 뒤 실패에서 트랜잭션이 그 오류로 고정되었는지(Commit도 실패하는지) 센다.
            uint32 afterCreate = 0;
            uint32 unfixed = 0;
            auto runOnce = [&]() noexcept -> ErrorCode {
                ObjectGraphStage stage(failure, registry, types, registry.OwnerAccess(), options);
                if (auto valid = stage.Validate(input); !valid) {
                    return valid.Error();
                }

                ConstructionContext transaction(registry, registry.OwnerAccess());
                if (auto created = stage.CreateObjects(transaction, base.Value()); !created) {
                    return created.Error();
                }

                auto finished = stage.ApplyFields(transaction);
                if (finished) {
                    finished = stage.CheckRequired(transaction);
                }

                if (!finished) {
                    ++afterCreate;
                    const ObjectHandle publish[] = {stage.Nodes()[0]};
                    RootRef<Object> held[1];
                    if (transaction.CheckOpen().IsOk() ||
                        transaction.CheckOpen().Error() != finished.Error() ||
                        ErrorOf(transaction.Commit(publish, held)) != finished.Error()) {
                        ++unfixed;
                    }

                    return finished.Error();
                }

                return ErrorCode::None;
            };
            REQUIRE(runOnce() == ErrorCode::None);
            const uint64 attempts = failure.Attempts();
            REQUIRE(attempts != 0);
            const auto stats = allocator.Stats();
            const auto leases = types.Stats().leases;
            for (uint64 index = 1; index <= attempts; ++index) {
                failure.FailOnAttempt(index);
                CHECK(runOnce() == ErrorCode::OutOfMemory);
                failure.DisableFailure();
                CHECK(allocator.Stats().liveAllocations == stats.liveAllocations);
                CHECK(types.Stats().leases == leases);
                CHECK(registry.Stats().reservedObjects == 0);
                CHECK(registry.Stats().liveObjects == 1);
            }

            CHECK(afterCreate != 0);
            CHECK(unfixed == 0);
        }

        REQUIRE(registry.Shutdown(registry.OwnerAccess()));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}
