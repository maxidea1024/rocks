#include "TextAssignTestSupport.h"

// 리플렉션 B9i3c1 대입 묶음 시험이다(Docs/Design/ReflectionExtensions.ko.md 10.2절, fixture
// ReflectionTestFixtures/AssignFixture.h): 앞 대입 위 해석과 종류별 적용, override 경로 정규화·
// Touches·Append, key 안 `=`의 대입 분해, 같은 경로 두 번·오류 모두 모으기, ReadOnly·Deprecated
// ClassRef 같은 값 무동작, Publish 원자성(변경 표지 Busy·owner Discard·GC·Weak 대상 Discard·할당
// 실패 주입), staged 대상의 ReadOnly 허용과 struct 값 대상.
using namespace rocks;
namespace text_assignment_tests_file {

using namespace rocks::reflection_text;
using namespace rocks::test::assign;
using reflection_fixture::AssignHolder;
using reflection_fixture::AssignKindCurrent;
using reflection_fixture::AssignKindOld;
using reflection_fixture::AssignPoint;
using reflection_fixture::AssignTarget;

// 대입을 차례로 적용하고 뒤 대입은 앞 대입이 만든 상태 위에서 해석한다. override 경로는 컨테이너·
// Atomic struct에서 자르고 값 변화 없는 대입도 넣으며, Touches는 겹침을 본다. Append는 Prepare와
// 같은 규칙이다.
TEST(TextAssign_AppliesInOrderAndNormalizesOverrides) {
    SystemAllocator system;
    Fixture fixture(system);
    auto root = fixture.Make();
    const ObjectHandle object = root.Handle();
    {
        Peek holder(fixture, object);
        REQUIRE(holder->points.TryPushBack(AssignPoint{1, 2}));
        REQUIRE(holder->points.TryPushBack(AssignPoint{3, 4}));
        holder->count = 5;
    }

    const TextAssignment inputs[] = {Set("numbers", "1,2,3", 0),
                                     Set("numbers[1]", "9", 1),
                                     Set("anchor", "{}", 2),
                                     Set("anchor.x", "4", 3),
                                     Set("byKey[k]", "{}", 4),
                                     Set("byKey[k].y", "2", 5),
                                     Set("scores", "a:1,b:2", 6),
                                     Set("scores[c]", "3", 7),
                                     Set("tags", "red,blue", 8),
                                     Set("tags[green]", "", 9),
                                     Other(AssignmentKind::Erase, "tags[red]", 10),
                                     Set("points[1]", "{}", 11),
                                     Set("points[0].x", "8", 12),
                                     Set("pair[1].y", "6", 13),
                                     Set("atom.a", "1", 14),
                                     Set("inner.point.x", "3", 15),
                                     Set("mixed.weight", "7", 16),
                                     Set("kind", "AssignKindCurrent", 17),
                                     Set("label", "hello", 18),
                                     Set("count", "5", 19),
                                     Other(AssignmentKind::Unset, "limit", 20)};
    auto prepared = fixture.Prepare(object, inputs);
    REQUIRE(prepared);
    CHECK(fixture.diagnostics.Count() == 0);
    PreparedAssignments& batch = prepared.Value();
    // 정규화: 컨테이너는 필드, Atomic struct는 struct, 그 밖은 끝 경로(중복 없음).
    const Span<const PropertyPath> overrides = batch.OverridePaths();
    CHECK(overrides.Size() == 14);
    const char* expected[] = {"numbers", "anchor",        "byKey",        "scores", "tags",
                              "points",  "pair",          "atom",         "kind",   "label",
                              "count",   "inner.point.x", "mixed.weight", "limit"};
    for (const char* text : expected) {
        CHECK(HasOverride(fixture, overrides, text));
    }

    CHECK(!HasOverride(fixture, overrides, "atom.a"));
    CHECK(Touches(fixture, batch, "atom.b"));
    CHECK(Touches(fixture, batch, "scores[z]"));
    CHECK(Touches(fixture, batch, "inner"));
    CHECK(!Touches(fixture, batch, "inner.value"));
    CHECK(!Touches(fixture, batch, "sealed"));

    // 게시 전에는 객체가 그대로다.
    {
        Peek holder(fixture, object);
        CHECK(holder->numbers.IsEmpty() && !holder->anchor.HasValue());
    }

    // Touches가 거짓인 경로를 덧붙인다(A5 방식).
    CHECK(!Touches(fixture, batch, "inner.value"));
    REQUIRE(batch.Append(Set("inner.value", "6", 22)));
    CHECK(Touches(fixture, batch, "inner.value"));
    CHECK(batch.OverridePaths().Size() == 15);

    auto published = batch.Publish();
    REQUIRE(published);
    CHECK(ErrorOf(batch.Publish()) == ErrorCode::InvalidArgument);
    // count는 값이 같아 사본을 바꾸지 않았고(게시 단위가 아니다) inner.point.x·inner.value는 단위
    // inner 하나다.
    CHECK(published.Value().UnitCount() == 13);
    CHECK(published.Value().OverridePaths().Size() == 15);
    {
        Peek holder(fixture, object);
        CHECK(holder->numbers.Size() == 3 && holder->numbers[1] == 9 && holder->numbers[2] == 3);
        CHECK(holder->anchor.HasValue() && holder->anchor.Value().x == 4);
        CHECK(holder->byKey.Count() == 1);
        CHECK(holder->scores.Count() == 3);
        CHECK(holder->tags.Count() == 2);
        CHECK(holder->points.Size() == 2 && holder->points[0].x == 8 && holder->points[1].x == 0 &&
              holder->points[1].y == 0);
        CHECK(holder->pair[1].y == 6 && holder->atom.a == 1 && holder->inner.point.x == 3);
        CHECK(holder->inner.value == 6 && holder->mixed.weight == 7);
        CHECK(holder->kind ==
              ClassRef<reflection_fixture::AssignKindBase>::Of<AssignKindCurrent>());
        CHECK(holder->label.View() == StringView("hello") && holder->count == 5);
        CHECK(!holder->limit.HasValue());
    }
}

// `[...]`·key 따옴표 안의 `=`는 대입을 나누지 않는다.
TEST(TextAssign_SplitKeepsEqualsInsideKeys) {
    SystemAllocator system;
    Fixture fixture(system);
    auto root = fixture.Make();
    TextAssignment assignment;
    REQUIRE(MakeTextAssignment("scores[\"a=b\"]=3", 4, assignment));
    CHECK(assignment.path == StringView("scores[\"a=b\"]") && assignment.value == "3");
    CHECK(assignment.sourceIndex == 4);
    CHECK(!MakeTextAssignment("count", 0, assignment));
    auto prepared = fixture.Prepare(root.Handle(), Span<const TextAssignment>(&assignment, 1));
    REQUIRE(prepared);
    REQUIRE(prepared.Value().Publish());
    Peek holder(fixture, root.Handle());
    CHECK(holder->scores.Count() == 1);
}

// 같은 경로 두 번·모르는 경로·값 오류·ReadOnly·강한 참조·값 없는 자리를 대입마다 진단으로 모두
// 모으고 아무것도 게시하지 않는다. 값이 같은 ReadOnly 대입과 Deprecated 클래스 ClassRef의 같은 값
// 재대입은 무동작이다(ReadOnly는 override 경로에도 없다).
TEST(TextAssign_CollectsErrorsAndKeepsSameValues) {
    SystemAllocator system;
    Fixture fixture(system);
    auto root = fixture.Make();
    const ObjectHandle object = root.Handle();
    const TextAssignment inputs[] = {Set("count", "1", 0),
                                     Set("count", "2", 1),
                                     Set("nothing", "1", 2),
                                     Set("numbers", "1,x", 3),
                                     Set("locked", "3", 4),
                                     Set("mixed", "{}", 5),
                                     Set("items[1]", "{}", 6),
                                     Set("pair[0]", "{}", 7),
                                     Other(AssignmentKind::Unset, "label", 8),
                                     Other(AssignmentKind::Erase, "atom", 9),
                                     Set("tags[x]", "v", 10),
                                     Set("sealed.x", "1", 11)};
    auto prepared = fixture.Prepare(object, inputs);
    CHECK(ErrorOf(prepared) == ErrorCode::InvalidArgument);
    // 참조 struct·정적 배열 원소의 `{}`는 멤버마다 나뉘어 받는다(리플렉션 B9i3c3) — 빈
    // StructRefArray의 원소 수보다 큰 index는 범위 밖이다(같으면 추가 — 리플렉션 B9i5). 값 단위의
    // ReadOnly 자리는 호출 끝에 대조하므로 그 진단은 맨 뒤에 단위 순서로 온다(리플렉션 B9i 리뷰
    // 반영).
    const TextDiagnosticCode codes[] = {TextDiagnosticCode::DuplicateAssignment,
                                        TextDiagnosticCode::UnknownProperty,
                                        TextDiagnosticCode::InvalidValue,
                                        TextDiagnosticCode::OutOfRange,
                                        TextDiagnosticCode::InvalidValue,
                                        TextDiagnosticCode::InvalidValue,
                                        TextDiagnosticCode::InvalidValue,
                                        TextDiagnosticCode::ReadOnly,
                                        TextDiagnosticCode::ReadOnly};
    const uint32 sources[] = {1, 2, 3, 6, 8, 9, 10, 4, 11};
    REQUIRE(fixture.diagnostics.Count() == 9);
    for (usize index = 0; index < 9; ++index) {
        CHECK(fixture.diagnostics.Get(index).code == codes[index]);
        CHECK(fixture.diagnostics.Get(index).source.sourceIndex == sources[index]);
    }

    {
        Peek holder(fixture, object);
        CHECK(holder->count == 0 && holder->numbers.IsEmpty());
        holder->kind = ClassRef<reflection_fixture::AssignKindBase>::Of<AssignKindOld>();
    }

    // 같은 값 무동작: ReadOnly(override 없음)·Deprecated ClassRef·보통 필드(override 있음).
    const TextAssignment same[] = {Set("locked", "0"), Set("sealed.x", "0", 1),
                                   Set("kind", "AssignKindOld", 2), Set("count", "0", 3)};
    auto kept = fixture.Prepare(object, same);
    REQUIRE(kept);
    CHECK(kept.Value().OverridePaths().Size() == 2);
    CHECK(HasOverride(fixture, kept.Value().OverridePaths(), "kind"));
    auto published = kept.Value().Publish();
    REQUIRE(published);
    CHECK(published.Value().UnitCount() == 0);

    // Deprecated 클래스로 바꾸는 대입은 거부한다(지금 값이 다르다).
    {
        Peek holder(fixture, object);
        holder->kind = ClassRef<reflection_fixture::AssignKindBase>::Of<AssignKindCurrent>();
    }

    const TextAssignment deprecated[] = {Set("kind", "AssignKindOld", 5)};
    CHECK(ErrorOf(fixture.Prepare(object, deprecated)) == ErrorCode::InvalidArgument);
    REQUIRE(fixture.diagnostics.Count() == 1);
    CHECK(fixture.diagnostics.Get(0).code == TextDiagnosticCode::InvalidValue);
}

// Publish 원자성: Prepare 뒤 owner가 바뀌면 Busy, owner가 Discard·GC 회수되면 StaleHandle, 고정한
// Weak 대상이 Discard되면 StaleHandle이고 객체는 그대로다. 앞 Append가 진단을 남긴 묶음은 게시할 수
// 없다.
TEST(TextAssign_PublishFailuresChangeNothing) {
    SystemAllocator system;
    Fixture fixture(system);
    auto root = fixture.Make();
    const ObjectHandle object = root.Handle();
    const TextAssignment inputs[] = {Set("count", "1"), Set("numbers", "4,5", 1)};

    auto busy = fixture.Prepare(object, inputs);
    REQUIRE(busy);
    FieldValue label(system);
    label.kind = ValueKind::Utf8String;
    REQUIRE(label.string.TryAssign("direct"));
    PropertyPathBuffer labelPath;
    REQUIRE(ParsePropertyPath(fixture.holder, "label", labelPath));
    REQUIRE(PathWrite(fixture.objects, fixture.Access(), object, labelPath.View(), label));
    CHECK(ErrorOf(busy.Value().Publish()) == ErrorCode::Busy);
    {
        Peek holder(fixture, object);
        CHECK(holder->count == 0 && holder->numbers.IsEmpty());
    }

    auto poisoned = fixture.Prepare(object, inputs);
    REQUIRE(poisoned);
    CHECK(ErrorOf(poisoned.Value().Append(Set("count", "2", 2))) == ErrorCode::InvalidArgument);
    CHECK(ErrorOf(poisoned.Value().Append(Set("limit", "2", 3))) == ErrorCode::InvalidArgument);
    CHECK(ErrorOf(poisoned.Value().Publish()) == ErrorCode::InvalidArgument);

    // Weak 대상: Prepare가 고정하고 Publish가 다시 확인한다.
    auto target = fixture.objects.NewObject<AssignTarget>(fixture.Access(),
                                                          GeneratedType<AssignTarget>::Schema().id);
    REQUIRE(target);
    TargetResolver resolver;
    resolver.target = target.Value().Handle();
    fixture.options.resolver = &resolver;
    const TextAssignment weak[] = {Set("peer", "/t"), Set("count", "3", 1)};
    auto pinned = fixture.Prepare(object, weak);
    REQUIRE(pinned);
    REQUIRE(fixture.objects.Discard(fixture.Access(), resolver.target));
    CHECK(ErrorOf(pinned.Value().Publish()) == ErrorCode::StaleHandle);
    {
        Peek holder(fixture, object);
        CHECK(holder->count == 0);
    }

    // owner Discard·GC 회수.
    auto discarded = fixture.Prepare(object, inputs);
    REQUIRE(discarded);
    auto other = fixture.Make();
    auto collected = fixture.Prepare(other.Handle(), inputs);
    REQUIRE(collected);
    other.Reset();
    REQUIRE(fixture.objects.Collect(fixture.Access()));
    CHECK(ErrorOf(collected.Value().Publish()) == ErrorCode::StaleHandle);
    REQUIRE(fixture.objects.Discard(fixture.Access(), object));
    CHECK(ErrorOf(discarded.Value().Publish()) == ErrorCode::StaleHandle);
}

// Prepare의 할당 실패는 호출 실패(OutOfMemory)이고 객체는 그대로다. 실패 지점을 하나씩 옮겨 모든
// 할당에서 확인한 뒤 성공한 묶음을 게시한다.
TEST(TextAssign_AllocationFailureLeavesObjectUnchanged) {
    SystemAllocator system;
    FailingAllocator failing(system);
    Fixture fixture(failing);
    auto root = fixture.Make();
    const ObjectHandle object = root.Handle();
    const TextAssignment inputs[] = {Set("label", "a long label that needs its own buffer"),
                                     Set("numbers", "1,2,3", 1), Set("anchor", "{}", 2),
                                     Set("scores", "a:1,b:2", 3), Set("inner.point.y", "5", 4)};
    uint64 failures = 0;
    for (uint64 failAt = 1;; ++failAt) {
        failing.allocations = 0;
        failing.failAt = failAt;
        auto prepared = fixture.Prepare(object, inputs);
        failing.failAt = 0;
        if (!prepared) {
            CHECK(prepared.Error() == ErrorCode::OutOfMemory);
            ++failures;
            Peek holder(fixture, object);
            CHECK(holder->label.IsEmpty() && holder->numbers.IsEmpty());
            CHECK(!holder->anchor.HasValue() && holder->scores.Count() == 0);
            continue;
        }

        REQUIRE(prepared.Value().Publish());
        break;
    }

    CHECK(failures > 5);
    Peek holder(fixture, object);
    CHECK(holder->numbers.Size() == 3 && holder->scores.Count() == 2);
    CHECK(holder->anchor.HasValue() && holder->inner.point.y == 5);
}

// staged 객체 대상은 ReadOnly 필드·멤버도 쓴다(공개 전 초기값). struct 값 대상은 그 값에 게시한다.
TEST(TextAssign_StagedAcceptsReadOnlyAndStructValues) {
    SystemAllocator system;
    Fixture fixture(system);
    {
        ConstructionContext transaction(fixture.objects, fixture.Access());
        auto created = transaction.Create<AssignHolder>(GeneratedType<AssignHolder>::Schema().id);
        REQUIRE(created);
        const TextAssignment inputs[] = {Set("locked", "4"), Set("sealed.x", "2", 1),
                                         Set("sealed", "{}", 2), Set("sealed.y", "3", 3),
                                         Set("count", "1", 4)};
        fixture.diagnostics.Clear();
        auto prepared = PrepareAssignments(AssignmentTarget::Staged(transaction, created.Value()),
                                           inputs, fixture.options, fixture.diagnostics);
        REQUIRE(prepared);
        REQUIRE(prepared.Value().Publish());
        auto staged = transaction.Get<AssignHolder>(created.Value());
        REQUIRE(staged);
        CHECK(staged.Value()->locked == 4 && staged.Value()->sealed.x == 0);
        CHECK(staged.Value()->sealed.y == 3 && staged.Value()->count == 1);
        transaction.Rollback();
    }

    auto pointType =
        fixture.types.AcquireValueType(GeneratedType<AssignPoint>::Schema().id, TypeKind::Struct);
    REQUIRE(pointType);
    AssignPoint point{1, 1};
    const TextAssignment inputs[] = {Set("x", "7"), Set("y", "1", 1)};
    fixture.diagnostics.Clear();
    auto prepared = PrepareAssignments(AssignmentTarget::Value(fixture.objects, fixture.Access(),
                                                               pointType.Value(), &point, system),
                                       inputs, fixture.options, fixture.diagnostics);
    REQUIRE(prepared);
    CHECK(prepared.Value().OverridePaths().Size() == 2);
    REQUIRE(prepared.Value().Publish());
    CHECK(point.x == 7 && point.y == 1);
}

// 참조 단위(리플렉션 B9i3c2): 값·ObjectRef·정적 배열 원소·StructRefArray 원소 멤버가 섞인 묶음을 한
// 구간에서 게시하고 관찰자 기록은 간선 변경으로 한 번에 남는다. 비우기는 옛 대상을 돌려준다.
TEST(TextAssign_ReferenceUnitsPublishWithValues) {
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
    PropertyPathBuffer itemPath;
    REQUIRE(ParsePropertyPath(fixture.holder, "items[0]", itemPath));
    REQUIRE(PathInsertElement(fixture.objects, fixture.Access(), object, itemPath.View(), nullptr));

    const TextAssignment inputs[] = {
        Set("next", "/t"),        Set("mixed.weight", "3", 1),   Set("mixed.link", "/u", 2),
        Set("slots[1]", "/t", 3), Set("items[0].link", "/u", 4), Set("items[0].weight", "4", 5),
        Set("count", "2", 6)};
    auto prepared = fixture.Prepare(object, inputs);
    REQUIRE(prepared);
    const Span<const PropertyPath> overrides = prepared.Value().OverridePaths();
    CHECK(HasOverride(fixture, overrides, "next") && HasOverride(fixture, overrides, "slots"));
    CHECK(HasOverride(fixture, overrides, "mixed.link") &&
          HasOverride(fixture, overrides, "mixed.weight"));
    CHECK(HasOverride(fixture, overrides, "items") && overrides.Size() == 6);
    auto published = prepared.Value().Publish();
    REQUIRE(published);
    CHECK(published.Value().ReferenceCount() == 4 && published.Value().UnitCount() == 3);
    for (usize index = 0; index < published.Value().ReferenceCount(); ++index) {
        CHECK(published.Value().OldReference(index).IsNull());
    }

    {
        Peek holder(fixture, object);
        CHECK(holder->next.Handle() == first.Handle() &&
              holder->slots[1].Handle() == first.Handle());
        CHECK(holder->mixed.link.Handle() == second.Handle() &&
              holder->items[0].link.Handle() == second.Handle());
        CHECK(holder->mixed.weight == 3 && holder->items[0].weight == 4 && holder->count == 2);
    }

    // (package, first)와 (package, second)가 각각 +2로 한 번에 기록된다.
    Array<ReferenceEdgeDelta> changes = fixture.Take();
    REQUIRE(changes.Size() == 2);
    for (usize index = 0; index < changes.Size(); ++index) {
        const ReferenceEdgeDelta& change = changes[index];
        CHECK(change.ownerPackage == fixture.package && change.delta == 2 &&
              change.cause == ReferenceChangeCause::Edit);
        CHECK(change.targetPackage == first.Handle() || change.targetPackage == second.Handle());
    }

    const TextAssignment clear[] = {Set("next", "none"), Set("slots[1]", "none", 1)};
    auto cleared = fixture.Prepare(object, clear);
    REQUIRE(cleared);
    auto clearedPublished = cleared.Value().Publish();
    REQUIRE(clearedPublished);
    REQUIRE(clearedPublished.Value().ReferenceCount() == 2);
    CHECK(clearedPublished.Value().OldReference(0) == first.Handle() &&
          clearedPublished.Value().OldReference(1) == first.Handle());
    changes = fixture.Take();
    REQUIRE(changes.Size() == 1);
    CHECK(changes[0].targetPackage == first.Handle() && changes[0].delta == -2);
    Peek holder(fixture, object);
    CHECK(holder->next.Handle().IsNull() && holder->slots[1].Handle().IsNull());
}

// 참조 단위의 거부와 원자성: ReadOnly·Unset·없는 StructRefArray 원소 지우기는 진단, 같은 대상은
// 무동작이다. 대상 고정은 Prepare 뒤 GC 회수를 막고, 대상이 Discard되면 Publish가 StaleHandle이고
// 객체는 그대로다.
TEST(TextAssign_ReferenceUnitFailuresChangeNothing) {
    SystemAllocator system;
    Fixture fixture(system);
    auto root = fixture.Make();
    const ObjectHandle object = root.Handle();
    auto target = fixture.MakeTarget();
    TargetResolver resolver;
    resolver.target = target.Handle();
    fixture.options.resolver = &resolver;

    const TextAssignment rejected[] = {Set("anchorLink", "/t"),
                                       Other(AssignmentKind::Unset, "next", 1),
                                       Other(AssignmentKind::Erase, "items[0]", 2)};
    CHECK(ErrorOf(fixture.Prepare(object, rejected)) == ErrorCode::InvalidArgument);
    REQUIRE(fixture.diagnostics.Count() == 3);
    CHECK(fixture.diagnostics.Get(0).code == TextDiagnosticCode::ReadOnly);
    CHECK(fixture.diagnostics.Get(1).code == TextDiagnosticCode::InvalidValue);
    CHECK(fixture.diagnostics.Get(2).code == TextDiagnosticCode::OutOfRange);

    const TextAssignment same[] = {Set("anchorLink", "none"), Set("next", "none", 1)};
    auto kept = fixture.Prepare(object, same);
    REQUIRE(kept);
    CHECK(kept.Value().OverridePaths().Size() == 1);
    auto keptPublished = kept.Value().Publish();
    REQUIRE(keptPublished);
    CHECK(keptPublished.Value().ReferenceCount() == 0);

    // 고정한 대상은 RootRef를 놓고 GC를 돌려도 남는다.
    const TextAssignment link[] = {Set("next", "/t"), Set("count", "5", 1)};
    auto pinned = fixture.Prepare(object, link);
    REQUIRE(pinned);
    target.Reset();
    REQUIRE(fixture.objects.Collect(fixture.Access()));
    REQUIRE(pinned.Value().Publish());
    {
        Peek holder(fixture, object);
        CHECK(holder->next.Handle() == resolver.target && holder->count == 5);
    }

    const TextAssignment relink[] = {Set("slots[0]", "/t"), Set("count", "6", 1)};
    auto discarded = fixture.Prepare(object, relink);
    REQUIRE(discarded);
    REQUIRE(fixture.objects.Discard(fixture.Access(), resolver.target));
    CHECK(ErrorOf(discarded.Value().Publish()) == ErrorCode::StaleHandle);
    Peek holder(fixture, object);
    CHECK(holder->slots[0].Handle().IsNull() && holder->count == 5);
}

// 참조 단위 Publish의 할당 실패(관찰자 allocator의 키 예약, Registry allocator의 게시 목록·자리
// 찾기)는 OutOfMemory이고 객체와 관찰자 기록은 그대로다. 실패 지점을 하나씩 옮긴 뒤 게시한다.
TEST(TextAssign_ReferencePublishAllocationFailuresChangeNothing) {
    SystemAllocator system;
    FailingAllocator failing(system);
    FailingAllocator observerFailing(system);
    Fixture fixture(failing, true, &observerFailing);
    auto root = fixture.Make();
    const ObjectHandle object = root.Handle();
    auto target = fixture.MakeTarget();
    TargetResolver resolver;
    resolver.target = target.Handle();
    fixture.options.resolver = &resolver;
    const TextAssignment inputs[] = {Set("next", "/t"), Set("mixed.link", "/t", 1),
                                     Set("count", "7", 2)};
    const TextAssignment clear[] = {Set("next", "none"), Set("mixed.link", "none", 1),
                                    Set("count", "0", 2)};
    FailingAllocator* const allocators[] = {&observerFailing, &failing};
    for (usize which = 0; which < 2; ++which) {
        FailingAllocator& allocator = *allocators[which];
        uint64 failures = 0;
        for (uint64 failAt = 1;; ++failAt) {
            auto prepared = fixture.Prepare(object, inputs);
            REQUIRE(prepared);
            allocator.allocations = 0;
            allocator.failAt = failAt;
            auto published = prepared.Value().Publish();
            allocator.failAt = 0;
            if (!published) {
                CHECK(published.Error() == ErrorCode::OutOfMemory);
                ++failures;
                CHECK(fixture.Take().IsEmpty());
                Peek holder(fixture, object);
                CHECK(holder->next.Handle().IsNull() && holder->count == 0);
                continue;
            }

            CHECK(fixture.Take().Size() == 1);
            break;
        }

        CHECK(failures >= 1);
        if (which == 0) {
            // 다음 차례를 위해 되돌린다.
            auto cleared = fixture.Prepare(object, clear);
            REQUIRE(cleared);
            REQUIRE(cleared.Value().Publish());
            (void)fixture.Take();
        }
    }

    Peek holder(fixture, object);
    CHECK(holder->next.Handle() == target.Handle() &&
          holder->mixed.link.Handle() == target.Handle() && holder->count == 7);
}

// 컨테이너 단위(리플렉션 B9i3c3): ObjectRefArray·ObjectRefMap의 통째·원소·항목 대입을 준비 사본에
// 차례로 적용하고(뒤 대입은 앞 대입이 만든 사본 위) 값 단위와 한 구간에서 게시한다. 관찰자 기록은
// 옛 원소·새 원소의 간선 차이로 한 번에 남고, 결과는 컨테이너마다 옛 handle을 돌려준다.
TEST(TextAssign_ContainerUnitsPublishWithValues) {
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

    const TextAssignment inputs[] = {Set("group", "/t,/u"), Set("named", "a:/t,b:/u", 1),
                                     Set("count", "3", 2)};
    auto prepared = fixture.Prepare(object, inputs);
    REQUIRE(prepared);
    CHECK(HasOverride(fixture, prepared.Value().OverridePaths(), "group") &&
          HasOverride(fixture, prepared.Value().OverridePaths(), "named"));
    auto published = prepared.Value().Publish();
    REQUIRE(published);
    CHECK(published.Value().ContainerCount() == 2 && published.Value().UnitCount() == 1);
    CHECK(published.Value().OldReferences(0).IsEmpty() &&
          published.Value().OldReferences(1).IsEmpty());
    {
        Peek holder(fixture, object);
        REQUIRE(holder->group.Size() == 2 && holder->named.Count() == 2);
        CHECK(holder->group[0] == first.Handle() && holder->group[1] == second.Handle());
        CHECK(*holder->named.Find(StringView("a")) == first.Handle() &&
              *holder->named.Find(StringView("b")) == second.Handle() && holder->count == 3);
    }

    // (package, first)와 (package, second)가 각각 +2다.
    Array<ReferenceEdgeDelta> changes = fixture.Take();
    REQUIRE(changes.Size() == 2);
    for (usize index = 0; index < changes.Size(); ++index) {
        CHECK(changes[index].delta == 2 && changes[index].cause == ReferenceChangeCause::Edit);
    }

    // 원소 쓰기 뒤 지우기, 항목 null 쓰기·지우기·넣기가 사본 위에서 이어진다.
    const TextAssignment edits[] = {
        Set("group[0]", "/u"), Other(AssignmentKind::Erase, "group[1]", 1),
        Set("named[a]", "none", 2), Other(AssignmentKind::Erase, "named[b]", 3),
        Set("named[c]", "/t", 4)};
    auto edited = fixture.Prepare(object, edits);
    REQUIRE(edited);
    auto editPublished = edited.Value().Publish();
    REQUIRE(editPublished);
    REQUIRE(editPublished.Value().ContainerCount() == 2);
    const Span<const ObjectHandle> oldGroup = editPublished.Value().OldReferences(0);
    REQUIRE(oldGroup.Size() == 2);
    CHECK(oldGroup[0] == first.Handle() && oldGroup[1] == second.Handle());
    {
        Peek holder(fixture, object);
        REQUIRE(holder->group.Size() == 1 && holder->named.Count() == 2);
        CHECK(holder->group[0] == second.Handle());
        CHECK(holder->named.Find(StringView("a"))->IsNull() &&
              *holder->named.Find(StringView("c")) == first.Handle());
    }

    // first는 group·named.a를 잃고 named.c를 얻어 -1, second는 named.b를 잃어 -1이다.
    changes = fixture.Take();
    REQUIRE(changes.Size() == 2);
    for (usize index = 0; index < changes.Size(); ++index) {
        CHECK(changes[index].delta == -1);
    }
}

// 컨테이너 단위의 거부와 원자성: 범위 밖 원소·없는 키는 진단이다. Prepare 뒤 컨테이너가 바뀌면
// Busy, 새 대상이 Discard되면 StaleHandle이고 객체는 그대로다. 고정한 대상은 GC 회수되지 않는다.
// staged 객체는 트랜잭션의 통째 지정으로 게시한다.
TEST(TextAssign_ContainerUnitFailuresChangeNothing) {
    SystemAllocator system;
    Fixture fixture(system);
    auto root = fixture.Make();
    const ObjectHandle object = root.Handle();
    auto target = fixture.MakeTarget();
    TargetResolver resolver;
    resolver.target = target.Handle();
    fixture.options.resolver = &resolver;

    const TextAssignment rejected[] = {Set("group[2]", "/t"),
                                       Other(AssignmentKind::Erase, "named[zz]", 1)};
    CHECK(ErrorOf(fixture.Prepare(object, rejected)) == ErrorCode::InvalidArgument);
    REQUIRE(fixture.diagnostics.Count() == 2);
    CHECK(fixture.diagnostics.Get(0).code == TextDiagnosticCode::OutOfRange);
    CHECK(fixture.diagnostics.Get(1).code == TextDiagnosticCode::InvalidValue);

    PropertyPathBuffer groupPath;
    REQUIRE(ParsePropertyPath(fixture.holder, "group", groupPath));
    const TextAssignment fill[] = {Set("group", "/t,/t"), Set("count", "1", 1)};
    auto busy = fixture.Prepare(object, fill);
    REQUIRE(busy);
    REQUIRE(PathClear(fixture.objects, fixture.Access(), object, groupPath.View()));
    CHECK(ErrorOf(busy.Value().Publish()) == ErrorCode::Busy);
    {
        Peek holder(fixture, object);
        CHECK(holder->group.IsEmpty() && holder->count == 0);
    }

    // 고정한 대상은 RootRef를 놓고 GC를 돌려도 남는다.
    const TextAssignment name[] = {Set("named", "k:/t")};
    auto pinned = fixture.Prepare(object, name);
    REQUIRE(pinned);
    target.Reset();
    REQUIRE(fixture.objects.Collect(fixture.Access()));
    REQUIRE(pinned.Value().Publish());
    {
        Peek holder(fixture, object);
        REQUIRE(holder->named.Count() == 1);
        CHECK(*holder->named.Find(StringView("k")) == resolver.target);
    }

    auto discarded = fixture.Prepare(object, fill);
    REQUIRE(discarded);
    REQUIRE(fixture.objects.Discard(fixture.Access(), resolver.target));
    CHECK(ErrorOf(discarded.Value().Publish()) == ErrorCode::StaleHandle);
    {
        Peek holder(fixture, object);
        CHECK(holder->group.IsEmpty() && holder->count == 0);
    }

    auto live = fixture.MakeTarget();
    resolver.target = live.Handle();
    ConstructionContext transaction(fixture.objects, fixture.Access());
    auto created = transaction.Create<AssignHolder>(GeneratedType<AssignHolder>::Schema().id);
    REQUIRE(created);
    const TextAssignment staged[] = {Set("group", "/t,none"), Set("named", "k:/t", 1),
                                     Set("named[j]", "none", 2), Set("bundle.crew", "/t", 3)};
    fixture.diagnostics.Clear();
    auto prepared = PrepareAssignments(AssignmentTarget::Staged(transaction, created.Value()),
                                       staged, fixture.options, fixture.diagnostics);
    REQUIRE(prepared);
    REQUIRE(prepared.Value().Publish());
    auto holder = transaction.Get<AssignHolder>(created.Value());
    REQUIRE(holder);
    REQUIRE(holder.Value()->group.Size() == 2 && holder.Value()->named.Count() == 2);
    CHECK(holder.Value()->group[0] == live.Handle() && holder.Value()->group[1].IsNull());
    CHECK(*holder.Value()->named.Find(StringView("k")) == live.Handle() &&
          holder.Value()->bundle.crew.Size() == 1);
    transaction.Rollback();
}

// 참조 struct·원소의 `{}`(리플렉션 B9i3c3)는 멤버마다 값 단위의 기본값·null 참조 단위·컨테이너
// 비우기로 나뉘고, 뒤 대입은 그 단위 위에서 이어진다(같은 참조 위치는 한 단위). 정적 배열의 struct
// 원소 `{}`는 멤버마다 기본값이다. override 경로는 그 struct 경로다.
TEST(TextAssign_ReferenceStructResetSplitsIntoUnits) {
    SystemAllocator system;
    Fixture fixture(system, true);
    auto root = fixture.Make();
    const ObjectHandle object = root.Handle();
    auto target = fixture.MakeTarget();
    TargetResolver resolver;
    resolver.target = target.Handle();
    fixture.options.resolver = &resolver;
    PropertyPathBuffer itemPath;
    REQUIRE(ParsePropertyPath(fixture.holder, "items[0]", itemPath));
    REQUIRE(PathInsertElement(fixture.objects, fixture.Access(), object, itemPath.View(), nullptr));

    const TextAssignment fill[] = {
        Set("bundle.level", "4"),       Set("bundle.lead", "/t", 1), Set("bundle.crew", "/t,/t", 2),
        Set("mixed.weight", "2", 3),    Set("mixed.link", "/t", 4),  Set("items[0].link", "/t", 5),
        Set("items[0].weight", "5", 6), Set("pair[0].x", "3", 7),    Set("pair[0].y", "4", 8)};
    auto filled = fixture.Prepare(object, fill);
    REQUIRE(filled);
    REQUIRE(filled.Value().Publish());
    (void)fixture.Take();

    const TextAssignment reset[] = {Set("bundle", "{}"),      Set("bundle.level", "9", 1),
                                    Set("mixed", "{}", 2),    Set("mixed.link", "/t", 3),
                                    Set("items[0]", "{}", 4), Set("pair[0]", "{}", 5)};
    auto prepared = fixture.Prepare(object, reset);
    REQUIRE(prepared);
    const Span<const PropertyPath> overrides = prepared.Value().OverridePaths();
    CHECK(HasOverride(fixture, overrides, "bundle") && HasOverride(fixture, overrides, "mixed"));
    CHECK(HasOverride(fixture, overrides, "items") && HasOverride(fixture, overrides, "pair"));
    auto published = prepared.Value().Publish();
    REQUIRE(published);
    // 참조 위치는 bundle.lead·mixed.link·items[0].link 셋이다(mixed.link는 한 단위).
    CHECK(published.Value().ReferenceCount() == 3 && published.Value().ContainerCount() == 1);
    {
        Peek holder(fixture, object);
        CHECK(holder->bundle.level == 9 && holder->bundle.lead.Handle().IsNull() &&
              holder->bundle.crew.IsEmpty());
        CHECK(holder->mixed.weight == 0 && holder->mixed.link.Handle() == target.Handle());
        CHECK(holder->items[0].weight == 0 && holder->items[0].link.Handle().IsNull());
        CHECK(holder->pair[0].x == 0 && holder->pair[0].y == 0);
    }

    // lead·crew 둘·items[0].link를 잃어 (package, target)이 -4로 한 번 기록된다.
    Array<ReferenceEdgeDelta> changes = fixture.Take();
    REQUIRE(changes.Size() == 1);
    CHECK(changes[0].targetPackage == target.Handle() && changes[0].delta == -4);
}
} // namespace text_assignment_tests_file
