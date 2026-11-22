#include <Core/Memory/SystemAllocator.h>
#include <SparksVm/VmSession.h>
#include <TestSupport/Test.h>

#include "VmTestSupport.h"

// hot reload API 시험이다(Docs/Design/SparksReload.ko.md — LC7a): PlanReload의 인자 거부(같은
// 객체·session 밖·다른 경로·링크 안 됨·닫힌 session), 계획 없는·소비한·낡은 계획의 ApplyReload
// 거부, 적용 뒤 References. 클래스·실행이 있는 경로는 `.run` golden(Run/Reload)이 덮는다.
using namespace rocks;
using namespace rocks::sparks;
namespace sparks_vm_reload_tests_file {

using namespace rocks::sparks::vm_test_support;

namespace {

// 경로 path에 Scalar 하나를 돌려주는 함수 F 하나뿐인 모듈을 조립한다.
void Build(Assembler& assembler, StringView path) noexcept {
    assembler.module.path = assembler.String(path);
    Shape shape;
    shape.registers[0] = 1;
    shape.frame[0] = 1;
    shape.returnSlots = 1;
    assembler.Begin("F", shape);
    assembler.Emit(EncodeABC(Opcode::Ret, 0, 1, 0));
    assembler.End();
    CHECK(assembler.Verified());
}

// 경로 Game/Lib에 Scalar 하나를 그대로 돌려주는 Id(x) 하나뿐인 모듈을 조립한다.
void BuildLibrary(Assembler& assembler) noexcept {
    assembler.module.path = assembler.String("Game/Lib");
    Shape shape;
    shape.registers[0] = 2;
    shape.frame[0] = 4;
    shape.parameters[0] = 1;
    shape.returnSlots = 1;
    assembler.Begin("Id", shape);
    assembler.Emit(EncodeABC(Opcode::Ret, 0, 1, 0));
    assembler.End();
    CHECK(assembler.Verified());
}

// 경로 Game/Use에 F 하나와 Game/Lib:Id의 import 참조를 둔 모듈을 조립한다.
void BuildUser(Assembler& assembler) noexcept {
    Build(assembler, "Game/Use");
    BytecodeRef ref;
    ref.kind = BytecodeRefKind::ImportedFunction;
    ref.name = assembler.String("Game/Lib:Id");
    ref.arguments = assembler.String("i64>i64");
    CHECK(assembler.module.refs.TryPushBack(ref));
    CHECK(assembler.Verified());
}

// first의 import를 modules와 targets로 푼다(실패면 CHECK).
void Import(VmLinkedModule& first, Span<const VmLinkedModule* const> targets,
            Allocator& allocator) noexcept {
    VmLinkedModule* modules[] = {&first};
    StringBuilder<256> reason(allocator);
    CHECK(LinkImports(Span<VmLinkedModule* const>(modules, 1), targets, reason));
}

} // namespace

TEST(SparksVmReload_BundleRulesForImports) {
    // 모듈 묶음 reload(Docs/Design/SparksReload.ko.md 6절 LC7c2b): 묶음 밖 모듈이 묶음의 옛
    // revision을 import하거나, 새 revision이 묶음의 옛 revision을 import하거나, 같은 모듈이 두 번
    // 나오면 계획을 거부한다. 새 revision끼리 이은 묶음은 한 번에 적용되고, LinkImports의 두 번째
    // 묶음(targets)은 바뀌지 않는다.
    SystemAllocator allocator;
    Assembler libOld(allocator);
    Assembler libNew(allocator);
    Assembler libOther(allocator);
    Assembler useOld(allocator);
    Assembler useStale(allocator);
    Assembler useNew(allocator);
    BuildLibrary(libOld);
    BuildLibrary(libNew);
    BuildLibrary(libOther);
    BuildUser(useOld);
    BuildUser(useStale);
    BuildUser(useNew);
    libOld.Linked();
    libNew.Linked();
    libOther.Linked();
    useOld.Linked();
    useStale.Linked();
    useNew.Linked();
    Import(libOld.linked, {}, allocator);
    Import(libNew.linked, {}, allocator);
    Import(libOther.linked, {}, allocator);
    const VmLinkedModule* oldTargets[] = {&libOld.linked};
    const VmLinkedModule* newTargets[] = {&libNew.linked};
    Import(useOld.linked, oldTargets, allocator);
    Import(useStale.linked, oldTargets, allocator); // 새 revision인데 옛 Lib을 가리킨다
    Import(useNew.linked, newTargets, allocator);
    VmSession session(allocator, nullptr);
    REQUIRE(session.AddModule(libOld.linked));
    REQUIRE(session.AddModule(useOld.linked));
    VmReloadPlan plan(allocator);
    StringBuilder<256> reason(allocator);
    auto rejects = [&](Span<const VmReloadModule> pairs, StringView why) noexcept {
        reason.Clear();
        auto planned = session.PlanReload(pairs, VmReloadExecutions::Cancel, plan, reason);
        CHECK(!planned);
        CHECK(!planned && planned.Error() == ErrorCode::InvalidArgument);
        CHECK(reason.View() == why);
        CHECK(!plan.Ready());
    };
    const VmReloadModule libOnly[] = {{&libOld.linked, &libNew.linked}};
    rejects(libOnly, "module Game/Use imports a module of the reload set (reload it too)");
    const VmReloadModule stale[] = {{&libOld.linked, &libNew.linked},
                                    {&useOld.linked, &useStale.linked}};
    rejects(stale, "the new revision of Game/Use imports an old revision of the reload set");
    const VmReloadModule twice[] = {{&libOld.linked, &libNew.linked},
                                    {&libOld.linked, &libOther.linked}};
    rejects(twice, "a module appears twice in the reload set");
    rejects(Span<const VmReloadModule>(), "the reload set is empty");
    // 새 revision끼리 이은 묶음: 한 계획·한 적용, 옛 revision은 놓인다.
    const VmReloadModule bundle[] = {{&libOld.linked, &libNew.linked},
                                     {&useOld.linked, &useNew.linked}};
    reason.Clear();
    REQUIRE(session.PlanReload(bundle, VmReloadExecutions::Cancel, plan, reason));
    auto applied = session.ApplyReload(plan);
    REQUIRE(applied);
    CHECK(applied.Value().modules == 2);
    CHECK(!session.References(libOld.linked));
    CHECK(!session.References(useOld.linked));
    CHECK(session.References(libNew.linked));
    CHECK(session.References(useNew.linked));
    // LinkImports의 targets(옛 Lib)는 새 revision을 이을 때 바뀌지 않았다(옛 Use의 import도 그대로
    // 풀린 상태 — Keep 실행의 호출은 옛 revision 안에서 해석된다).
    CHECK(useOld.linked.ImportsLinked());
    CHECK(libOld.linked.ImportsLinked());
}

TEST(SparksVmReload_RejectsBadArgumentsAndStalePlans) {
    SystemAllocator allocator;
    Assembler first(allocator);
    Assembler second(allocator);
    Assembler third(allocator);
    Assembler stranger(allocator);
    Assembler moved(allocator);
    Build(first, "Game/Door");
    Build(second, "Game/Door");
    Build(third, "Game/Door");
    Build(stranger, "Game/Door");
    Build(moved, "Game/Other");
    const VmLinkedModule& old = first.Linked();
    const VmLinkedModule& fresh = second.Linked();
    const VmLinkedModule& newer = third.Linked();
    const VmLinkedModule& outside = stranger.Linked();
    const VmLinkedModule& elsewhere = moved.Linked();
    VmLinkedModule unlinked(allocator);
    VmSession session(allocator, nullptr);
    REQUIRE(session.AddModule(old));
    VmReloadPlan plan(allocator);
    StringBuilder<256> reason(allocator);
    auto rejects = [&](const VmLinkedModule& from, const VmLinkedModule& to,
                       StringView why) noexcept {
        reason.Clear();
        auto planned = session.PlanReload(from, to, VmReloadExecutions::Cancel, plan, reason);
        CHECK(!planned);
        CHECK(!planned && planned.Error() == ErrorCode::InvalidArgument);
        CHECK(reason.View() == why);
        CHECK(!plan.Ready());
    };
    rejects(old, old, "the new revision must be a new linked module");
    rejects(outside, fresh, "the old revision is not a session module");
    rejects(old, elsewhere, "the new revision has another module path");
    rejects(old, unlinked, "the new revision is not linked with the session host");
    // 계획 없는 적용은 거부한다.
    CHECK(!session.ApplyReload(plan));
    // 적용: 모듈 목록이 바뀌고 옛 revision은 참조되지 않는다(실행·instance 없음).
    reason.Clear();
    REQUIRE(session.PlanReload(old, fresh, VmReloadExecutions::Keep, plan, reason));
    CHECK(plan.Ready());
    auto applied = session.ApplyReload(plan);
    REQUIRE(applied);
    CHECK(applied.Value().instances == 0);
    CHECK(applied.Value().cancelled == 0);
    CHECK(!plan.Ready());
    CHECK(!session.References(old));
    CHECK(session.References(fresh));
    // 소비한 계획은 다시 적용하지 않는다.
    CHECK(!session.ApplyReload(plan));
    // 이미 session 모듈인 revision으로는 바꾸지 않는다(fresh가 지금 모듈).
    rejects(fresh, fresh, "the new revision must be a new linked module");
    // 낡은 계획: 계획 뒤 다른 reload가 old를 바꿨으면 적용하지 않고 session은 그대로다.
    VmReloadPlan stale(allocator);
    reason.Clear();
    REQUIRE(session.PlanReload(fresh, newer, VmReloadExecutions::Cancel, stale, reason));
    reason.Clear();
    REQUIRE(session.PlanReload(fresh, outside, VmReloadExecutions::Cancel, plan, reason));
    REQUIRE(session.ApplyReload(plan));
    auto late = session.ApplyReload(stale);
    CHECK(!late);
    CHECK(!late && late.Error() == ErrorCode::InvalidArgument);
    CHECK(session.References(outside));
    CHECK(!session.References(newer));
    // 닫힌 session은 계획하지 않는다.
    REQUIRE(session.CloseSession());
    rejects(outside, newer, "the session is closed or running");
}

} // namespace sparks_vm_reload_tests_file
