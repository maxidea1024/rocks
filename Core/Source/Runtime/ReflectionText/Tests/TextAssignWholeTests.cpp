#include "TextAssignTestSupport.h"
#include "../Private/TextAssignInternal.h"

#include <Object/PathKey.h>
#include <ReflectionText/TextJson.h>

// 리플렉션 B9i 묶음 리뷰 반영 시험이다(Docs/Design/ReflectionExtensions.ko.md 10.2·10.4절 "리뷰
// 반영에서 정한 점", fixture ReflectionTestFixtures/AssignFixture.h의 AssignWholeHolder): Append
// 호출 오류 뒤 게시 거부, 통째 값의 같은 값 무동작과 ReadOnly 호출 끝 대조(export → 같은 객체
// import 왕복), MergeByKey가 준비 상태를 보는지, 같은 경로 검사의 선형 작업량과 JSON 대입 청구,
// 버린 단위의 고정이 게시 검사에서 빠지는지.
using namespace rocks;
using namespace rocks::reflection_text;
using namespace rocks::test::assign;
using reflection_fixture::AssignKindBase;
using reflection_fixture::AssignKindCurrent;
using reflection_fixture::AssignKindOld;
using reflection_fixture::AssignWholeHolder;

namespace {

using KindRef = ClassRef<AssignKindBase>;

// AssignWholeHolder를 읽기 Borrow로 들여다본다(묶음 게시 전에 놓는다).
class WholePeek {
public:
    WholePeek(Fixture& fixture, ObjectHandle object) noexcept
        : borrow_(fixture.objects.Resolve<AssignWholeHolder>(fixture.Access(), object)) {
        REQUIRE(borrow_);
    }

    AssignWholeHolder* operator->() noexcept {
        return borrow_.Value().Get().Value();
    }

private:
    Result<Borrow<AssignWholeHolder>> borrow_;
};

// staged 객체에 inputs(ReadOnly 포함)를 써서 공개한 AssignWholeHolder를 out에 둔다(실패하면 빈
// RootRef).
void MakeWhole(Fixture& fixture, Span<const TextAssignment> inputs, RootRef<Object>& out) noexcept {
    ConstructionContext transaction(fixture.objects, fixture.Access());
    auto created =
        transaction.Create<AssignWholeHolder>(GeneratedType<AssignWholeHolder>::Schema().id);
    REQUIRE(created);
    fixture.diagnostics.Clear();
    auto prepared = PrepareAssignments(AssignmentTarget::Staged(transaction, created.Value()),
                                       inputs, fixture.options, fixture.diagnostics);
    REQUIRE(prepared);
    REQUIRE(prepared.Value().Publish());
    auto committed = transaction.Commit();
    REQUIRE(committed);
    out = Move(committed.Value());
}

// target을 JSON 텍스트로 out에 쓴다.
Result<void> Export(const AssignmentTarget& target, const TextCodecOptions& options,
                    StringBuilderBase& out) noexcept {
    out.Clear();
    JsonWriter writer(out);
    if (auto written = ExportJson(target, writer, options); !written) {
        return written;
    }

    return writer.Finish();
}

// text를 읽어 target에 import한다(문서는 호출 동안만 산다).
Result<PreparedAssignments> Import(Allocator& allocator, StringView text,
                                   const AssignmentTarget& target, const TextCodecOptions& options,
                                   TextDiagnostics& diagnostics) noexcept {
    JsonDocument document(allocator);
    const bool parsed = document.Parse(text).Succeeded();
    CHECK(parsed);
    if (!parsed) {
        return Result<PreparedAssignments>::Failure(ErrorCode::InvalidArgument);
    }

    diagnostics.Clear();
    return ImportJson(document.Root(), target, options, diagnostics);
}

// override 경로에 lease 타입의 text 경로가 있는지다.
bool HasPath(const TypeLease& lease, Span<const PropertyPath> paths, const char* text) noexcept {
    PropertyPathBuffer expected;
    if (!ParsePropertyPath(lease, StringView(text), expected)) {
        return false;
    }

    for (usize index = 0; index < paths.Size(); ++index) {
        if (SamePropertyPath(paths[index], expected.View())) {
            return true;
        }
    }

    return false;
}

const TextAssignment WholeInputs[] = {Set("numbers", "1,2,3"),
                                      Set("tags", "x,y", 1),
                                      Set("scores", "a:1,b:2", 2),
                                      Set("pair", "4,5", 3),
                                      Set("anchor", "{}", 4),
                                      Set("anchor.x", "6", 5),
                                      Set("points[0]", "{}", 6),
                                      Set("points[0].y", "7", 7),
                                      Set("points[1]", "{}", 8),
                                      Set("byKey[k]", "{}", 9),
                                      Set("byKey[k].x", "8", 10),
                                      Set("shelves[0]", "{}", 11),
                                      Set("shelves[0].byKey[k]", "{}", 12),
                                      Set("shelves[0].byKey[k].x", "5", 13),
                                      Set("shelves[0].byKey[k].y", "6", 14)};

} // namespace

