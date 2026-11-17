#include "TextAssignTestSupport.h"

// 리플렉션 B9i3c4 대입 묶음 시험이다(Docs/Design/ReflectionExtensions.ko.md 10.2절
// "구현(B9i3c4)에서 정한 점", fixture ReflectionTestFixtures/AssignFixture.h): StructRefArray 원소
// 지우기·비우기와 같은 묶음 안 index 이동(뒤 대입의 지금 index → 묶을 때 index, 지운 원소 안의 단위
// 버림), 지운 원소 참조의 관찰자 기록, 참조 struct `{}`의 StructRefArray 멤버 비우기, Publish
// 원자성(배열 변경 Busy· owner Discard·할당 실패 주입), staged 대상의 원소 지우기와 같은 트랜잭션의
// 공개 전 대상.
using namespace rocks;
using namespace rocks::reflection_text;
using namespace rocks::test::assign;
using reflection_fixture::AssignHolder;
using reflection_fixture::AssignTarget;

namespace {

// array(`items[0]` 꼴 원소 경로)의 배열 끝에 기본 원소 count개를 넣는다(live Registry API).
void InsertElements(Fixture& fixture, ObjectHandle object, const char* element,
                    uint32 count) noexcept {
    PropertyPathBuffer parsed;
    REQUIRE(ParsePropertyPath(fixture.holder, StringView(element), parsed));
    const PropertyPath path = parsed.View();
    PathStep steps[MaxPropertyPathSteps];
    for (usize index = 0; index < path.steps.Size(); ++index) {
        steps[index] = path.steps[index];
    }

    usize size = 0;
    {
        PathStep whole[MaxPropertyPathSteps];
        for (usize index = 0; index < path.steps.Size(); ++index) {
            whole[index] = path.steps[index];
        }

        whole[path.steps.Size() - 1].selector = PathSelector::None;
        whole[path.steps.Size() - 1].index = 0;
        auto counted = PathCount(fixture.objects, fixture.Access(), object,
                                 PropertyPath{Span<const PathStep>(whole, path.steps.Size()), {}});
        REQUIRE(counted);
        size = counted.Value();
    }

    for (uint32 index = 0; index < count; ++index) {
        steps[path.steps.Size() - 1].index = static_cast<uint32>(size + index);
        REQUIRE(PathInsertElement(fixture.objects, fixture.Access(), object,
                                  PropertyPath{Span<const PathStep>(steps, path.steps.Size()), {}},
                                  nullptr));
    }
}

void Apply(Fixture& fixture, ObjectHandle object, Span<const TextAssignment> inputs) noexcept {
    auto prepared = fixture.Prepare(object, inputs);
    REQUIRE(prepared);
    REQUIRE(prepared.Value().Publish());
}

} // namespace

// 원소 지우기는 같은 묶음의 뒤 대입이 지금 index로 가리키고(묶을 때 index로 옮긴다), 지운 원소 안의
// 앞 대입은 버린다. 남은 원소의 값·참조 단위는 압축 전에 게시하고, 지운 원소의 참조와 참조 단위의
// 간선이 관찰자에 한 번 기록된다.
TEST(TextAssign_ElementRemovalMovesIndicesAndRecordsObserver) {
    SystemAllocator system;
    Fixture fixture(system, true);
    auto root = fixture.Make();
    const ObjectHandle object = root.Handle();
    auto target = fixture.MakeTarget();
    TargetResolver resolver;
    resolver.target = target.Handle();
    fixture.options.resolver = &resolver;
    InsertElements(fixture, object, "items[0]", 4);
    const TextAssignment fill[] = {Set("items[0].weight", "1"),    Set("items[1].weight", "2", 1),
                                   Set("items[2].weight", "3", 2), Set("items[3].weight", "4", 3),
                                   Set("items[0].link", "/t", 4),  Set("items[1].link", "/t", 5),
                                   Set("items[3].link", "/t", 6)};
    Apply(fixture, object, fill);
    (void)fixture.Take();

    // 지금 index: [0 1 2 3] → items[1] 지우기 → [0 2 3] → items[0] 지우기 → [2 3].
    const TextAssignment edit[] = {
        Set("items[0].weight", "99"),    Other(AssignmentKind::Erase, "items[1]", 1),
        Set("items[1].weight", "30", 2), Other(AssignmentKind::Erase, "items[0]", 3),
        Set("items[1].link", "none", 4), Set("count", "5", 5)};
    auto prepared = fixture.Prepare(object, edit);
    REQUIRE(prepared);
    const Span<const PropertyPath> overrides = prepared.Value().OverridePaths();
    CHECK(HasOverride(fixture, overrides, "items") && HasOverride(fixture, overrides, "count"));
    auto published = prepared.Value().Publish();
    REQUIRE(published);
    // 지운 원소 0의 weight 단위는 버렸다: 원래 원소 2의 weight와 count, 원래 원소 3의 link다.
    CHECK(published.Value().UnitCount() == 2 && published.Value().ReferenceCount() == 1);
    {
        Peek holder(fixture, object);
        REQUIRE(holder->items.Size() == 2);
        CHECK(holder->items[0].weight == 30 && holder->items[0].link.Handle().IsNull());
        CHECK(holder->items[1].weight == 4 && holder->items[1].link.Handle().IsNull());
        CHECK(holder->count == 5);
    }

    // 지운 원소 0·1의 link와 원소 3의 link 비우기로 (package, target)이 -3으로 한 번 기록된다.
    Array<ReferenceEdgeDelta> changes = fixture.Take();
    REQUIRE(changes.Size() == 1);
    CHECK(changes[0].targetPackage == target.Handle() && changes[0].delta == -3);
}

