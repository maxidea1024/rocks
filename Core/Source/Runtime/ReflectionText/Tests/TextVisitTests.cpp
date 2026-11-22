#include "TextAssignTestSupport.h"

#include <Core/Text/NamePool.h>
#include <Object/RuntimeTypeOps.h>
#include <Platform/Math/GenericPlatformMath.h>
#include <TestSupport/NFailAllocator.h>
#include <ReflectionTestFixtures/BytesFixture.generated.h>
#include <ReflectionTestFixtures/InstancedStructFixture.generated.h>
#include <ReflectionTestFixtures/ReflectionTestFixtures.module.generated.h>
#include <ReflectionTestFixtures/TextFixture.generated.h>
#include <ReflectionText/TextVisit.h>

// 값 순회 시험이다(Docs/Design/ReflectionExtensions.ko.md 10.6절 — ReflectionText/TextVisit.h):
// 모든 종류의 이벤트 순서(필드 순서·Map·Set 저장 순서·Optional·struct·배열), 대상 셋(살아 있는
// 객체·staged·struct 값)이 같은 이벤트, 용도별 제외, InstancedStruct·byte 배열, 거부(Opaque·Name
// pool·struct 값 대상), visitor 오류가 그 지점에서 멈춤.
using namespace rocks;
namespace text_visit_tests_file {

using namespace rocks::reflection_text;
using namespace rocks::test::assign;
using reflection_fixture::BytesHolder;
using reflection_fixture::InstancedHolder;
using reflection_fixture::TextHolder;
using reflection_fixture::TextPoint;

namespace {

struct VisitObjects {
    VisitObjects() noexcept
        : failing(system), types(system), registration(types, system), names(system),
          objects(failing, types, {0x5649, 1}, MakeOptions(names)), access(objects.OwnerAccess()),
          diagnostics(system) {
        REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(registration));
        REQUIRE(names.Initialize());
    }

    ~VisitObjects() noexcept {
        failing.DisableFailure();
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
        CHECK(registration.Unregister());
        CHECK(names.Shutdown());
        CHECK(types.Shutdown());
    }

    static RegistryOptions MakeOptions(NamePool& pool) noexcept {
        RegistryOptions options;
        options.names = &pool;
        return options;
    }

    template <class T>
    RootRef<T> Make() noexcept {
        auto created = objects.NewObject<T>(access, GeneratedType<T>::Schema().id);
        CHECK(created);
        return created ? Move(created.Value()) : RootRef<T>{};
    }

    template <class T, class Edit>
    void With(ObjectHandle object, Edit&& edit) noexcept {
        auto borrowed = objects.Resolve<T>(access, object);
        REQUIRE(borrowed);
        auto value = borrowed.Value().Get();
        REQUIRE(value);
        edit(*value.Value());
    }

    bool Apply(const AssignmentTarget& target, Span<const TextAssignment> inputs) noexcept {
        diagnostics.Clear();
        auto prepared = PrepareAssignments(target, inputs, options, diagnostics);
        return prepared && static_cast<bool>(prepared.Value().Publish());
    }

    SystemAllocator system;
    // 객체 Registry의 allocator다(할당 실패 시험 — 기본은 실패하지 않는다).
    test::NFailAllocator failing;
    TypeRegistry types;
    ModuleRegistration registration;
    NamePool names;
    ObjectRegistry objects;
    ObjectAccessContext access;
    TextDiagnostics diagnostics;
    TextCodecOptions options;
};

// 이벤트를 한 줄씩 적는 visitor다. failAt(1부터)번째 이벤트가 failWith로 실패한다.
class LogVisitor final : public ValueVisitor {
public:
    explicit LogVisitor(Allocator& allocator) noexcept : log(allocator) {
    }

    Result<void> BeginStruct() noexcept override {
        return Note("{");
    }

    Result<void> EndStruct() noexcept override {
        return Note("}");
    }

    Result<void> Field(StringView name) noexcept override {
        return Note("F:", name);
    }

    Result<void> Value(const FieldValue& value) noexcept override {
        return NoteValue("V:", value);
    }

    Result<void> Absent() noexcept override {
        return Note("A");
    }

