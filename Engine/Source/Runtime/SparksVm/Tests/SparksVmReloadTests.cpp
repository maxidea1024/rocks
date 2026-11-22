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

} // namespace

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