// 비우기·범위 규칙과 참조 struct `{}`의 StructRefArray 멤버: 비운 배열의 원소 경로와 범위 밖
// 지우기는 OutOfRange, index 없는 지우기는 InvalidValue다. 같은 경로 문자열의 지우기 두 번은 앞 두
// 원소를 지운다(지금 index가 다르다). `bundle={}`는 parts를 비우고, 빈 배열 비우기는 무동작이다.
TEST(TextAssign_ElementClearAndStructResetEmptyArrays) {
    SystemAllocator system;
    Fixture fixture(system, true);
    auto root = fixture.Make();
    const ObjectHandle object = root.Handle();
    auto target = fixture.MakeTarget();
    TargetResolver resolver;
    resolver.target = target.Handle();
    fixture.options.resolver = &resolver;
    InsertElements(fixture, object, "items[0]", 3);
    InsertElements(fixture, object, "bundle.parts[0]", 2);
    const TextAssignment fill[] = {Set("items[0].link", "/t"), Set("items[2].link", "/t", 1),
                                   Set("bundle.parts[1].link", "/t", 2),
                                   Set("bundle.parts[0].weight", "6", 3)};
    Apply(fixture, object, fill);
    (void)fixture.Take();

    const TextAssignment rejected[] = {Set("items", ""), Set("items[0].weight", "1", 1),
                                       Other(AssignmentKind::Erase, "items[5]", 2),
                                       Other(AssignmentKind::Erase, "bundle.parts", 3)};
    CHECK(ErrorOf(fixture.Prepare(object, rejected)) == ErrorCode::InvalidArgument);
    REQUIRE(fixture.diagnostics.Count() == 3);
    CHECK(fixture.diagnostics.Get(0).code == TextDiagnosticCode::OutOfRange);
    CHECK(fixture.diagnostics.Get(1).code == TextDiagnosticCode::OutOfRange);
    CHECK(fixture.diagnostics.Get(2).code == TextDiagnosticCode::InvalidValue);
    {
        Peek holder(fixture, object);
        CHECK(holder->items.Size() == 3 && holder->bundle.parts.Size() == 2);
    }

    const TextAssignment twice[] = {Other(AssignmentKind::Erase, "items[0]"),
                                    Other(AssignmentKind::Erase, "items[0]", 1)};
    Apply(fixture, object, twice);
    {
        Peek holder(fixture, object);
        REQUIRE(holder->items.Size() == 1);
        CHECK(holder->items[0].link.Handle() == target.Handle());
    }

    Array<ReferenceEdgeDelta> erased = fixture.Take();
    REQUIRE(erased.Size() == 1);
    CHECK(erased[0].delta == -1);

    const TextAssignment reset[] = {Set("bundle", "{}"), Set("items", "", 1)};
    auto prepared = fixture.Prepare(object, reset);
    REQUIRE(prepared);
    CHECK(HasOverride(fixture, prepared.Value().OverridePaths(), "bundle"));
    CHECK(HasOverride(fixture, prepared.Value().OverridePaths(), "items"));
    REQUIRE(prepared.Value().Publish());
    {
        Peek holder(fixture, object);
        CHECK(holder->items.IsEmpty() && holder->bundle.parts.IsEmpty());
    }

    Array<ReferenceEdgeDelta> cleared = fixture.Take();
    REQUIRE(cleared.Size() == 1);
    CHECK(cleared[0].delta == -2);

    const TextAssignment empty[] = {Set("items", "")};
    auto unchanged = fixture.Prepare(object, empty);
    REQUIRE(unchanged);
    CHECK(HasOverride(fixture, unchanged.Value().OverridePaths(), "items"));
    REQUIRE(unchanged.Value().Publish());
    CHECK(fixture.Take().IsEmpty());
}

