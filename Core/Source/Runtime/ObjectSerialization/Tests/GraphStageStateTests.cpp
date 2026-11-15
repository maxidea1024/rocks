#include <TestSupport/Test.h>
#include "GraphStageTestSupport.h"

// 단계형 복원(ObjectGraphStage)의 상태 기계·스레드·트랜잭션 고정 시험이다(Docs/Design/
// ObjectFoundations.ko.md 5.4절, 9절 Od2): 호출 검사 실패는 상태를 바꾸지 않고(순서·스레드·문맥·
// 트랜잭션), 진행 중인 같은 Stage에 다시 들어오면 Busy, 끝난 트랜잭션은 이름 점유 확인보다 먼저
// InvalidArgument, 트랜잭션에 객체를 넣은 뒤의 실패는 트랜잭션을 고정한다. worker Validate·hook
// 스레드·Required 검사 중 FindObject도 본다.
using namespace rocks;
using namespace rocks::test::stage;

// 상태 기계: 순서 위반·다른 트랜잭션·문맥 오류 같은 호출 검사 실패는 상태를 바꾸지 않아 같은
// Stage가 올바른 호출로 계속 진행된다. 순서 확인 뒤의 실패(Validate의 손상 byte)는 이후 모든 단계를
// InvalidArgument로 만든다. 저장소가 다르거나 접근 문맥이 다른 Stage는 모든 단계가 WrongContext다.
TEST(GraphStage_PhaseOrderAndCallChecksKeepState) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        TypeRegistry otherTypes(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry registry(allocator, types, {0x475334a, 1});
        ObjectRegistry otherRegistry(allocator, types, {0x475334a, 2});
        {
            const Array<uint8> node = MakeRecord(allocator, NodeType, 5);
            const test::graph::Node nodes[] = {{NodeType, 0, {}, node.View()}};
            const Array<uint8> graph = MakeGraph(allocator, nodes, true);
            ObjectGraphInput input;
            input.bytes = graph.View();
            {
                ObjectGraphStage stage(allocator, registry, types, registry.OwnerAccess());
                ConstructionContext transaction(registry, registry.OwnerAccess());
                CHECK(stage.CreateObjects(transaction).Error() == ErrorCode::InvalidArgument);
                CHECK(stage.ApplyFields(transaction).Error() == ErrorCode::InvalidArgument);
                CHECK(stage.CheckRequired(transaction).Error() == ErrorCode::InvalidArgument);
                REQUIRE(stage.Validate(input));
                CHECK(stage.Validate(input).Error() == ErrorCode::InvalidArgument);
                CHECK(stage.ApplyFields(transaction).Error() == ErrorCode::InvalidArgument);
                // 다른 Registry의 트랜잭션은 WrongContext이고 상태는 그대로다.
                {
                    ConstructionContext foreign(otherRegistry, otherRegistry.OwnerAccess());
                    CHECK(stage.CreateObjects(foreign).Error() == ErrorCode::WrongContext);
                }

                REQUIRE(stage.CreateObjects(transaction));
                CHECK(stage.CreateObjects(transaction).Error() == ErrorCode::InvalidArgument);
                CHECK(stage.CheckRequired(transaction).Error() == ErrorCode::InvalidArgument);
                // 다른 트랜잭션은 InvalidArgument이고 상태는 그대로라 올바른 트랜잭션으로 계속한다.
                {
                    ConstructionContext otherTransaction(registry, registry.OwnerAccess());
                    CHECK(stage.ApplyFields(otherTransaction).Error() ==
                          ErrorCode::InvalidArgument);
                }

                REQUIRE(stage.ApplyFields(transaction));
                REQUIRE(stage.CheckRequired(transaction));
                CHECK(stage.CheckRequired(transaction).Error() == ErrorCode::InvalidArgument);
                CHECK(stage.Nodes().Size() == 1);
                const ObjectHandle roots[] = {stage.Nodes()[0]};
                RootRef<Object> held[1];
                REQUIRE(transaction.Commit(roots, held));
                CHECK(Look(registry, held[0].Handle()).number == 5);
            }

            REQUIRE(registry.Collect(registry.OwnerAccess()));
            // 끝난 트랜잭션: CreateObjects 뒤 Commit하면 ApplyFields는 InvalidArgument이고 상태는
            // 그대로다(새 트랜잭션은 CreateObjects의 트랜잭션이 아니므로 역시 InvalidArgument).
            {
                ObjectGraphStage stage(allocator, registry, types, registry.OwnerAccess());
                REQUIRE(stage.Validate(input));
                ConstructionContext transaction(registry, registry.OwnerAccess());
                REQUIRE(stage.CreateObjects(transaction));
                const ObjectHandle roots[] = {stage.Nodes()[0]};
                RootRef<Object> held[1];
                REQUIRE(transaction.Commit(roots, held));
                CHECK(stage.ApplyFields(transaction).Error() == ErrorCode::InvalidArgument);
                ConstructionContext fresh(registry, registry.OwnerAccess());
                CHECK(stage.ApplyFields(fresh).Error() == ErrorCode::InvalidArgument);
                // 적용 전에 공개했으므로 값은 생성자 값이다.
                CHECK(Look(registry, held[0].Handle()).number == 0);
            }

            REQUIRE(registry.Collect(registry.OwnerAccess()));
            // Validate 실패(magic) 뒤에는 다시 부를 수 없다(순서 확인 뒤의 실패).
            {
                Array<uint8> corrupt(allocator);
                REQUIRE(corrupt.TryResize(graph.Size()));
                for (usize index = 0; index < graph.Size(); ++index) {
                    corrupt[index] = graph[index];
                }

                corrupt[0] = 'X';
                ObjectGraphInput bad;
                bad.bytes = corrupt.View();
                ObjectGraphStage stage(allocator, registry, types, registry.OwnerAccess());
                CHECK(stage.Validate(bad).Error() == ErrorCode::InvalidArgument);
                CHECK(stage.Validate(input).Error() == ErrorCode::InvalidArgument);
                CHECK(stage.StoredVersion(0).Error() == ErrorCode::InvalidArgument);
            }

            // 저장소 불일치·접근 문맥 불일치는 모든 단계가 WrongContext다.
            {
                ObjectGraphStage mismatched(allocator, registry, otherTypes,
                                            registry.OwnerAccess());
                ObjectGraphStage foreign(allocator, registry, types, {{0x475334a, 9}});
                ConstructionContext transaction(registry, registry.OwnerAccess());
                ObjectGraphStage* const stageCases[] = {&mismatched, &foreign};
                for (ObjectGraphStage* stage : stageCases) {
                    CHECK(stage->Validate(input).Error() == ErrorCode::WrongContext);
                    CHECK(stage->CreateObjects(transaction).Error() == ErrorCode::WrongContext);
                    CHECK(stage->ApplyFields(transaction).Error() == ErrorCode::WrongContext);
                    CHECK(stage->CheckRequired(transaction).Error() == ErrorCode::WrongContext);
                }
            }

            CHECK(registry.Stats().reservedObjects == 0);
        }

        REQUIRE(registry.Shutdown(registry.OwnerAccess()));
        REQUIRE(otherRegistry.Shutdown(otherRegistry.OwnerAccess()));
        REQUIRE(types.Shutdown());
        REQUIRE(otherTypes.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// Validate는 ObjectRegistry에 닿지 않으므로 worker 스레드에서 부를 수 있고 1단계 hook(모르는 필드
// 진단)은 그 스레드에서 불린다. RequiredMissing(2c)은 소유 스레드에서 오고, Required 검사 중 표시
// 동안에도 ConstructionContext::FindObject는 된다(Create는 Busy). 소유 스레드 단계를 worker에서
// 부르면 Registry를 부르지 않는 그래프에서도 WrongThread이고 상태는 그대로다. worker에서 만든
// Stage와 기준 Outer 없는 편의 복원은 손상 byte보다 먼저 WrongThread다.
TEST(GraphStage_ValidateRunsOnWorkerThread) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry registry(allocator, types, {0x475335a, 1});
        {
            const Array<uint8> node = MakeRecord(allocator, NodeType, 9, 0, true);
            const test::graph::Node nodes[] = {{NodeType, 0, {}, node.View()}};
            const Array<uint8> graph = MakeGraph(allocator, nodes, true);
            ObjectGraphInput input;
            input.bytes = graph.View();
            ThreadDiagnostics diagnostics;
            ObjectGraphLoadOptions options;
            options.hooks = &diagnostics;
            const uint64 owner = platform::CurrentThreadId();
            {
                ObjectGraphStage stage(allocator, registry, types, registry.OwnerAccess(), options);
                ErrorCode validated = ErrorCode::DependencyFailed;
                auto validate = [&]() noexcept { validated = ErrorOf(stage.Validate(input)); };
                REQUIRE(RunOnWorker(validate));
                CHECK(validated == ErrorCode::None);
                REQUIRE(diagnostics.count == 1);
                CHECK(diagnostics.kinds[0] == FieldDiagnosticKind::UnknownField);
                CHECK(diagnostics.threads[0] != owner);
                ConstructionContext transaction(registry, registry.OwnerAccess());
                REQUIRE(stage.CreateObjects(transaction));
                REQUIRE(stage.ApplyFields(transaction));
                diagnostics.probe = &transaction;
                diagnostics.probeOutermost = stage.Nodes()[0];
                REQUIRE(stage.CheckRequired(transaction));
                diagnostics.probe = nullptr;
                REQUIRE(diagnostics.count == 2);
                CHECK(diagnostics.kinds[1] == FieldDiagnosticKind::RequiredMissing);
                CHECK(diagnostics.threads[1] == owner);
                // 검사 중 표시 동안 FindObject는 읽기라 된다(대표 자식이 없어 NotFound — Busy가
                // 아니다). Create는 Busy였다.
                CHECK(diagnostics.findError == ErrorCode::NotFound);
                CHECK(diagnostics.createError == ErrorCode::Busy);
                const ObjectHandle roots[] = {stage.Nodes()[0]};
                RootRef<Object> held[1];
                REQUIRE(transaction.Commit(roots, held));
                CHECK(Look(registry, held[0].Handle()).number == 9);
            }

            REQUIRE(registry.Collect(registry.OwnerAccess()));
            // 소유 스레드 단계를 worker에서 부르면 WrongThread이고 상태는 그대로다 — 필드 없는
            // 노드만 있고 hooks도 없어 ApplyFields·CheckRequired가 Registry를 부르지 않는
            // 그래프에서도 그렇다. 이어서 소유 스레드에서 계속한다.
            {
                const Array<uint8> empty = MakeRecord(allocator);
                const test::graph::Node bare[] = {{NodeType, 0, {}, empty.View()}};
                const Array<uint8> bareGraph = MakeGraph(allocator, bare, true);
                ObjectGraphInput bareInput;
                bareInput.bytes = bareGraph.View();
                ObjectGraphStage stage(allocator, registry, types, registry.OwnerAccess());
                REQUIRE(stage.Validate(bareInput));
                ConstructionContext transaction(registry, registry.OwnerAccess());
                ErrorCode foreign = ErrorCode::None;
                auto create = [&]() noexcept {
                    foreign = ErrorOf(stage.CreateObjects(transaction));
                };
                REQUIRE(RunOnWorker(create));
                CHECK(foreign == ErrorCode::WrongThread);
                REQUIRE(stage.CreateObjects(transaction));
                auto apply = [&]() noexcept { foreign = ErrorOf(stage.ApplyFields(transaction)); };
                REQUIRE(RunOnWorker(apply));
                CHECK(foreign == ErrorCode::WrongThread);
                REQUIRE(stage.ApplyFields(transaction));
                auto check = [&]() noexcept {
                    foreign = ErrorOf(stage.CheckRequired(transaction));
                };
                REQUIRE(RunOnWorker(check));
                CHECK(foreign == ErrorCode::WrongThread);
                REQUIRE(stage.CheckRequired(transaction));
                CHECK(transaction.CheckOpen().IsOk());
            }

            // worker에서 만든 Stage는 모든 단계가 WrongThread다(생성자는 소유 스레드 전용).
            ErrorCode constructed = ErrorCode::None;
            auto construct = [&]() noexcept {
                ObjectGraphStage stage(allocator, registry, types, registry.OwnerAccess());
                constructed = ErrorOf(stage.Validate(input));
            };
            REQUIRE(RunOnWorker(construct));
            CHECK(constructed == ErrorCode::WrongThread);
            // 기준 Outer 없는 편의 복원을 worker에서 손상 byte로 부르면 envelope보다 먼저
            // WrongThread다.
            const uint8 garbage[] = {'X', 'Y', 'Z'};
            ErrorCode loaded = ErrorCode::None;
            auto load = [&]() noexcept {
                auto result = LoadObjectGraph(allocator, registry, types, registry.OwnerAccess(),
                                              Span<const uint8>(garbage, sizeof(garbage)));
                loaded = result ? ErrorCode::None : result.Error();
            };
            REQUIRE(RunOnWorker(load));
            CHECK(loaded == ErrorCode::WrongThread);
            CHECK(LoadObjectGraph(allocator, registry, types, registry.OwnerAccess(),
                                  Span<const uint8>(garbage, sizeof(garbage)))
                      .Error() == ErrorCode::InvalidArgument);
            CHECK(registry.Stats().liveObjects == 0 && registry.Stats().reservedObjects == 0);
        }

        REQUIRE(registry.Shutdown(registry.OwnerAccess()));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 진행 중인 Stage에 다시 들어오면 Busy이고 상태를 바꾸지 않는다: 노드 생성자가 같은 Stage의
// CreateObjects를 부르면(계약 위반 모의 — 기준 Outer 있는 그래프와 없는 그래프) 안쪽은 Busy, 바깥은
// 정상으로 끝나 노드 셋이 모두 만들어지고 이어서 적용·Commit된다.
TEST(GraphStage_ReentryIsBusy) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry registry(allocator, types, {0x475339a, 1});
        {
            Array<RootRef<Object>> roots(allocator);
            auto base = New(registry, roots, {}, "Base");
            REQUIRE(base);
            const Array<uint8> first = MakeRecord(allocator, NodeType, 3);
            const Array<uint8> reenter = MakeRecord(allocator, ReenterType);
            const Array<uint8> last = MakeRecord(allocator, NodeType, 4);
            const bool hasBaseCases[] = {false, true};
            for (bool hasBase : hasBaseCases) {
                const StringView firstName = hasBase ? StringView("P") : StringView();
                const test::graph::Node nodes[] = {{NodeType, 0, firstName, first.View()},
                                                   {ReenterType, 1, "R", reenter.View()},
                                                   {NodeType, 1, "Z", last.View()}};
                const Array<uint8> graph = MakeGraph(allocator, nodes, true, hasBase);
                ObjectGraphInput input;
                input.bytes = graph.View();
                input.hasBase = hasBase;
                if (hasBase) {
                    auto described = registry.DescribeOuter(registry.OwnerAccess(), base.Value());
                    REQUIRE(described);
                    input.base = described.Value();
                }

                ObjectGraphStage stage(allocator, registry, types, registry.OwnerAccess());
                REQUIRE(stage.Validate(input));
                ConstructionContext transaction(registry, registry.OwnerAccess());
                const ObjectHandle baseOuter = hasBase ? base.Value() : ObjectHandle{};
                ReentryStage = &stage;
                ReentryTransaction = &transaction;
                ReentryBase = baseOuter;
                ReentryError = ErrorCode::None;
                auto created = stage.CreateObjects(transaction, baseOuter);
                ReentryStage = nullptr;
                REQUIRE(created);
                CHECK(ReentryError == ErrorCode::Busy);
                REQUIRE(stage.Nodes().Size() == 3);
                REQUIRE(stage.ApplyFields(transaction));
                REQUIRE(stage.CheckRequired(transaction));
                const ObjectHandle publish[] = {stage.Nodes()[0]};
                RootRef<Object> held[1];
                REQUIRE(transaction.Commit(publish, held));
                CHECK(Look(registry, stage.Nodes()[0]).number == 3);
                CHECK(Look(registry, stage.Nodes()[2]).number == 4);
                CHECK(NameIs(registry, stage.Nodes()[2], "Z", allocator));
            }

            for (RootRef<Object>& root : roots) {
                root.Reset();
            }
        }

        REQUIRE(registry.Collect(registry.OwnerAccess()));
        CHECK(registry.Stats().liveObjects == 0);
        REQUIRE(registry.Shutdown(registry.OwnerAccess()));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 트랜잭션에 객체를 넣은 뒤의 실패는 트랜잭션을 그 오류로 고정한다: ApplyFields(staged 쓰기 실패)·
// CheckRequired(hook 실패) 뒤 Commit(roots)은 그 오류로 실패하고 소멸자가 되돌려 공개된 객체가
// 없다. 한 트랜잭션에 Stage 둘이면 한쪽의 실패가 트랜잭션 전체를 실패시키고, 다른 Stage의 다음
// 단계는 그 오류를 돌려준다(상태 불변). 사전 확인 실패는 고정하지 않는다(같은 트랜잭션의 다른
// Stage가 계속된다).
TEST(GraphStage_FailureAfterCreateFixesTransaction) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry registry(allocator, types, {0x47533aa, 1});
        {
            const Array<uint8> first = MakeRecord(allocator, NodeType, 7);
            const Array<uint8> second = MakeRecord(allocator, NodeType, 9, 1);
            const test::graph::Node nodes[] = {{NodeType, 0, {}, first.View()},
                                               {NodeType, 1, "Two", second.View()}};
            const Array<uint8> graph = MakeGraph(allocator, nodes, true);
            ObjectGraphInput input;
            input.bytes = graph.View();
            ThreadDiagnostics diagnostics;
            ObjectGraphLoadOptions options;
            options.hooks = &diagnostics;
            const bool failApplyCases[] = {true, false};
            for (bool failApply : failApplyCases) {
                ObjectGraphStage stage(allocator, registry, types, registry.OwnerAccess(), options);
                REQUIRE(stage.Validate(input));
                ConstructionContext transaction(registry, registry.OwnerAccess());
                REQUIRE(stage.CreateObjects(transaction));
                FailNumberWrite = failApply;
                diagnostics.failRequired = failApply ? ErrorCode::None : ErrorCode::Unsupported;
                Result<void> failed = stage.ApplyFields(transaction);
                FailNumberWrite = false;
                if (!failApply) {
                    REQUIRE(failed);
                    failed = stage.CheckRequired(transaction);
                }

                diagnostics.failRequired = ErrorCode::None;
                const ErrorCode expected =
                    failApply ? ErrorCode::OutOfMemory : ErrorCode::Unsupported;
                CHECK(ErrorOf(failed) == expected);
                CHECK(ErrorOf(transaction.CheckOpen()) == expected);
                const ObjectHandle publish[] = {stage.Nodes()[0]};
                RootRef<Object> held[1];
                CHECK(ErrorOf(transaction.Commit(publish, held)) == expected);
                CHECK(!held[0].IsValid());
                CHECK(stage.CheckRequired(transaction).Error() == ErrorCode::InvalidArgument);
            }

            CHECK(registry.Stats().liveObjects == 0 && registry.Stats().reservedObjects == 0);
            // 한 트랜잭션에 Stage 둘: A의 적용 실패가 트랜잭션을 고정하면 B의 다음 단계는 그
            // 오류(상태 불변)이고 Commit도 실패한다.
            {
                const test::graph::Node other[] = {{NodeType, 0, "B", first.View()}};
                const Array<uint8> otherGraph = MakeGraph(allocator, other, true);
                ObjectGraphInput otherInput;
                otherInput.bytes = otherGraph.View();
                const test::graph::Node named[] = {{NodeType, 0, "A", first.View()}};
                const Array<uint8> namedGraph = MakeGraph(allocator, named, true);
                ObjectGraphInput namedInput;
                namedInput.bytes = namedGraph.View();
                ObjectGraphStage stageA(allocator, registry, types, registry.OwnerAccess());
                ObjectGraphStage stageB(allocator, registry, types, registry.OwnerAccess());
                ObjectGraphStage stageC(allocator, registry, types, registry.OwnerAccess());
                REQUIRE(stageA.Validate(namedInput));
                REQUIRE(stageB.Validate(otherInput));
                REQUIRE(stageC.Validate(otherInput));
                ConstructionContext transaction(registry, registry.OwnerAccess());
                REQUIRE(stageA.CreateObjects(transaction));
                REQUIRE(stageB.CreateObjects(transaction));
                // 사전 확인 실패(C의 이름 B는 이미 같은 트랜잭션에 있다)는 트랜잭션을 고정하지
                // 않는다.
                CHECK(stageC.CreateObjects(transaction).Error() == ErrorCode::AlreadyExists);
                CHECK(transaction.CheckOpen().IsOk());
                FailNumberWrite = true;
                CHECK(stageA.ApplyFields(transaction).Error() == ErrorCode::OutOfMemory);
                FailNumberWrite = false;
                CHECK(stageB.ApplyFields(transaction).Error() == ErrorCode::OutOfMemory);
                CHECK(ErrorOf(transaction.Fail(ErrorCode::Busy)) == ErrorCode::None);
                CHECK(ErrorOf(transaction.CheckOpen()) == ErrorCode::OutOfMemory);
                const ObjectHandle publish[] = {stageA.Nodes()[0], stageB.Nodes()[0]};
                RootRef<Object> held[2];
                CHECK(transaction.Commit(publish, held).Error() == ErrorCode::OutOfMemory);
            }

            CHECK(registry.Stats().liveObjects == 0 && registry.Stats().reservedObjects == 0);
        }

        REQUIRE(registry.Shutdown(registry.OwnerAccess()));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 끝난 트랜잭션은 이름 점유 확인보다 먼저 InvalidArgument다(기준 Outer 유무와 무관 — 이름이 이미
// 점유되어도 AlreadyExists가 아니다). 고정된 트랜잭션은 그 오류다. 모두 상태를 바꾸지 않는다.
// ConstructionContext::Fail의 규칙(None 거부·먼저 고정된 오류 유지·끝난 트랜잭션 InvalidArgument·
// 다른 스레드 WrongThread)도 본다.
TEST(GraphStage_FinishedTransactionIsRejectedFirst) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry registry(allocator, types, {0x47533ba, 1});
        {
            Array<RootRef<Object>> roots(allocator);
            REQUIRE(New(registry, roots, {}, "Taken"));
            auto base = New(registry, roots);
            REQUIRE(base);
            const Array<uint8> record = MakeRecord(allocator);
            const test::graph::Node taken[] = {{NodeType, 0, "Taken", record.View()}};
            const Array<uint8> takenGraph = MakeGraph(allocator, taken, true);
            const Array<uint8> basedGraph = MakeGraph(allocator, taken, true, true);
            ObjectGraphInput topInput;
            topInput.bytes = takenGraph.View();
            ObjectGraphInput baseInput;
            baseInput.bytes = basedGraph.View();
            baseInput.hasBase = true;
            baseInput.base = OuterSummary::ForNewOutermost(false);
            const bool hasBaseCases[] = {false, true};
            for (bool hasBase : hasBaseCases) {
                ObjectGraphStage stage(allocator, registry, types, registry.OwnerAccess());
                REQUIRE(stage.Validate(hasBase ? baseInput : topInput));
                const ObjectHandle baseOuter = hasBase ? base.Value() : ObjectHandle{};
                {
                    ConstructionContext committed(registry, registry.OwnerAccess());
                    REQUIRE(committed.Commit({}, {}));
                    CHECK(stage.CreateObjects(committed, baseOuter).Error() ==
                          ErrorCode::InvalidArgument);
                }

                {
                    ConstructionContext rolledBack(registry, registry.OwnerAccess());
                    rolledBack.Rollback();
                    CHECK(stage.CreateObjects(rolledBack, baseOuter).Error() ==
                          ErrorCode::InvalidArgument);
                }

                {
                    ConstructionContext fixed(registry, registry.OwnerAccess());
                    REQUIRE(fixed.Fail(ErrorCode::Busy));
                    CHECK(stage.CreateObjects(fixed, baseOuter).Error() == ErrorCode::Busy);
                }

                // 상태는 그대로라 열린 트랜잭션으로 계속한다(기준 Outer 없는 그래프는 최상위 이름
                // Taken이 점유되어 AlreadyExists — 끝난 트랜잭션 오류보다 뒤 단계다).
                ConstructionContext open(registry, registry.OwnerAccess());
                CHECK(ErrorOf(stage.CreateObjects(open, baseOuter)) ==
                      (hasBase ? ErrorCode::None : ErrorCode::AlreadyExists));
            }

            // Fail 규칙.
            {
                ConstructionContext transaction(registry, registry.OwnerAccess());
                CHECK(transaction.Fail(ErrorCode::None).Error() == ErrorCode::InvalidArgument);
                CHECK(transaction.CheckOpen().IsOk());
                ErrorCode foreign = ErrorCode::None;
                auto fail = [&]() noexcept {
                    foreign = ErrorOf(transaction.Fail(ErrorCode::Busy));
                };
                REQUIRE(RunOnWorker(fail));
                CHECK(foreign == ErrorCode::WrongThread);
                REQUIRE(transaction.Create<Object>(NodeType));
                REQUIRE(transaction.Fail(ErrorCode::NotFound));
                REQUIRE(transaction.Fail(ErrorCode::Busy));
                CHECK(transaction.CheckOpen().Error() == ErrorCode::NotFound);
                CHECK(transaction.Create<Object>(NodeType).Error() == ErrorCode::NotFound);
                CHECK(transaction.Commit().Error() == ErrorCode::NotFound);
                CHECK(registry.Stats().reservedObjects == 1);
            }

            CHECK(registry.Stats().reservedObjects == 0);
            {
                ConstructionContext committed(registry, registry.OwnerAccess());
                REQUIRE(committed.Commit({}, {}));
                CHECK(committed.Fail(ErrorCode::Busy).Error() == ErrorCode::InvalidArgument);
                CHECK(committed.CheckOpen().Error() == ErrorCode::InvalidArgument);
            }
        }

        REQUIRE(registry.Collect(registry.OwnerAccess()));
        REQUIRE(registry.Shutdown(registry.OwnerAccess()));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}