    Result<void> Bytes(Span<const uint8> bytes) noexcept override {
        StringBuilder<64> text(log.GetAllocator());
        CHECK(text.TryAppendFormat("X:{}", bytes.Size()));
        for (uint8 byte : bytes) {
            CHECK(text.TryAppendFormat(",{}", byte));
        }

        return Note(text.View());
    }

    Result<void> BeginArray(usize count) noexcept override {
        return NoteCount("[", count);
    }

    Result<void> EndArray() noexcept override {
        return Note("]");
    }

    Result<void> BeginSet(usize count) noexcept override {
        return NoteCount("S<", count);
    }

    Result<void> EndSet() noexcept override {
        return Note("S>");
    }

    Result<void> BeginMap(usize count) noexcept override {
        return NoteCount("M<", count);
    }

    Result<void> MapKey(const FieldValue& key) noexcept override {
        return NoteValue("K:", key);
    }

    Result<void> EndMap() noexcept override {
        return Note("M>");
    }

    Result<void> Instanced(TypeId type, StringView toolName) noexcept override {
        (void)type;
        return Note("I:", toolName);
    }

    String log;
    usize events = 0;
    usize failAt = 0;
    ErrorCode failWith = ErrorCode::Busy;

private:
    Result<void> Note(StringView head, StringView tail = {}) noexcept {
        ++events;
        if (failAt != 0 && events == failAt) {
            return Result<void>::Failure(failWith);
        }

        CHECK(log.TryAppend(head) && log.TryAppend(tail) && log.TryAppend("\n"));
        return Result<void>::Success();
    }

    Result<void> NoteCount(StringView head, usize count) noexcept {
        StringBuilder<32> text(log.GetAllocator());
        CHECK(text.TryAppendFormat("{}{}", head, count));
        return Note(text.View());
    }

    // 값은 종류 번호와 대표 칸(정수·실수 bit·문자열·Guid)으로 적는다.
    Result<void> NoteValue(StringView head, const FieldValue& value) noexcept {
        StringBuilder<160> text(log.GetAllocator());
        CHECK(text.TryAppendFormat("{}{}", head, static_cast<uint32>(value.kind)));
        switch (value.kind) {
        case ValueKind::Bool:
            CHECK(text.TryAppendFormat("={}", value.boolean ? 1 : 0));
            break;
        case ValueKind::Int8:
        case ValueKind::Int16:
        case ValueKind::Int32:
        case ValueKind::Int64:
            CHECK(text.TryAppendFormat("={}", value.signedInteger));
            break;
        case ValueKind::Float32:
            CHECK(text.TryAppendFormat("={}", platform::BitCast<uint32>(value.float32)));
            break;
        case ValueKind::Float64:
            CHECK(text.TryAppendFormat("={}", platform::BitCast<uint64>(value.float64)));
            break;
        case ValueKind::Utf8String:
            CHECK(text.TryAppendFormat("={}", value.string.View()));
            break;
        default:
            CHECK(text.TryAppendFormat("={}", value.unsignedInteger));
            break;
        }

        return Note(text.View());
    }
};

bool Has(const String& text, StringView part) noexcept {
    return StringView(text.View()).Contains(part);
}

} // namespace