// Append의 호출 오류(할당 실패)도 묶음을 실패로 표시한다: 목록 대입이 비우기 뒤 넣기 도중 실패해도
// 그 묶음의 Append·Publish는 InvalidArgument이고 객체는 그대로다. 실패 지점을 하나씩 옮긴다.
TEST(TextAssign_AppendCallErrorPoisonsBatch) {
    SystemAllocator system;
    FailingAllocator failing(system);
    Fixture fixture(failing);
    auto root = fixture.Make();
    const ObjectHandle object = root.Handle();
    const TextAssignment first[] = {Set("numbers", "9")};
    auto seeded = fixture.Prepare(object, first);
    REQUIRE(seeded);
    REQUIRE(seeded.Value().Publish());
    uint64 failures = 0;
    for (uint64 failAt = 1;; ++failAt) {
        auto prepared = fixture.Prepare(object, Span<const TextAssignment>());
        REQUIRE(prepared);
        failing.allocations = 0;
        failing.failAt = failAt;
        const Result<void> appended = prepared.Value().Append(Set("numbers", "1,2,3"));
        failing.failAt = 0;
        if (appended) {
            REQUIRE(prepared.Value().Publish());
            break;
        }

        CHECK(appended.Error() == ErrorCode::OutOfMemory);
        ++failures;
        CHECK(ErrorOf(prepared.Value().Append(Set("count", "1", 1))) == ErrorCode::InvalidArgument);
        CHECK(ErrorOf(prepared.Value().Publish()) == ErrorCode::InvalidArgument);
        Peek holder(fixture, object);
        CHECK(holder->numbers.Size() == 1 && holder->numbers[0] == 9 && holder->count == 0);
    }

    CHECK(failures > 2);
    Peek holder(fixture, object);
    CHECK(holder->numbers.Size() == 3 && holder->numbers[2] == 3);
}