// 원소 지우기 Publish의 원자성: Prepare 뒤 배열이 바뀌면 Busy, 할당 실패(묶음 allocator의 참조
// 목록·블록 목록·간선 목록)는 OutOfMemory, owner Discard는 StaleHandle이고 모두 배열과 관찰자
// 기록이 그대로다. 실패 지점을 하나씩 옮긴 뒤 게시한다.
TEST(TextAssign_ElementRemovalFailuresChangeNothing) {
    SystemAllocator system;
    FailingAllocator failing(system);
    Fixture fixture(failing, true);
    auto root = fixture.Make();
    const ObjectHandle object = root.Handle();
    auto target = fixture.MakeTarget();
    TargetResolver resolver;
    resolver.target = target.Handle();
    fixture.options.resolver = &resolver;
    InsertElements(fixture, object, "items[0]", 3);
    const TextAssignment link[] = {Set("items[1].link", "/t")};
    Apply(fixture, object, link);
    (void)fixture.Take();

    const TextAssignment erase[] = {Other(AssignmentKind::Erase, "items[1]")};
    auto changed = fixture.Prepare(object, erase);
    REQUIRE(changed);
    InsertElements(fixture, object, "items[0]", 1);
    CHECK(ErrorOf(changed.Value().Publish()) == ErrorCode::Busy);
    {
        Peek holder(fixture, object);
        CHECK(holder->items.Size() == 4 && holder->items[1].link.Handle() == target.Handle());
    }

    CHECK(fixture.Take().IsEmpty());
    uint64 failures = 0;
    for (uint64 failAt = 1;; ++failAt) {
        auto prepared = fixture.Prepare(object, erase);
        REQUIRE(prepared);
        failing.allocations = 0;
        failing.failAt = failAt;
        auto published = prepared.Value().Publish();
        failing.failAt = 0;
        if (!published) {
            CHECK(published.Error() == ErrorCode::OutOfMemory);
            ++failures;
            CHECK(fixture.Take().IsEmpty());
            Peek holder(fixture, object);
            CHECK(holder->items.Size() == 4 && holder->items[1].link.Handle() == target.Handle());
            continue;
        }

        CHECK(fixture.Take().Size() == 1);
        break;
    }

    CHECK(failures >= 1);
    {
        Peek holder(fixture, object);
        CHECK(holder->items.Size() == 3 && holder->items[1].link.Handle().IsNull());
    }

    auto discarded = fixture.Prepare(object, erase);
    REQUIRE(discarded);
    REQUIRE(fixture.objects.Discard(fixture.Access(), object));
    CHECK(ErrorOf(discarded.Value().Publish()) == ErrorCode::StaleHandle);
}

// staged 대상: 원소 지우기는 트랜잭션 안 압축이고(앞 단위는 묶을 때 index로 먼저 게시), 같은
// 트랜잭션의 공개 전 객체를 새 대상으로 대입할 수 있다(고정하지 않는다). 게시된 객체 대상에서는
// 공개 전 객체가 살아 있지 않은 대상이다.
TEST(TextAssign_StagedElementRemovalAndPendingTargets) {
    SystemAllocator system;
    Fixture fixture(system);
    auto root = fixture.Make();
    TargetResolver resolver;
    fixture.options.resolver = &resolver;
    ConstructionContext transaction(fixture.objects, fixture.Access());
    auto created = transaction.Create<AssignHolder>(GeneratedType<AssignHolder>::Schema().id);
    REQUIRE(created);
    auto pending = transaction.Create<AssignTarget>(GeneratedType<AssignTarget>::Schema().id);
    REQUIRE(pending);
    resolver.target = pending.Value();
    {
        auto holder = transaction.GetForWrite<AssignHolder>(created.Value());
        REQUIRE(holder);
        REQUIRE(transaction.AssignElements(created.Value(), holder.Value()->items, 3));
    }

    const TextAssignment inputs[] = {Set("items[2].link", "/t"),
                                     Other(AssignmentKind::Erase, "items[0]", 1),
                                     Set("items[0].weight", "7", 2), Set("next", "/t", 3)};
    fixture.diagnostics.Clear();
    auto prepared = PrepareAssignments(AssignmentTarget::Staged(transaction, created.Value()),
                                       inputs, fixture.options, fixture.diagnostics);
    REQUIRE(prepared);
    REQUIRE(prepared.Value().Publish());
    {
        auto staged = transaction.Get<AssignHolder>(created.Value());
        REQUIRE(staged);
        REQUIRE(staged.Value()->items.Size() == 2);
        CHECK(staged.Value()->items[0].weight == 7 &&
              staged.Value()->items[0].link.Handle().IsNull());
        CHECK(staged.Value()->items[1].link.Handle() == pending.Value());
        CHECK(staged.Value()->next.Handle() == pending.Value());
    }

    const TextAssignment live[] = {Set("next", "/t")};
    CHECK(ErrorOf(fixture.Prepare(root.Handle(), live)) == ErrorCode::InvalidArgument);
    REQUIRE(fixture.diagnostics.Count() == 1);
    CHECK(fixture.diagnostics.Get(0).code == TextDiagnosticCode::InvalidValue);
    transaction.Rollback();
}