// 모든 종류의 이벤트: 필드 이름 순서(스키마), 단일 값, unset·set Optional(단일 값·struct), struct,
// Array·StaticArray·Array<struct>, Set·Map 저장 순서(정렬하지 않는다), Map<.., struct>. Edit·
// Report는 Transient를 빼고 ReadOnly는 넘긴다. 시작·끝 이벤트는 짝이 맞는다.
TEST(TextVisit_WalksEveryKindInStoredOrder) {
    VisitObjects fixture;
    auto root = fixture.Make<TextHolder>();
    const ObjectHandle object = root.Handle();
    const AssignmentTarget live = AssignmentTarget::Live(fixture.objects, fixture.access, object);
    const TextAssignment inputs[] = {
        Set("enabled", "true"),       Set("count", "4000000000", 1), Set("tiny", "-128", 2),
        Set("ratio", "0.5", 3),       Set("precise", "nan", 4),      Set("label", "a,b", 5),
        Set("quality", "High", 6),    Set("numbers", "3,1,2", 7),    Set("points[0]", "{}", 8),
        Set("points[0].x", "4", 9),   Set("triple", "1,2,3", 10),    Set("tags", "b,a", 11),
        Set("scores", "z:1,a:2", 12), Set("byName[k]", "{}", 13),    Set("byName[k].y", "6", 14),
        Set("anchor", "{}", 15),      Set("anchor.x", "7", 16),      Set("origin.y", "-1", 17)};
    REQUIRE(fixture.Apply(live, inputs));
    fixture.With<TextHolder>(object, [](TextHolder& holder) noexcept {
        holder.cache = 5;
        holder.locked = 3;
    });

    LogVisitor visitor(fixture.system);
    fixture.options.use = TextUse::Report;
    REQUIRE(VisitValues(live, visitor, fixture.options));
    const String& log = visitor.log;
    CHECK(log.View().StartsWith("{\n") && log.View().EndsWith("}\n"));
    CHECK(Has(log, "F:enabled\nV:1=1\n") && Has(log, "F:tiny\nV:") && Has(log, "=-128\n"));
    CHECK(Has(log, "F:label\nV:") && Has(log, "=a,b\n"));
    // unset Optional<단일 값>·set Optional<struct>, struct 필드.
    CHECK(Has(log, "F:limit\nA\n") && Has(log, "F:anchor\n{\nF:x\nV:") &&
          Has(log, "F:origin\n{\n"));
    // 배열 셋: Array<int32>·StaticArray·Array<struct>.
    CHECK(Has(log, "F:numbers\n[3\nV:") && Has(log, "F:triple\n[3\n") &&
          Has(log, "F:points\n[1\n{\nF:x\n"));
    // Set·Map은 저장 순서다(정렬은 소비자 — 키 기록이 한다).
    CHECK(Has(log, "F:tags\nS<2\n") && Has(log, "F:scores\nM<2\nK:"));
    const StringView text = log.View();
    const usize z = text.Find("=z\n");
    const usize a = text.Find("=a\n");
    CHECK(z != IndexNone && a != IndexNone && z < a);
    CHECK(Has(log, "F:byName\nM<1\nK:") && Has(log, "{\nF:x\nV:"));
    // 제외: Report는 Transient만 뺀다(ReadOnly는 넘긴다).
    CHECK(!Has(log, "F:cache\n") && Has(log, "F:locked\n"));
    usize opens = 0;
    usize closes = 0;
    for (usize index = 0; index < text.ByteCount(); ++index) {
        opens += text.Data()[index] == '{' ? 1 : 0;
        closes += text.Data()[index] == '}' ? 1 : 0;
    }

    CHECK(opens == closes);

    // Edit도 Transient를 뺀다.
    LogVisitor edit(fixture.system);
    fixture.options.use = TextUse::Edit;
    REQUIRE(VisitValues(live, edit, fixture.options));
    CHECK(!Has(edit.log, "F:cache\n"));
}