// 통째 값의 같은 값은 무동작이다: ReadOnly 목록·항목 목록·정적 배열·unset Optional은 override
// 경로에도 없고, Deprecated 클래스를 담은 ClassRef 목록·Set·Map은 같은 값 재대입을 받는다.
// ReadOnly 자리는 호출 끝에 대조하므로 `anchor={}` 뒤 `anchor.x=6`처럼 지금 값을 다시 만드는
// 대입들도 무동작이고, 다르면 대입마다 ReadOnly 진단이다.
TEST(TextAssign_WholeValuesKeepSameValues) {
    SystemAllocator system;
    Fixture fixture(system);
    RootRef<Object> root;
    MakeWhole(fixture, WholeInputs, root);
    REQUIRE(!root.Handle().IsNull());
    const ObjectHandle object = root.Handle();
    {
        WholePeek holder(fixture, object);
        REQUIRE(holder->kinds.TryPushBack(KindRef::Of<AssignKindOld>()));
        REQUIRE(holder->kinds.TryPushBack(KindRef::Of<AssignKindCurrent>()));
        REQUIRE(holder->kindSet.Add(KindRef::Of<AssignKindOld>()));
        String key(system);
        REQUIRE(key.TryAssign("a"));
        REQUIRE(holder->kindMap.Insert(Move(key), KindRef::Of<AssignKindOld>()));
    }

    auto lease = fixture.types.Acquire(GeneratedType<AssignWholeHolder>::Schema().id);
    REQUIRE(lease);
    const TextAssignment same[] = {Set("numbers", "1,2,3"),
                                   Set("tags", "x,y", 1),
                                   Set("scores", "a:1,b:2", 2),
                                   Set("pair", "4,5", 3),
                                   Other(AssignmentKind::Unset, "limit", 4),
                                   Set("anchor", "{}", 5),
                                   Set("anchor.x", "6", 6),
                                   Set("points", "", 7),
                                   Set("points[0]", "{}", 8),
                                   Set("points[0].y", "7", 9),
                                   Set("points[1]", "{}", 10),
                                   Set("kinds", "AssignKindOld,AssignKindCurrent", 11),
                                   Set("kindSet", "AssignKindOld", 12),
                                   Set("kindMap", "a:AssignKindOld", 13)};
    auto kept = fixture.Prepare(object, same);
    REQUIRE(kept);
    const Span<const PropertyPath> overrides = kept.Value().OverridePaths();
    CHECK(overrides.Size() == 3);
    CHECK(HasPath(lease.Value(), overrides, "kinds") &&
          HasPath(lease.Value(), overrides, "kindSet"));
    CHECK(HasPath(lease.Value(), overrides, "kindMap"));
    auto published = kept.Value().Publish();
    REQUIRE(published);
    CHECK(published.Value().UnitCount() == 0);

    // 다른 값: 순서가 다른 목록, 다른 Optional 멤버, 줄어든 struct 배열, 다른 Deprecated 값.
    const TextAssignment changed[] = {Set("numbers", "3,2,1"), Set("anchor", "{}", 1),
                                      Set("points", "", 2),    Set("points[0]", "{}", 3),
                                      Set("tags", "y,x", 4),   Set("kinds", "AssignKindOld", 5)};
    CHECK(ErrorOf(fixture.Prepare(object, changed)) == ErrorCode::InvalidArgument);
    REQUIRE(fixture.diagnostics.Count() == 5);
    CHECK(fixture.diagnostics.Get(0).code == TextDiagnosticCode::InvalidValue);
    CHECK(fixture.diagnostics.Get(0).source.sourceIndex == 5);
    const uint32 sources[] = {0, 1, 2, 4};
    for (usize index = 0; index < 4; ++index) {
        CHECK(fixture.diagnostics.Get(index + 1).code == TextDiagnosticCode::ReadOnly);
        CHECK(fixture.diagnostics.Get(index + 1).source.sourceIndex == sources[index]);
    }

    // Append도 호출마다 대조한다: `anchor={}` 하나만 덧붙이면 그 Append에서 다르다.
    auto appended = fixture.Prepare(object, Span<const TextAssignment>());
    REQUIRE(appended);
    CHECK(ErrorOf(appended.Value().Append(Set("anchor", "{}"))) == ErrorCode::InvalidArgument);
    WholePeek holder(fixture, object);
    CHECK(holder->numbers.Size() == 3 && holder->numbers[0] == 1);
    CHECK(holder->anchor.HasValue() && holder->anchor.Value().x == 6);
    CHECK(holder->points.Size() == 2 && holder->points[0].y == 7);
}

