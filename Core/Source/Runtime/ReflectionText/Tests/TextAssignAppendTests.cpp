#include "TextAssignTestSupport.h"

// 리플렉션 B9i5 대입 묶음 시험이다(Docs/Design/ReflectionExtensions.ko.md 10.2절 "구현(B9i5)에서
// 정한 점", fixture ReflectionTestFixtures/AssignFixture.h): StructRefArray 원소
// 추가(`items[i]={}`의 i = 지금 원소 수)와 추가 원소 안 값·참조 대입의 게시·관찰자 기록, 같은 묶음
// 안 지우기와 섞인 index 이동, 할당 실패 주입의 원자성, staged 대상의 원소 추가, 컨테이너 게시가
// 그대로 남는 원소(Discard된 대상)를 다시 확인하지 않는 것(B9i 리뷰 L1).
using namespace rocks;
using namespace rocks::reflection_text;
using namespace rocks::test::assign;
using reflection_fixture::AssignHolder;
using reflection_fixture::AssignNestHolder;
using reflection_fixture::AssignTarget;

namespace {

// items 끝에 기본 원소 count개를 넣는다(live 경로 연산).
void InsertItems(Fixture& fixture, ObjectHandle object, uint32 count) noexcept {
    PropertyPathBuffer parsed;
    REQUIRE(ParsePropertyPath(fixture.holder, StringView("items[0]"), parsed));
    PathStep step = parsed.View().steps[0];
    PathStep whole = step;
    whole.selector = PathSelector::None;
    whole.index = 0;
    auto counted = PathCount(fixture.objects, fixture.Access(), object,
                             PropertyPath{Span<const PathStep>(&whole, 1), {}});
    REQUIRE(counted);
    for (uint32 index = 0; index < count; ++index) {
        step.index = static_cast<uint32>(counted.Value() + index);
        REQUIRE(PathInsertElement(fixture.objects, fixture.Access(), object,
                                  PropertyPath{Span<const PathStep>(&step, 1), {}}, nullptr));
    }
}

void Publish(Fixture& fixture, ObjectHandle object, Span<const TextAssignment> inputs) noexcept {
    auto prepared = fixture.Prepare(object, inputs);
    REQUIRE(prepared);
    REQUIRE(prepared.Value().Publish());
}

// changes에서 target의 기록 변화량이다(없으면 0).
int64 DeltaOf(const Array<ReferenceEdgeDelta>& changes, ObjectHandle target) noexcept {
    int64 delta = 0;
    for (usize index = 0; index < changes.Size(); ++index) {
        if (changes[index].targetPackage == target) {
            delta += changes[index].delta;
        }
    }

    return delta;
}

} // namespace