// 대상 셋(살아 있는 객체·staged 객체·struct 값)이 같은 이벤트를 낸다. struct 값 대상의 lease가
// 없거나 struct가 아니면 InvalidArgument, 다른 Name pool은 InvalidArgument.
TEST(TextVisit_TargetsAgreeAndBadTargetsFail) {
    VisitObjects fixture;
    auto root = fixture.Make<TextHolder>();
    const ObjectHandle object = root.Handle();
    const AssignmentTarget live = AssignmentTarget::Live(fixture.objects, fixture.access, object);
    const TextAssignment inputs[] = {Set("count", "7"), Set("scores", "q:1", 1),
                                     Set("origin.x", "2", 2)};
    REQUIRE(fixture.Apply(live, inputs));
    LogVisitor first(fixture.system);
    REQUIRE(VisitValues(live, first, fixture.options));

    ConstructionContext transaction(fixture.objects, fixture.access);
    auto created = transaction.Create<TextHolder>(GeneratedType<TextHolder>::Schema().id);
    REQUIRE(created);
    const AssignmentTarget staged = AssignmentTarget::Staged(transaction, created.Value());
    REQUIRE(fixture.Apply(staged, inputs));
    LogVisitor second(fixture.system);
    REQUIRE(VisitValues(staged, second, fixture.options));
    CHECK(second.log.View() == first.log.View());
    transaction.Rollback();

    // struct 값(TextPoint).
    auto lease =
        fixture.types.AcquireValueType(GeneratedType<TextPoint>::Schema().id, TypeKind::Struct);
    REQUIRE(lease);
    TextPoint point;
    point.x = 9;
    LogVisitor value(fixture.system);
    REQUIRE(VisitValues(AssignmentTarget::Value(fixture.objects, fixture.access, lease.Value(),
                                                &point, fixture.system),
                        value, fixture.options));
    // 필드 순서는 등록 스키마 순서다(MetaCompiler가 낸 순서) — 두 필드와 값, 감싼 struct를 본다.
    StringBuilder<32> nine(fixture.system);
    StringBuilder<32> zero(fixture.system);
    REQUIRE(nine.TryAppendFormat("F:x\nV:{}=9\n", static_cast<uint32>(ValueKind::Int32)) &&
            zero.TryAppendFormat("F:y\nV:{}=0\n", static_cast<uint32>(ValueKind::Int32)));
    CHECK(value.log.View().StartsWith("{\n") && value.log.View().EndsWith("}\n") &&
          Has(value.log, nine.View()) && Has(value.log, zero.View()) && value.events == 6);
    CHECK(VisitValues(AssignmentTarget::Value(fixture.objects, fixture.access, lease.Value(),
                                              nullptr, fixture.system),
                      value, fixture.options)
              .Error() == ErrorCode::InvalidArgument);
    auto classLease = fixture.types.Acquire(GeneratedType<TextHolder>::Schema().id);
    REQUIRE(classLease);
    CHECK(VisitValues(AssignmentTarget::Value(fixture.objects, fixture.access, classLease.Value(),
                                              &point, fixture.system),
                      value, fixture.options)
              .Error() == ErrorCode::InvalidArgument);
    NamePool other(fixture.system);
    REQUIRE(other.Initialize());
    TextCodecOptions foreign;
    foreign.names = &other;
    CHECK(VisitValues(live, value, foreign).Error() == ErrorCode::InvalidArgument);
    CHECK(other.Shutdown());
    // 잘못된 codec 옵션(용도 값 밖)은 문맥 시작의 InvalidArgument다.
    TextCodecOptions broken;
    broken.use = static_cast<TextUse>(200);
    CHECK(VisitValues(live, value, broken).Error() == ErrorCode::InvalidArgument);
}

// InstancedStruct(빈 값 Absent, 담긴 값은 타입 도구 이름과 struct)·byte 배열(Bytes 하나 — 원소
// 순서).
TEST(TextVisit_InstancedAndBytes) {
    VisitObjects fixture;
    {
        auto root = fixture.Make<InstancedHolder>();
        LogVisitor empty(fixture.system);
        const AssignmentTarget live =
            AssignmentTarget::Live(fixture.objects, fixture.access, root.Handle());
        REQUIRE(VisitValues(live, empty, fixture.options));
        CHECK(Has(empty.log, "F:value\nA\n"));
        fixture.With<InstancedHolder>(root.Handle(), [&](InstancedHolder& holder) noexcept {
            REQUIRE(holder.value.Emplace<reflection_fixture::InstancedPoint>(
                fixture.types, fixture.objects.ObjectAllocator()));
        });
        LogVisitor filled(fixture.system);
        REQUIRE(VisitValues(live, filled, fixture.options));
        CHECK(Has(filled.log, "F:value\nI:InstancedPoint\n{\n") && Has(filled.log, "F:label\n"));
        fixture.With<InstancedHolder>(
            root.Handle(), [](InstancedHolder& holder) noexcept { holder.value.Reset(); });
    }

    auto bytes = fixture.Make<BytesHolder>();
    fixture.With<BytesHolder>(bytes.Handle(), [](BytesHolder& holder) noexcept {
        REQUIRE(holder.small.TryPushBack(7) && holder.small.TryPushBack(200));
    });
    LogVisitor visitor(fixture.system);
    REQUIRE(VisitValues(AssignmentTarget::Live(fixture.objects, fixture.access, bytes.Handle()),
                        visitor, fixture.options));
    CHECK(Has(visitor.log, "F:small\nX:2,7,200\n") && Has(visitor.log, "F:payload\nX:0\n"));
}

