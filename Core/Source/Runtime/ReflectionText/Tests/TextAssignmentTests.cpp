#include <ReflectionTestFixtures/AssignFixture.generated.h>
#include <ReflectionTestFixtures/ReflectionTestFixtures.module.generated.h>
#include <Core/Memory/SystemAllocator.h>
#include <Meta/GeneratedType.h>
#include <Object/ConstructionContext.h>
#include <Object/ModuleRegistration.h>
#include <ReflectionText/TextAssignments.h>
#include <TestSupport/Test.h>

// 리플렉션 B9i3c1 대입 묶음 시험이다(Docs/Design/ReflectionExtensions.ko.md 10.2절, fixture
// ReflectionTestFixtures/AssignFixture.h): 앞 대입 위 해석과 종류별 적용, override 경로 정규화·
// Touches·Append, key 안 `=`의 대입 분해, 같은 경로 두 번·오류 모두 모으기, ReadOnly·Deprecated
// ClassRef 같은 값 무동작, Publish 원자성(변경 표지 Busy·owner Discard·GC·Weak 대상 Discard·할당
// 실패 주입), staged 대상의 ReadOnly 허용과 struct 값 대상.
using namespace rocks;
using namespace rocks::reflection_text;
using reflection_fixture::AssignHolder;
using reflection_fixture::AssignKindCurrent;
using reflection_fixture::AssignKindOld;
using reflection_fixture::AssignPoint;
using reflection_fixture::AssignTarget;

namespace {

ErrorCode ErrorOf(const Result<void>& result) noexcept {
    return result ? ErrorCode::None : result.Error();
}

template <class T>
ErrorCode ErrorOf(const Result<T>& result) noexcept {
    return result ? ErrorCode::None : result.Error();
}

// 할당을 세고 failAt번째 할당을 실패시키는 allocator다.
class FailingAllocator final : public Allocator {
public:
    explicit FailingAllocator(Allocator& inner) noexcept : inner_(inner) {
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        ++allocations;
        if (failAt != 0 && allocations == failAt) {
            return Result<void*>::Failure(ErrorCode::OutOfMemory);
        }

        return inner_.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        return inner_.Deallocate(address);
    }

    uint64 allocations = 0;
    uint64 failAt = 0;

private:
    Allocator& inner_;
};

// 논리 경로 `/t`를 target으로 푸는 resolver다(Weak 대입).
class TargetResolver final : public TextReferenceResolver {
public:
    Result<void> Resolve(StringView text, TypeId, TextReferenceKind,
                         FieldValue& out) noexcept override {
        if (text != "/t") {
            return Result<void>::Failure(ErrorCode::NotFound);
        }

        out.reference = target;
        return Result<void>::Success();
    }

    Result<void> Format(TextReferenceKind, const FieldValue&,
                        StringBuilderBase&) noexcept override {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    ObjectHandle target{};
};

// 모듈 등록·객체 Registry·AssignHolder lease·진단 모음이다(소멸: lease → Registry → 등록).
struct Fixture {
    explicit Fixture(Allocator& objectAllocator) noexcept
        : types(system), registration(types, system), objects(objectAllocator, types, {0x4153, 1}),
          access(objects.OwnerAccess()), diagnostics(system) {
        REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(registration));
        auto acquired = types.Acquire(GeneratedType<AssignHolder>::Schema().id);
        REQUIRE(acquired);
        holder = Move(acquired.Value());
    }

    ~Fixture() noexcept {
        holder = TypeLease{};
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
        CHECK(registration.Unregister());
        CHECK(types.Shutdown());
    }

    RootRef<AssignHolder> Make() noexcept {
        auto made =
            objects.NewObject<AssignHolder>(Access(), GeneratedType<AssignHolder>::Schema().id);
        CHECK(made);
        return made ? Move(made.Value()) : RootRef<AssignHolder>{};
    }

    const ObjectAccessContext& Access() const noexcept {
        return access;
    }

    Result<PreparedAssignments> Prepare(ObjectHandle object,
                                        Span<const TextAssignment> assignments) noexcept {
        diagnostics.Clear();
        return PrepareAssignments(AssignmentTarget::Live(objects, Access(), object), assignments,
                                  options, diagnostics);
    }

    SystemAllocator system;
    TypeRegistry types;
    ModuleRegistration registration;
    ObjectRegistry objects;
    ObjectAccessContext access;
    TextDiagnostics diagnostics;
    TextCodecOptions options;
    TypeLease holder;
};

// 객체를 읽기 Borrow로 들여다본다(묶음 게시 전에 놓는다).
class Peek {
public:
    Peek(Fixture& fixture, ObjectHandle object) noexcept
        : borrow_(fixture.objects.Resolve<AssignHolder>(fixture.Access(), object)) {
        REQUIRE(borrow_);
    }

    AssignHolder* operator->() noexcept {
        return borrow_.Value().Get().Value();
    }

private:
    Result<Borrow<AssignHolder>> borrow_;
};

TextAssignment Set(const char* path, const char* value, uint32 source = 0) noexcept {
    return TextAssignment{AssignmentKind::Set, StringView(path), StringView(value), source};
}

TextAssignment Other(AssignmentKind kind, const char* path, uint32 source = 0) noexcept {
    return TextAssignment{kind, StringView(path), StringView(), source};
}

// override 경로 하나가 text(AssignHolder 경로)와 같은지다.
bool HasOverride(Fixture& fixture, Span<const PropertyPath> paths, const char* text) noexcept {
    PropertyPathBuffer expected;
    if (!ParsePropertyPath(fixture.holder, StringView(text), expected)) {
        return false;
    }

    for (usize index = 0; index < paths.Size(); ++index) {
        if (SamePropertyPath(paths[index], expected.View())) {
            return true;
        }
    }

    return false;
}

bool Touches(Fixture& fixture, const PreparedAssignments& prepared, const char* text) noexcept {
    PropertyPathBuffer path;
    CHECK(ParsePropertyPath(fixture.holder, StringView(text), path));
    return prepared.Touches(path.View());
}

} // namespace

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
                                     Set("next", "none", 5),
                                     Set("mixed.link", "none", 6),
                                     Set("pair[0]", "{}", 7),
                                     Other(AssignmentKind::Unset, "label", 8),
                                     Other(AssignmentKind::Erase, "atom", 9),
                                     Set("tags[x]", "v", 10),
                                     Set("sealed.x", "1", 11)};
    auto prepared = fixture.Prepare(object, inputs);
    CHECK(ErrorOf(prepared) == ErrorCode::InvalidArgument);
    const TextDiagnosticCode codes[] = {TextDiagnosticCode::DuplicateAssignment,
                                        TextDiagnosticCode::UnknownProperty,
                                        TextDiagnosticCode::InvalidValue,
                                        TextDiagnosticCode::ReadOnly,
                                        TextDiagnosticCode::Unsupported,
                                        TextDiagnosticCode::Unsupported,
                                        TextDiagnosticCode::Unsupported,
                                        TextDiagnosticCode::InvalidValue,
                                        TextDiagnosticCode::InvalidValue,
                                        TextDiagnosticCode::InvalidValue,
                                        TextDiagnosticCode::ReadOnly};
    const uint32 sources[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    REQUIRE(fixture.diagnostics.Count() == 11);
    for (usize index = 0; index < 11; ++index) {
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