// ReadOnly 통째 값과 Deprecated ClassRef 컨테이너를 export한 JSON을 같은 객체에 import하면
// 무동작으로 게시된다(ReadOnly는 override 경로에 없다). ReadOnly 값을 바꾸는 JSON은 ReadOnly
// 진단이다.
TEST(TextJson_ReadOnlyWholeValuesRoundTrip) {
    SystemAllocator system;
    Fixture fixture(system);
    RootRef<Object> root;
    MakeWhole(fixture, WholeInputs, root);
    REQUIRE(!root.Handle().IsNull());
    const ObjectHandle object = root.Handle();
    {
        WholePeek holder(fixture, object);
        REQUIRE(holder->kinds.TryPushBack(KindRef::Of<AssignKindOld>()));
        REQUIRE(holder->kindSet.Add(KindRef::Of<AssignKindOld>()));
    }

    auto lease = fixture.types.Acquire(GeneratedType<AssignWholeHolder>::Schema().id);
    REQUIRE(lease);
    const AssignmentTarget live = AssignmentTarget::Live(fixture.objects, fixture.Access(), object);
    StringBuilder<1024> text(system);
    REQUIRE(Export(live, fixture.options, text));
    auto prepared = Import(system, text.View(), live, fixture.options, fixture.diagnostics);
    REQUIRE(prepared);
    const Span<const PropertyPath> overrides = prepared.Value().OverridePaths();
    const char* const sealed[] = {"numbers", "tags",   "scores", "pair",
                                  "limit",   "anchor", "points", "byKey"};
    for (const char* path : sealed) {
        CHECK(!HasPath(lease.Value(), overrides, path));
    }

    CHECK(HasPath(lease.Value(), overrides, "kinds") &&
          HasPath(lease.Value(), overrides, "shelves"));
    REQUIRE(prepared.Value().Publish());
    StringBuilder<1024> again(system);
    REQUIRE(Export(live, fixture.options, again));
    CHECK(again.View() == text.View());

    const char* changed = R"({"anchor": {"x": 1}, "byKey": {"k": {"x": 9}}, "limit": 3})";
    CHECK(ErrorOf(Import(system, changed, live, fixture.options, fixture.diagnostics)) ==
          ErrorCode::InvalidArgument);
    REQUIRE(fixture.diagnostics.Count() == 3);
    for (usize index = 0; index < 3; ++index) {
        CHECK(fixture.diagnostics.Get(index).code == TextDiagnosticCode::ReadOnly);
    }
}

// MergeByKey는 키가 있는지를 준비 상태로 본다: live 배열이 비어 있거나(같은 import가 원소를 만든다)
// Optional이 unset이어도 호출 오류가 아니고, 같은 import가 바깥 struct를 리셋해 상자에서 사라진
// 키는 없던 항목(기본값 뒤 멤버 병합)이다.
TEST(TextJson_MergeByKeySeesPreparedState) {
    SystemAllocator system;
    Fixture fixture(system);
    const TextAssignment none[] = {Set("numbers", "")};
    RootRef<Object> root;
    MakeWhole(fixture, Span<const TextAssignment>(none, 1), root);
    REQUIRE(!root.Handle().IsNull());
    const ObjectHandle object = root.Handle();
    const AssignmentTarget live = AssignmentTarget::Live(fixture.objects, fixture.Access(), object);
    fixture.options.mapMerge = MapMergeMode::MergeByKey;
    const char* fresh = R"({"shelves": [{"byKey": {"k": {"x": 1}}}],
                            "spare": {"byKey": {"k": {"y": 2}}}})";
    auto prepared = Import(system, fresh, live, fixture.options, fixture.diagnostics);
    REQUIRE(prepared);
    REQUIRE(prepared.Value().Publish());
    {
        WholePeek holder(fixture, object);
        REQUIRE(holder->shelves.Size() == 1 && holder->spare.HasValue());
        const auto* shelf = holder->shelves[0].byKey.Find(StringView("k"));
        const auto* spare = holder->spare.Value().byKey.Find(StringView("k"));
        REQUIRE(shelf && spare);
        CHECK(shelf->x == 1 && spare->y == 2);
    }

    // live에는 키가 있지만(x 1) 같은 import의 원소 리셋으로 상자에서 사라졌다: 기본값 뒤 y만 병합.
    const char* reset = R"({"shelves": [{"byKey": {"k": {"y": 4}}}]})";
    auto merged = Import(system, reset, live, fixture.options, fixture.diagnostics);
    REQUIRE(merged);
    REQUIRE(merged.Value().Publish());
    WholePeek holder(fixture, object);
    const auto* shelf = holder->shelves[0].byKey.Find(StringView("k"));
    REQUIRE(shelf);
    CHECK(shelf->x == 0 && shelf->y == 4);
}