// 원소 추가: `items[i]={}`(i = 지금 원소 수)가 끝에 기본 원소를 더하고, 그 원소 안 값·참조 대입은
// 게시 전 블록에 적용된다. 게시는 원소와 참조 간선을 한 번에 넣고 관찰자에 대상마다 한 번 기록한다.
// i가 지금 원소 수보다 크면 OutOfRange, 더한 원소의 `{}` 두 번·지우기는 DuplicateAssignment, 원소
// 아닌 값은 InvalidValue다.
TEST(TextAssign_ElementAppendPublishesAndRecordsObserver) {
    SystemAllocator system;
    Fixture fixture(system, true);
    auto root = fixture.Make();
    const ObjectHandle object = root.Handle();
    auto first = fixture.MakeTarget();
    auto second = fixture.MakeTarget();
    TargetResolver resolver;
    resolver.target = first.Handle();
    resolver.second = second.Handle();
    fixture.options.resolver = &resolver;
    InsertItems(fixture, object, 1);
    (void)fixture.Take();

    const TextAssignment inputs[] = {Set("items[1]", "{}"),
                                     Set("items[1].weight", "5", 1),
                                     Set("items[1].note", "added", 2),
                                     Set("items[1].link", "/u", 3),
                                     Set("items[2]", "{}", 4),
                                     Set("items[2].link", "/t", 5),
                                     Set("items[0].weight", "7", 6)};
    auto prepared = fixture.Prepare(object, inputs);
    REQUIRE(prepared);
    CHECK(HasOverride(fixture, prepared.Value().OverridePaths(), "items"));
    {
        // 게시 전에는 객체가 그대로다.
        Peek holder(fixture, object);
        CHECK(holder->items.Size() == 1);
    }

    REQUIRE(prepared.Value().Publish());
    {
        Peek holder(fixture, object);
        REQUIRE(holder->items.Size() == 3);
        CHECK(holder->items[0].weight == 7 && holder->items[0].link.Handle().IsNull());
        CHECK(holder->items[1].weight == 5 && holder->items[1].note.View() == StringView("added"));
        CHECK(holder->items[1].link.Handle() == second.Handle());
        CHECK(holder->items[2].weight == 0 && holder->items[2].link.Handle() == first.Handle());
    }

    Array<ReferenceEdgeDelta> changes = fixture.Take();
    CHECK(changes.Size() == 2);
    CHECK(DeltaOf(changes, first.Handle()) == 1 && DeltaOf(changes, second.Handle()) == 1);

    // 더한 원소의 지우기는 그 추가와 같은 경로라 DuplicateAssignment다(같은 묶음에서 버리려면
    // 비우기 — 아래 시험).
    const TextAssignment dropped[] = {Set("items[3]", "{}"),
                                      Other(AssignmentKind::Erase, "items[3]", 1)};
    CHECK(ErrorOf(fixture.Prepare(object, dropped)) == ErrorCode::InvalidArgument);
    REQUIRE(fixture.diagnostics.Count() == 1);
    CHECK(fixture.diagnostics.Get(0).code == TextDiagnosticCode::DuplicateAssignment);

    const TextAssignment beyond[] = {Set("items[4]", "{}")};
    CHECK(ErrorOf(fixture.Prepare(object, beyond)) == ErrorCode::InvalidArgument);
    REQUIRE(fixture.diagnostics.Count() == 1);
    CHECK(fixture.diagnostics.Get(0).code == TextDiagnosticCode::OutOfRange);

    const TextAssignment twice[] = {Set("items[3]", "{}"), Set("items[3]", "{}", 1)};
    CHECK(ErrorOf(fixture.Prepare(object, twice)) == ErrorCode::InvalidArgument);
    REQUIRE(fixture.diagnostics.Count() == 1);
    CHECK(fixture.diagnostics.Get(0).code == TextDiagnosticCode::DuplicateAssignment);

    const TextAssignment value[] = {Set("items[3]", "5")};
    CHECK(ErrorOf(fixture.Prepare(object, value)) == ErrorCode::InvalidArgument);
    REQUIRE(fixture.diagnostics.Count() == 1);
    CHECK(fixture.diagnostics.Get(0).code == TextDiagnosticCode::InvalidValue);
}

// 지우기와 추가가 섞인 index 이동: 지금 index는 앞 대입이 지우고 더한 뒤의 자리다. 지운 원래
// 원소의 참조는 {참조, null}, 비우기로 버린 추가 원소의 참조는 기록되지 않는다.
TEST(TextAssign_ElementAppendMixesWithRemovals) {
    SystemAllocator system;
    Fixture fixture(system, true);
    auto root = fixture.Make();
    const ObjectHandle object = root.Handle();
    auto target = fixture.MakeTarget();
    TargetResolver resolver;
    resolver.target = target.Handle();
    fixture.options.resolver = &resolver;
    InsertItems(fixture, object, 3);
    const TextAssignment fill[] = {Set("items[0].link", "/t"), Set("items[1].link", "/t", 1),
                                   Set("items[2].weight", "3", 2)};
    Publish(fixture, object, fill);
    (void)fixture.Take();

    // 원래 [0 1 2] → items[0] 지우기 [1 2] → 추가 A [1 2 A] → items[0] 지우기 [2 A] → 추가 C
    // [2 A C].
    const TextAssignment edit[] = {Other(AssignmentKind::Erase, "items[0]"),
                                   Set("items[2]", "{}", 1),
                                   Set("items[2].weight", "40", 2),
                                   Other(AssignmentKind::Erase, "items[0]", 3),
                                   Set("items[1].note", "a", 4),
                                   Set("items[2]", "{}", 5),
                                   Set("items[2].weight", "43", 6),
                                   Set("items[0].weight", "30", 7)};
    auto prepared = fixture.Prepare(object, edit);
    REQUIRE(prepared);
    REQUIRE(prepared.Value().Publish());
    {
        Peek holder(fixture, object);
        REQUIRE(holder->items.Size() == 3);
        CHECK(holder->items[0].weight == 30 && holder->items[0].link.Handle().IsNull());
        CHECK(holder->items[1].weight == 40 && holder->items[1].note.View() == StringView("a"));
        CHECK(holder->items[2].weight == 43 && holder->items[2].link.Handle().IsNull());
    }

    Array<ReferenceEdgeDelta> changes = fixture.Take();
    REQUIRE(changes.Size() == 1);
    CHECK(changes[0].targetPackage == target.Handle() && changes[0].delta == -2);

    // 비우기는 앞서 더한 원소도 버린다(그 원소 안 참조는 기록되지 않는다). 비운 뒤 index 0은 다시
    // 추가다.
    const TextAssignment refill[] = {Set("items[3]", "{}"), Set("items[3].link", "/t", 1),
                                     Set("items", "", 2), Set("items[0]", "{}", 3),
                                     Set("items[0].weight", "1", 4)};
    Publish(fixture, object, refill);
    {
        Peek holder(fixture, object);
        REQUIRE(holder->items.Size() == 1);
        CHECK(holder->items[0].weight == 1 && holder->items[0].link.Handle().IsNull());
    }

    CHECK(fixture.Take().IsEmpty());
}