// visitor가 n번째 이벤트에서 실패하면 순회가 그 오류로 멈춘다(모든 n).
TEST(TextVisit_VisitorErrorsStopTheWalk) {
    VisitObjects fixture;
    auto root = fixture.Make<TextHolder>();
    const AssignmentTarget live =
        AssignmentTarget::Live(fixture.objects, fixture.access, root.Handle());
    const TextAssignment inputs[] = {Set("scores", "z:1", 0), Set("tags", "b", 1),
                                     Set("anchor", "{}", 2), Set("numbers", "1", 3)};
    REQUIRE(fixture.Apply(live, inputs));
    LogVisitor counting(fixture.system);
    REQUIRE(VisitValues(live, counting, fixture.options));
    const usize total = counting.events;
    REQUIRE(total > 10);
    for (usize failAt = 1; failAt <= total; ++failAt) {
        LogVisitor visitor(fixture.system);
        visitor.failAt = failAt;
        visitor.failWith = ErrorCode::Busy;
        CHECK(VisitValues(live, visitor, fixture.options).Error() == ErrorCode::Busy &&
              visitor.events == failAt);
    }
}

namespace {

struct OpaqueVisitHolder final : Object {};

constexpr TypeId OpaqueVisitHolderType{0x5649, 7};
constexpr Guid OpaqueVisitFieldId{0x5649, 8};
constexpr StringView OpaqueVisitHolderName = "OpaqueVisitHolder";

} // namespace

// 접근기 없는 Opaque 필드는 Unsupported다(ExportJson과 같다).
TEST(TextVisit_OpaqueIsUnsupported) {
    VisitObjects fixture;
    FieldSchema field;
    field.id = OpaqueVisitFieldId;
    field.name = Span<const char>("blob", 4);
    field.value = ValueType{ValueKind::Opaque, {}};
    auto token = fixture.types.Register(
        {OpaqueVisitHolderType, 1, Span<const FieldSchema>(&field, 1),
         Span<const char>(OpaqueVisitHolderName.Data(), OpaqueVisitHolderName.ByteCount())},
        MakeRuntimeTypeOps<OpaqueVisitHolder>());
    REQUIRE(token);
    auto created =
        fixture.objects.NewObject<OpaqueVisitHolder>(fixture.access, OpaqueVisitHolderType);
    REQUIRE(created);
    const ObjectHandle object = created.Value().Handle();
    LogVisitor visitor(fixture.system);
    CHECK(VisitValues(AssignmentTarget::Live(fixture.objects, fixture.access, object), visitor,
                      fixture.options)
              .Error() == ErrorCode::Unsupported);
    REQUIRE(fixture.objects.RequestDestroy(fixture.access, object));
}

// 명시한 Name pool(대상 Registry의 것)은 받는다, 경로 단계 상한(MaxPropertyPathSteps)을 넘는 자리는
// Unsupported다(InstancedStruct 깊이 16 사슬 — ExportJson과 같다).
TEST(TextVisit_ExplicitPoolAndDepthLimit) {
    VisitObjects fixture;
    auto root = fixture.Make<InstancedHolder>();
    const AssignmentTarget live =
        AssignmentTarget::Live(fixture.objects, fixture.access, root.Handle());
    TextCodecOptions own;
    own.names = &fixture.names;
    LogVisitor visitor(fixture.system);
    CHECK(VisitValues(live, visitor, own));
    fixture.With<InstancedHolder>(root.Handle(), [&](InstancedHolder& holder) noexcept {
        REQUIRE(holder.host.value.Emplace<reflection_fixture::InstancedDepth16>(
            fixture.types, fixture.objects.ObjectAllocator()));
    });
    LogVisitor deep(fixture.system);
    CHECK(VisitValues(live, deep, fixture.options).Error() == ErrorCode::Unsupported);
    fixture.With<InstancedHolder>(
        root.Handle(), [](InstancedHolder& holder) noexcept { holder.host.value.Reset(); });
}