// 같은 경로 검사는 해시 색인이다: 서로 다른 경로 N개를 넣는 비교 수는 0이고(고정 seed — 결정적),
// 같은 경로는 비교 한 번으로 찾는다. Name key는 Core Name 규칙(대소문자 무시·None 동치)으로 같다.
TEST(TextAssign_SamePathCheckIsLinear) {
    SystemAllocator system;
    reflection_text::detail::PathSet set(system, HashSeed{0x1234, 0x5678});
    constexpr uint32 Count = 4096;
    PathStep step;
    step.fieldId = Guid{1, 2};
    step.selector = PathSelector::Index;
    for (uint32 index = 0; index < Count; ++index) {
        step.index = index;
        auto added = set.Add(PropertyPath{Span<const PathStep>(&step, 1), {}});
        REQUIRE(added && added.Value());
    }

    CHECK(set.Comparisons() == 0 && set.Count() == Count);
    for (uint32 index = 0; index < Count; ++index) {
        step.index = index;
        auto added = set.Add(PropertyPath{Span<const PathStep>(&step, 1), {}});
        REQUIRE(added && !added.Value());
    }

    CHECK(set.Comparisons() == Count);
    const char* const names[] = {"Hero_2", "HERO_2", "None", ""};
    const bool fresh[] = {true, false, true, false};
    step.selector = PathSelector::Key;
    step.index = 0;
    for (usize index = 0; index < 4; ++index) {
        PathKeyValue value;
        value.kind = ValueKind::Name;
        value.text = StringView(names[index]);
        Array<uint8> wire(system);
        REQUIRE(AppendPathKey(value, wire));
        const PathKey key{ValueKind::Name, wire.View()};
        auto added =
            set.Add(PropertyPath{Span<const PathStep>(&step, 1), Span<const PathKey>(&key, 1)});
        REQUIRE(added);
        CHECK(added.Value() == fresh[index]);
    }

    // JSON import는 대입마다 1 byte를 청구한다: 빈 struct 원소 100개(입력 약 300 byte)는 한도 64
    // byte를 넘는다.
    Fixture fixture(system);
    auto root = fixture.Make();
    const AssignmentTarget live =
        AssignmentTarget::Live(fixture.objects, fixture.Access(), root.Handle());
    StringBuilder<512> json(system);
    REQUIRE(json.TryAppend("{\"points\":["));
    for (usize index = 0; index < 100; ++index) {
        REQUIRE(json.TryAppend(index == 0 ? "{}" : ",{}"));
    }

    REQUIRE(json.TryAppend("]}"));
    fixture.options.maxInputBytes = 64;
    CHECK(ErrorOf(Import(system, json.View(), live, fixture.options, fixture.diagnostics)) ==
          ErrorCode::CapacityExceeded);
    fixture.options.maxInputBytes = 256;
    CHECK(Import(system, json.View(), live, fixture.options, fixture.diagnostics));
}

// 버린 단위의 고정은 게시 검사에서 빠진다: `bundle.lead=/u` 뒤 `bundle={}`가 lead를 null로 되돌리면
// /u 고정은 놓이고, `bundle.crew=/t` 뒤 `bundle={}`가 crew를 비우면 컨테이너 고정은 검사하지 않는다
// — 두 대상이 Discard돼도 게시된다.
TEST(TextAssign_DiscardedUnitPinsLeaveChecks) {
    SystemAllocator system;
    Fixture fixture(system);
    auto root = fixture.Make();
    const ObjectHandle object = root.Handle();
    auto first = fixture.MakeTarget();
    auto second = fixture.MakeTarget();
    TargetResolver resolver;
    resolver.target = first.Handle();
    resolver.second = second.Handle();
    fixture.options.resolver = &resolver;
    const TextAssignment inputs[] = {Set("bundle.lead", "/u"), Set("bundle.crew", "/t", 1),
                                     Set("bundle", "{}", 2), Set("count", "1", 3)};
    auto prepared = fixture.Prepare(object, inputs);
    REQUIRE(prepared);
    REQUIRE(fixture.objects.Discard(fixture.Access(), resolver.target));
    REQUIRE(fixture.objects.Discard(fixture.Access(), resolver.second));
    REQUIRE(prepared.Value().Publish());
    Peek holder(fixture, object);
    CHECK(holder->count == 1 && holder->bundle.crew.Size() == 0);
}