// 할당 실패 주입: 추가 원소 블록(Prepare)·게시 직전의 참조 목록과 새 색인(Publish) 어디서 실패해도
// 오류는 OutOfMemory이고 배열과 관찰자 기록이 그대로다. 실패 지점을 하나씩 옮긴 뒤 게시한다.
TEST(TextAssign_ElementAppendFailuresChangeNothing) {
    SystemAllocator system;
    FailingAllocator failing(system);
    Fixture fixture(failing, true);
    auto root = fixture.Make();
    const ObjectHandle object = root.Handle();
    auto target = fixture.MakeTarget();
    TargetResolver resolver;
    resolver.target = target.Handle();
    fixture.options.resolver = &resolver;
    // 원소 4개 — 추가하면 색인 용량을 넘는다.
    InsertItems(fixture, object, 4);
    (void)fixture.Take();

    const TextAssignment inputs[] = {Other(AssignmentKind::Erase, "items[1]"),
                                     Set("items[3]", "{}", 1), Set("items[3].link", "/t", 2),
                                     Set("items[4]", "{}", 3), Set("items[4].weight", "6", 4)};
    uint64 failures = 0;
    for (uint64 failAt = 1;; ++failAt) {
        failing.allocations = 0;
        failing.failAt = failAt;
        auto prepared = fixture.Prepare(object, inputs);
        Result<PublishedAssignments> published =
            prepared ? prepared.Value().Publish()
                     : Result<PublishedAssignments>::Failure(prepared.Error());
        failing.failAt = 0;
        if (!published) {
            CHECK(published.Error() == ErrorCode::OutOfMemory);
            ++failures;
            CHECK(fixture.Take().IsEmpty());
            Peek holder(fixture, object);
            CHECK(holder->items.Size() == 4);
            continue;
        }

        break;
    }

    CHECK(failures >= 3);
    {
        Peek holder(fixture, object);
        REQUIRE(holder->items.Size() == 5);
        CHECK(holder->items[3].link.Handle() == target.Handle());
        CHECK(holder->items[4].weight == 6);
    }

    Array<ReferenceEdgeDelta> changes = fixture.Take();
    REQUIRE(changes.Size() == 1);
    CHECK(changes[0].delta == 1);
}

// staged 대상: 원소 추가는 트랜잭션 안 색인 다시 짜기이고 추가 원소의 참조는 같은 트랜잭션의 공개
// 전 객체도 가리킬 수 있다(Commit이 trace로 검증한다).
TEST(TextAssign_StagedElementAppend) {
    SystemAllocator system;
    Fixture fixture(system);
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
        REQUIRE(transaction.AssignElements(created.Value(), holder.Value()->items, 1));
    }

    const TextAssignment inputs[] = {Set("items[1]", "{}"), Set("items[1].link", "/t", 1),
                                     Set("items[1].weight", "9", 2),
                                     Other(AssignmentKind::Erase, "items[0]", 3)};
    fixture.diagnostics.Clear();
    auto prepared = PrepareAssignments(AssignmentTarget::Staged(transaction, created.Value()),
                                       inputs, fixture.options, fixture.diagnostics);
    REQUIRE(prepared);
    REQUIRE(prepared.Value().Publish());
    {
        auto staged = transaction.Get<AssignHolder>(created.Value());
        REQUIRE(staged);
        REQUIRE(staged.Value()->items.Size() == 1);
        CHECK(staged.Value()->items[0].weight == 9);
        CHECK(staged.Value()->items[0].link.Handle() == pending.Value());
    }

    auto committed = transaction.Commit();
    REQUIRE(committed);
    auto holder = fixture.objects.Resolve<AssignHolder>(fixture.Access(), created.Value());
    REQUIRE(holder);
    REQUIRE(holder.Value().Get().Value()->items.Size() == 1);
    CHECK(holder.Value().Get().Value()->items[0].link.Handle() == pending.Value());
}