// InstancedStruct·byte 배열 대상에서도 visitor가 n번째 이벤트에서 실패하면 그 오류로 멈춘다.
TEST(TextVisit_VisitorErrorsInInstancedAndBytes) {
    VisitObjects fixture;
    auto instanced = fixture.Make<InstancedHolder>();
    fixture.With<InstancedHolder>(instanced.Handle(), [&](InstancedHolder& holder) noexcept {
        REQUIRE(holder.value.Emplace<reflection_fixture::InstancedPoint>(
            fixture.types, fixture.objects.ObjectAllocator()));
    });
    auto bytes = fixture.Make<BytesHolder>();
    fixture.With<BytesHolder>(
        bytes.Handle(), [](BytesHolder& holder) noexcept { REQUIRE(holder.small.TryPushBack(1)); });
    const ObjectHandle objects[] = {instanced.Handle(), bytes.Handle()};
    for (ObjectHandle object : objects) {
        const AssignmentTarget live =
            AssignmentTarget::Live(fixture.objects, fixture.access, object);
        LogVisitor counting(fixture.system);
        REQUIRE(VisitValues(live, counting, fixture.options));
        for (usize failAt = 1; failAt <= counting.events; ++failAt) {
            LogVisitor visitor(fixture.system);
            visitor.failAt = failAt;
            CHECK(VisitValues(live, visitor, fixture.options).Error() == ErrorCode::Busy &&
                  visitor.events == failAt);
        }
    }

    fixture.With<InstancedHolder>(instanced.Handle(),
                                  [](InstancedHolder& holder) noexcept { holder.value.Reset(); });
}

// 할당 실패(문맥·경로 단계·key wire·값 읽기·byte 배열·Map 키 읽기·Optional·InstancedStruct): 모두
// OutOfMemory이고 실패 없는 이벤트는 기준과 같다.
TEST(TextVisit_AllocationFailures) {
    VisitObjects fixture;
    auto text = fixture.Make<TextHolder>();
    const AssignmentTarget textTarget =
        AssignmentTarget::Live(fixture.objects, fixture.access, text.Handle());
    const TextAssignment inputs[] = {Set("label", "a,b"),       Set("numbers", "3,1", 1),
                                     Set("tags", "b,a", 2),     Set("scores", "z:1,a:2", 3),
                                     Set("byName[k]", "{}", 4), Set("anchor", "{}", 5),
                                     Set("limit", "4", 6),      Set("points[0]", "{}", 7)};
    REQUIRE(fixture.Apply(textTarget, inputs));
    auto instanced = fixture.Make<InstancedHolder>();
    fixture.With<InstancedHolder>(instanced.Handle(), [&](InstancedHolder& holder) noexcept {
        REQUIRE(holder.value.Emplace<reflection_fixture::InstancedPoint>(
            fixture.types, fixture.objects.ObjectAllocator()));
    });
    auto bytes = fixture.Make<BytesHolder>();
    fixture.With<BytesHolder>(bytes.Handle(), [](BytesHolder& holder) noexcept {
        REQUIRE(holder.small.TryPushBack(1) && holder.small.TryPushBack(2));
    });
    const ObjectHandle objects[] = {text.Handle(), instanced.Handle(), bytes.Handle()};
    uint32 failures = 0;
    for (ObjectHandle object : objects) {
        const AssignmentTarget live =
            AssignmentTarget::Live(fixture.objects, fixture.access, object);
        LogVisitor expected(fixture.system);
        REQUIRE(VisitValues(live, expected, fixture.options));
        for (uint64 attempt = 1;; ++attempt) {
            LogVisitor visitor(fixture.system);
            fixture.failing.FailOnAttempt(attempt);
            auto visited = VisitValues(live, visitor, fixture.options);
            const bool failed = fixture.failing.Attempts() >= attempt;
            fixture.failing.DisableFailure();
            if (!failed) {
                REQUIRE(visited);
                CHECK(visitor.log.View() == expected.log.View());
                break;
            }

            CHECK(visited.Error() == ErrorCode::OutOfMemory);
            ++failures;
        }
    }

    CHECK(failures > 3);
    fixture.With<InstancedHolder>(instanced.Handle(),
                                  [](InstancedHolder& holder) noexcept { holder.value.Reset(); });
}

} // namespace text_visit_tests_file