// 컨테이너 게시는 새로 들어오는 대상만 다시 확인한다(B9i 리뷰 L1): ObjectRefArray·ObjectRefMap에
// Discard된 대상이 남아 있어도 다른 원소·항목만 바꾸는 게시와 StructRefArray 원소 추가는 성공하고,
// 남는 간선은 관찰자에 기록되지 않는다.
TEST(TextAssign_ContainerPublishSkipsRetainedTargets) {
    SystemAllocator system;
    Fixture fixture(system, true);
    auto root = fixture.Make();
    const ObjectHandle object = root.Handle();
    auto first = fixture.MakeTarget();
    auto second = fixture.MakeTarget();
    TargetResolver resolver;
    resolver.target = first.Handle();
    resolver.second = second.Handle();
    fixture.options.resolver = &resolver;
    InsertItems(fixture, object, 1);
    const TextAssignment fill[] = {Set("group", "/t,/u"), Set("named", "a:/t,b:/u", 1),
                                   Set("items[0].link", "/t", 2)};
    Publish(fixture, object, fill);
    (void)fixture.Take();
    REQUIRE(fixture.objects.Discard(fixture.Access(), first.Handle()));
    (void)fixture.Take();

    const TextAssignment edit[] = {Set("group[1]", "none"), Set("named[b]", "none", 1),
                                   Set("items[1]", "{}", 2), Set("items[1].link", "/u", 3)};
    auto prepared = fixture.Prepare(object, edit);
    REQUIRE(prepared);
    REQUIRE(prepared.Value().Publish());
    {
        Peek holder(fixture, object);
        REQUIRE(holder->group.Size() == 2 && holder->named.Count() == 2);
        CHECK(holder->group[0] == first.Handle() && holder->group[1].IsNull());
        CHECK(*holder->named.Find(StringView("a")) == first.Handle());
        CHECK(holder->named.Find(StringView("b"))->IsNull());
        REQUIRE(holder->items.Size() == 2);
        CHECK(holder->items[1].link.Handle() == second.Handle());
    }

    // group·named에서 빠진 second 둘과 새 원소의 second 하나: -1이 한 번 기록된다.
    Array<ReferenceEdgeDelta> changes = fixture.Take();
    REQUIRE(changes.Size() == 1);
    CHECK(changes[0].targetPackage == second.Handle() && changes[0].delta == -1);
}

// 추가된 StructRefArray 원소 안의 참조 struct 멤버(AssignNest::mixed)도 `{}`로 기본값을 쓰고 그
// 멤버를 대입할 수 있다(B9j–B9m 묶음 리뷰의 B9i5 우려 확인).
TEST(TextAssign_AddedElementNestedReferenceStructReset) {
    SystemAllocator system;
    Fixture fixture(system);
    auto created = fixture.objects.NewObject<AssignNestHolder>(
        fixture.Access(), GeneratedType<AssignNestHolder>::Schema().id);
    REQUIRE(created);
    RootRef<AssignNestHolder> root = Move(created.Value());
    const TextAssignment inputs[] = {Set("nests[0]", "{}"), Set("nests[0].n", "4", 1),
                                     Set("nests[0].mixed", "{}", 2),
                                     Set("nests[0].mixed.weight", "9", 3)};
    auto prepared =
        PrepareAssignments(AssignmentTarget::Live(fixture.objects, fixture.Access(), root.Handle()),
                           inputs, fixture.options, fixture.diagnostics);
    for (usize index = 0; index < fixture.diagnostics.Count(); ++index) {
        CHECK(fixture.diagnostics.Get(index).code != TextDiagnosticCode::Unsupported);
    }

    REQUIRE(prepared);
    REQUIRE(prepared.Value().Publish());
    auto borrowed = fixture.objects.Resolve<AssignNestHolder>(fixture.Access(), root.Handle());
    REQUIRE(borrowed);
    auto value = borrowed.Value().Get();
    REQUIRE(value);
    REQUIRE(value.Value()->nests.Size() == 1);
    CHECK(value.Value()->nests[0].n == 4);
    CHECK(value.Value()->nests[0].mixed.weight == 9);
}
