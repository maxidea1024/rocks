#include <ReflectionTestFixtures/ClassDefaultsFixture.generated.h>
#include <ReflectionTestFixtures/ReflectionTestFixtures.module.generated.h>
#include <Core/Encoding/Sha256.h>
#include <Core/Memory/SystemAllocator.h>
#include <Object/GeneratedFieldAccess.h>
#include <Object/GeneratedFieldCodec.h>
#include <Object/ModuleRegistration.h>
#include <ObjectSerialization/ClassDefaults.h>
#include <ObjectSerialization/ObjectGraph.h>
#include <Platform/Fatal.h>
#include <Platform/Thread.h>
#include <Serialization/Record.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>
#include "GraphTestSupport.h"

// 클래스 기본값 레코드 시험이다(Docs/Design/RuntimeClassMetadata.ko.md '클래스 기본값 구현 명세 —
// A5a 레코드', fixture ReflectionTestFixtures/ClassDefaultsFixture.h): 레코드 byte가 같은 객체의
// 그래프 저장 Record와 같음(editor·cooked), 지문 공식, 게시 한 번, 레코드 없는 타입(추상·불완전
// 레이아웃·손 함수표)과 실패 기억, 비결정 생성자, non-null Soft 기본값, 할당 실패(기억하지 않음),
// 다른 스레드가 만든 레코드, 경로 조회, 전부 만들기.
using namespace rocks;
namespace class_defaults_tests_file {

using reflection_fixture::DefaultsAbstractBase;
using reflection_fixture::DefaultsCounter;
using reflection_fixture::DefaultsHolder;
using reflection_fixture::DefaultsKindPick;
using reflection_fixture::DefaultsPoint;
using reflection_fixture::DefaultsSoftLink;
using reflection_fixture::DefaultsSoftList;
using reflection_fixture::DefaultsSoftNested;
using reflection_fixture::DefaultsStrangerKind;
using reflection_fixture::DefaultsStrangerType;
using reflection_fixture::DefaultsVirtual;

namespace {

// 모듈 집계 등록 묶음이다.
struct DefaultsFixture {
    explicit DefaultsFixture(Allocator& allocator) noexcept
        : types(allocator), registration(types, allocator) {
        REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(registration));
    }

    ~DefaultsFixture() noexcept {
        CHECK(registration.Unregister());
        CHECK(types.Shutdown());
    }

    TypeLease Lease(TypeId type) noexcept {
        auto acquired = types.Acquire(type);
        if (!acquired) {
            platform::Fatal("test type lease failed");
        }

        return Move(acquired.Value());
    }

    TypeRegistry types;
    ModuleRegistration registration;
};

// 노드 하나짜리 그래프(NodeTable 없음)의 노드 Record다: header 24 byte 뒤 u64 길이 + Record.
Span<const uint8> NodeRecord(Span<const uint8> graph) noexcept {
    constexpr usize LengthBytes = 8;
    uint64 length = 0;
    for (usize index = 0; index < LengthBytes; ++index) {
        length |= static_cast<uint64>(graph[test::graph::HeaderBytes + index]) << (8 * index);
    }

    return graph.Slice(test::graph::HeaderBytes + LengthBytes, static_cast<usize>(length));
}

// 객체 하나를 policy로 저장한 그래프다.
Array<uint8> SaveOne(Allocator& allocator, ObjectRegistry& objects, ObjectHandle handle,
                     uint32 excludedFlags) noexcept {
    ObjectGraphSaveOptions options;
    options.policy.excludedFlags = excludedFlags;
    auto saved = SaveObjectGraph(allocator, objects, objects.OwnerAccess(), handle, options);
    if (!saved) {
        platform::Fatal("test graph save failed");
    }

    return saved.Value().TakeBytes();
}

// 명세의 지문 공식을 시험 쪽에서 따로 계산한다.
Sha256Digest ExpectedFingerprint(uint8 configuration, Span<const uint8> record) noexcept {
    static constexpr char Domain[] = "RocksClassDefaults";
    Sha256 hasher;
    hasher.Update(Span<const uint8>(reinterpret_cast<const uint8*>(Domain), sizeof(Domain) - 1));
    hasher.Update(Span<const uint8>(&configuration, 1));
    const uint8 version[] = {static_cast<uint8>(ObjectGraphFormatVersion), 0, 0, 0};
    hasher.Update(Span<const uint8>(version, 4));
    hasher.Update(record);
    return hasher.Finish();
}

constexpr uint32 EditorOnlyBit = static_cast<uint32>(FieldFlags::EditorOnly);

// fixture 스키마에서 이름으로 찾은 필드 ID다.
template <class T>
Guid FieldNamed(StringView name) noexcept {
    for (const FieldSchema& field : TypeOf<T>().fields) {
        if (StringView(field.name.Data(), field.name.Size()) == name) {
            return field.id;
        }
    }

    platform::Fatal("fixture field missing");
}

// 손 함수표 클래스다(생성 코덱 표지 없음 — ManualFields, completeFieldLayout 거짓이면
// IncompleteLayout). ManualType은 DefaultsStrangerKind의 ClassRef 대상 id와 같다.
constexpr Guid ManualType = DefaultsStrangerType;
constexpr Guid IncompleteType{0x4D414E55414C, 2};
constexpr Guid DirectType{0x4D414E55414C, 3};
constexpr Guid ManualValueId{0x4D414E, 1};

struct ManualThing final : Object {
    uint32 value = 6;
};

// 범용 codec이 아닌 손 encode다(Registry로 읽어 바로 쓴다 — 원본 모드를 모른다).
Result<uint32> DirectEncode(const RuntimeFieldOps&, ObjectRegistry* registry,
                            const ObjectAccessContext& access, ObjectHandle object,
                            ArchiveWriter& out, FieldEncodeContext&) noexcept {
    if (registry == nullptr) {
        return Result<uint32>::Failure(ErrorCode::InvalidArgument);
    }

    auto borrowed = registry->Resolve<ManualThing>(access, object);
    if (!borrowed) {
        return Result<uint32>::Failure(borrowed.Error());
    }

    auto value = borrowed.Value().Get();
    if (!value) {
        return Result<uint32>::Failure(value.Error());
    }

    if (auto written = out.WriteU32(value.Value()->value); !written) {
        return Result<uint32>::Failure(written.Error());
    }

    return Result<uint32>::Success(static_cast<uint32>(WireType::U32));
}

Result<TypeRegistrationToken>
RegisterManual(TypeRegistry& types, TypeId type, bool completeLayout,
               FieldEncodeFunction encode = &detail::EncodeThroughFieldValue) noexcept {
    static constexpr char Name[] = {'v', 'a', 'l', 'u', 'e'};
    FieldSchema field;
    field.id = ManualValueId;
    field.name = {Name, sizeof(Name)};
    field.value.kind = ValueKind::UInt32;
    const FieldSchema fields[] = {field};
    const RuntimeFieldOps accessors[] = {
        {ManualValueId,
         [](ObjectRegistry& registry, const ObjectAccessContext& access, ObjectHandle object,
            FieldValue& out) noexcept {
             return detail::ReadGeneratedField<ManualThing>(registry, access, object,
                                                            &ManualThing::value, out);
         },
         [](ObjectRegistry& registry, const ObjectAccessContext& access, ObjectHandle object,
            const FieldValue& in) noexcept {
             return detail::WriteGeneratedField<ManualThing>(registry, access, object,
                                                             &ManualThing::value, in);
         },
         [](ConstructionContext& transaction, ObjectHandle object, const FieldValue& in) noexcept {
             return detail::WriteGeneratedStagedField<ManualThing>(transaction, object,
                                                                   &ManualThing::value, in);
         },
         encode, &detail::DecodeThroughFieldValue}};
    auto ops = MakeRuntimeTypeOps<ManualThing>();
    ops.fields = accessors;
    ops.completeFieldLayout = completeLayout;
    static constexpr char TypeName[] = {'M', 'a', 'n', 'u', 'a', 'l'};
    return types.Register({type, 1, fields, {TypeName, sizeof(TypeName)}}, Move(ops));
}

// DefaultsStrangerKind의 ClassRef 대상이다(DefaultsAbstractBase 파생 손 타입 — 시험이 나중에
// 등록한다). base의 펼친 필드와 생성 함수표를 그대로 싣는다(상속 필드 서술 일치 규칙).
struct StrangerThing final : DefaultsAbstractBase {};

Result<TypeRegistrationToken> RegisterStranger(TypeRegistry& types) noexcept {
    auto base = types.Acquire(TypeIdOf<DefaultsAbstractBase>());
    if (!base) {
        return Result<TypeRegistrationToken>::Failure(base.Error());
    }

    static constexpr char Name[] = {'S', 't', 'r', 'a', 'n', 'g', 'e', 'r'};
    TypeSchema schema;
    schema.id = DefaultsStrangerType;
    schema.fields = base.Value().Schema().fields;
    schema.name = Span<const char>(Name, sizeof(Name));
    schema.baseType = TypeIdOf<DefaultsAbstractBase>();
    auto ops = MakeRuntimeTypeOps<StrangerThing>();
    ops.fields = base.Value().Ops().fields;
    ops.completeFieldLayout = true;
    ops.nativeBaseType = NativeTypeKey<DefaultsAbstractBase>();
    return types.Register(schema, Move(ops));
}

// builder 호출 수를 세는 시험 builder다(ResolveClassDefaults 게시 한 번 — 레코드 byte는 고정 값).
struct CountingBuild {
    uint32 calls = 0;
    uint8 bytes[1] = {0x5A};
};

Result<void> CountingBuilder(void* user, const TypeLease&, TypeRegistry&, ClassDefaultsDraft& draft,
                             ClassDefaultsDiagnostic&) noexcept {
    auto& build = *static_cast<CountingBuild*>(user);
    ++build.calls;
    draft.editorRecord = Span<const uint8>(build.bytes, 1);
    draft.cookedRecord = Span<const uint8>(build.bytes, 1);
    return Result<void>::Success();
}

} // namespace

// 레코드 = 같은 객체(생성자 값 그대로)의 그래프 저장 Record. editor는 기본 정책, cooked는
// EditorOnly 제외 정책과 같다(struct 안 EditorOnly 멤버 포함). 가상 함수가 있는 파생(저장소 시작 ≠
// Object 기반일 수 있다)과 상속 필드도 같다. 지문은 명세 공식이고 구성마다 다르다. 두 번째 호출은
// 같은 레코드(게시 한 번)다.
TEST(ClassDefaults_RecordMatchesGraphSave) {
    SystemAllocator allocator;
    DefaultsFixture fixture(allocator);
    {
        ObjectRegistry objects(allocator, fixture.types, {0x4344, 1});
        const TypeId types[] = {TypeIdOf<DefaultsHolder>(), TypeIdOf<DefaultsVirtual>()};
        for (TypeId type : types) {
            TypeLease lease = fixture.Lease(type);
            ClassDefaultsDiagnostic why;
            auto defaults = AcquireClassDefaults(lease, allocator, &why);
            REQUIRE(defaults);
            CHECK(why.reason == ClassDefaultsReason::None);
            auto created = objects.NewObject<DefaultsHolder>(objects.OwnerAccess(), type);
            REQUIRE(created);
            const Array<uint8> editorGraph =
                SaveOne(allocator, objects, created.Value().Handle(), 0);
            const Array<uint8> cookedGraph =
                SaveOne(allocator, objects, created.Value().Handle(), EditorOnlyBit);
            const ClassDefaults& record = *defaults.Value();
            CHECK(test::graph::SameBytes(record.Record(ClassDefaultsConfiguration::Editor),
                                         NodeRecord(editorGraph.View())));
            // 다른 ObjectRegistry(같은 TypeRegistry)의 객체도 같은 기준이다.
            {
                ObjectRegistry other(allocator, fixture.types, {0x4344, 2});
                auto copy = other.NewObject<DefaultsHolder>(other.OwnerAccess(), type);
                REQUIRE(copy);
                const Array<uint8> otherGraph = SaveOne(allocator, other, copy.Value().Handle(), 0);
                CHECK(test::graph::SameBytes(record.Record(ClassDefaultsConfiguration::Editor),
                                             NodeRecord(otherGraph.View())));
                copy.Value().Reset();
                CHECK(other.Shutdown(other.OwnerAccess()));
            }

            CHECK(test::graph::SameBytes(record.Record(ClassDefaultsConfiguration::Cooked),
                                         NodeRecord(cookedGraph.View())));
            CHECK(record.Fingerprint(ClassDefaultsConfiguration::Editor) ==
                  ExpectedFingerprint(0, record.Record(ClassDefaultsConfiguration::Editor)));
            CHECK(record.Fingerprint(ClassDefaultsConfiguration::Cooked) ==
                  ExpectedFingerprint(1, record.Record(ClassDefaultsConfiguration::Cooked)));
            CHECK(!(record.Fingerprint(ClassDefaultsConfiguration::Editor) ==
                    record.Fingerprint(ClassDefaultsConfiguration::Cooked)));
            // cooked에는 EditorOnly 필드가 없다.
            const TypeSchema& schema = lease.Schema();
            auto cooked =
                RecordReader::Open(allocator, record.Record(ClassDefaultsConfiguration::Cooked),
                                   schema.id, schema.version, schema.version);
            REQUIRE(cooked);
            const usize editorValue = lease.FieldIndex(FieldNamed<DefaultsHolder>("editorValue"));
            REQUIRE(editorValue != TypeLease::NoField);
            CHECK(cooked.Value().Find(schema.fields[editorValue].id).Error() ==
                  ErrorCode::NotFound);
            // 게시 한 번 — 같은 레코드.
            auto again = AcquireClassDefaults(lease, allocator);
            REQUIRE(again);
            CHECK(again.Value() == defaults.Value());
        }

        CHECK(objects.Shutdown(objects.OwnerAccess()));
    }
}

// 레코드 없는 타입: 추상 Abstract, 불완전 레이아웃 IncompleteLayout, 손 함수표 ManualFields — 모두
// Unsupported이고 기억한다(두 번째 호출도 같은 진단). enum·struct lease는 InvalidArgument.
TEST(ClassDefaults_TypesWithoutRecord) {
    SystemAllocator allocator;
    DefaultsFixture fixture(allocator);
    auto manual = RegisterManual(fixture.types, ManualType, true);
    auto incomplete = RegisterManual(fixture.types, IncompleteType, false);
    REQUIRE(manual && incomplete);
    const auto refuses = [&](TypeId type, ClassDefaultsReason reason) noexcept {
        TypeLease lease = fixture.Lease(type);
        ClassDefaultsDiagnostic first;
        ClassDefaultsDiagnostic second;
        auto a = AcquireClassDefaults(lease, allocator, &first);
        auto b = AcquireClassDefaults(lease, allocator, &second);
        return !a && !b && a.Error() == ErrorCode::Unsupported &&
               b.Error() == ErrorCode::Unsupported && first.reason == reason &&
               second.reason == reason && second.type == type;
    };
    CHECK(refuses(TypeIdOf<DefaultsAbstractBase>(), ClassDefaultsReason::Abstract));
    CHECK(refuses(IncompleteType, ClassDefaultsReason::IncompleteLayout));
    CHECK(refuses(ManualType, ClassDefaultsReason::ManualFields));
    // 범용 codec이 아닌 손 encode도 표지가 없어 ManualFields다(원본 모드로 부르지 않는다).
    auto direct = RegisterManual(fixture.types, DirectType, true, &DirectEncode);
    REQUIRE(direct);
    CHECK(refuses(DirectType, ClassDefaultsReason::ManualFields));
    CHECK(fixture.types.Unregister(Move(direct.Value())));
    CHECK(fixture.types.Unregister(Move(incomplete.Value())));
    CHECK(fixture.types.Unregister(Move(manual.Value())));
}

// 재등록하면 새 슬롯이다(옛 레코드·기억이 따라오지 않는다 — builder 다시 호출).
TEST(ClassDefaults_NewSlotAfterReregistration) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    CountingBuild build;
    {
        auto token = RegisterManual(types, ManualType, true);
        REQUIRE(token);
        {
            auto lease = types.Acquire(ManualType);
            REQUIRE(lease);
            REQUIRE(lease.Value().ResolveClassDefaults(&CountingBuilder, &build));
            REQUIRE(lease.Value().ResolveClassDefaults(&CountingBuilder, &build));
            CHECK(build.calls == 1);
        }

        REQUIRE(types.Unregister(Move(token.Value())));
    }

    auto again = RegisterManual(types, ManualType, true);
    REQUIRE(again);
    {
        auto lease = types.Acquire(ManualType);
        REQUIRE(lease);
        REQUIRE(lease.Value().ResolveClassDefaults(&CountingBuilder, &build));
        CHECK(build.calls == 2);
    }

    CHECK(types.Unregister(Move(again.Value())));
    CHECK(types.Shutdown());
}

// non-null ClassRef 기본값: 등록된 파생(DefaultsKindPick)은 레코드 = 그래프 저장 Record(ClassRef
// 판정은 TypeRegistry). 대상 미등록(DefaultsStrangerKind)은 NotFound이고 기억하지 않는다 — 대상을
// 등록하면 성공한다.
TEST(ClassDefaults_ClassRefDefaults) {
    SystemAllocator allocator;
    DefaultsFixture fixture(allocator);
    {
        TypeLease pick = fixture.Lease(TypeIdOf<DefaultsKindPick>());
        auto defaults = AcquireClassDefaults(pick, allocator);
        REQUIRE(defaults);
        ObjectRegistry objects(allocator, fixture.types, {0x4344, 3});
        auto created = objects.NewObject<DefaultsKindPick>(objects.OwnerAccess(),
                                                           TypeIdOf<DefaultsKindPick>());
        REQUIRE(created);
        const Array<uint8> graph = SaveOne(allocator, objects, created.Value().Handle(), 0);
        CHECK(test::graph::SameBytes(defaults.Value()->Record(ClassDefaultsConfiguration::Editor),
                                     NodeRecord(graph.View())));
        created.Value().Reset();
        CHECK(objects.Shutdown(objects.OwnerAccess()));
    }

    TypeLease stranger = fixture.Lease(TypeIdOf<DefaultsStrangerKind>());
    ClassDefaultsDiagnostic why;
    auto missing = AcquireClassDefaults(stranger, allocator, &why);
    CHECK(!missing && missing.Error() == ErrorCode::NotFound &&
          why.reason == ClassDefaultsReason::EncodeFailed);
    auto token = RegisterStranger(fixture.types);
    REQUIRE(token);
    CHECK(AcquireClassDefaults(stranger, allocator));
    stranger.Reset();
    CHECK(fixture.types.Unregister(Move(token.Value())));
}

// 비결정 생성자는 NonDeterministic(첫 다른 필드 = serial)·InvalidArgument이고 기억한다(다시 불러도
// 생성자를 돌리지 않는다). non-null Soft 기본값은 NonNullReference(경로 =
// target)·InvalidArgument다.
TEST(ClassDefaults_ConstructorContractViolations) {
    SystemAllocator allocator;
    DefaultsFixture fixture(allocator);
    TypeLease counter = fixture.Lease(TypeIdOf<DefaultsCounter>());
    ClassDefaultsDiagnostic why;
    auto first = AcquireClassDefaults(counter, allocator, &why);
    CHECK(!first && first.Error() == ErrorCode::InvalidArgument &&
          why.reason == ClassDefaultsReason::NonDeterministic && why.fieldPathSteps == 1 &&
          why.fieldPath[0].fieldId == FieldNamed<DefaultsCounter>("serial"));
    const int32 created = DefaultsCounter::created;
    ClassDefaultsDiagnostic remembered;
    auto second = AcquireClassDefaults(counter, allocator, &remembered);
    CHECK(!second && remembered.reason == ClassDefaultsReason::NonDeterministic &&
          remembered.fieldPathSteps == 1 && DefaultsCounter::created == created);

    TypeLease soft = fixture.Lease(TypeIdOf<DefaultsSoftLink>());
    ClassDefaultsDiagnostic softWhy;
    auto rejected = AcquireClassDefaults(soft, allocator, &softWhy);
    CHECK(!rejected && rejected.Error() == ErrorCode::InvalidArgument &&
          softWhy.reason == ClassDefaultsReason::NonNullReference && softWhy.fieldPathSteps >= 1 &&
          softWhy.fieldPath[0].fieldId == FieldNamed<DefaultsSoftLink>("target"));

    // struct 멤버(slot.target)·배열 원소(list[0]) 안의 non-null Soft도 같은 hook이 잡는다.
    TypeLease nested = fixture.Lease(TypeIdOf<DefaultsSoftNested>());
    ClassDefaultsDiagnostic nestedWhy;
    CHECK(!AcquireClassDefaults(nested, allocator, &nestedWhy) &&
          nestedWhy.reason == ClassDefaultsReason::NonNullReference &&
          nestedWhy.fieldPathSteps == 2 &&
          nestedWhy.fieldPath[0].fieldId == FieldNamed<DefaultsSoftNested>("slot") &&
          nestedWhy.fieldPath[1].fieldId ==
              FieldNamed<reflection_fixture::DefaultsSoftSlot>("target"));
    TypeLease list = fixture.Lease(TypeIdOf<DefaultsSoftList>());
    ClassDefaultsDiagnostic listWhy;
    CHECK(!AcquireClassDefaults(list, allocator, &listWhy) &&
          listWhy.reason == ClassDefaultsReason::NonNullReference && listWhy.fieldPathSteps >= 1 &&
          listWhy.fieldPath[0].fieldId == FieldNamed<DefaultsSoftList>("list"));
}

// 할당 실패: scratch의 모든 할당 실패는 OutOfMemory이고 기억하지 않아 다음 호출이 만든다.
// TypeRegistry allocator(게시 복사)의 실패도 기억하지 않는다.
TEST(ClassDefaults_AllocationFailures) {
    SystemAllocator allocator;
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        DefaultsFixture fixture(allocator);
        TypeLease lease = fixture.Lease(TypeIdOf<DefaultsHolder>());
        test::NFailAllocator failing(allocator);
        failing.FailOnAttempt(attempt);
        ClassDefaultsDiagnostic why;
        auto defaults = AcquireClassDefaults(lease, failing, &why);
        const bool exhausted = failing.Attempts() < attempt;
        failing.DisableFailure();
        if (exhausted) {
            CHECK(defaults);
            break;
        }

        CHECK(!defaults && defaults.Error() == ErrorCode::OutOfMemory);
        auto retried = AcquireClassDefaults(lease, allocator);
        CHECK(retried);
        ++failures;
    }

    CHECK(failures > 0);

    // 게시 복사(TypeRegistry allocator) 실패 — builder는 끝났지만 게시하지 못한다.
    test::NFailAllocator registryAllocator(allocator);
    {
        TypeRegistry types(registryAllocator);
        ModuleRegistration registration(types, registryAllocator);
        REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(registration));
        auto acquired = types.Acquire(TypeIdOf<DefaultsHolder>());
        REQUIRE(acquired);
        registryAllocator.FailOnAttempt(1);
        auto failed = AcquireClassDefaults(acquired.Value(), allocator);
        registryAllocator.DisableFailure();
        CHECK(!failed && failed.Error() == ErrorCode::OutOfMemory);
        CHECK(AcquireClassDefaults(acquired.Value(), allocator));
        acquired.Value().Reset();
        CHECK(registration.Unregister());
        CHECK(types.Shutdown());
    }
}

// 다른 스레드가 만든 레코드를 이 스레드가 받는다(builder 한 번 — 생성 잠금·다시 보기).
TEST(ClassDefaults_PublishedAcrossThreads) {
    SystemAllocator allocator;
    DefaultsFixture fixture(allocator);
    TypeLease lease = fixture.Lease(TypeIdOf<DefaultsHolder>());
    struct Worker {
        const TypeLease* lease = nullptr;
        CountingBuild* build = nullptr;
        const ClassDefaults* result = nullptr;
    };
    CountingBuild build;
    Worker worker{&lease, &build, nullptr};
    platform::Thread thread;
    auto entry = [](void* user) noexcept {
        auto& self = *static_cast<Worker*>(user);
        auto resolved = self.lease->ResolveClassDefaults(&CountingBuilder, self.build);
        self.result = resolved ? resolved.Value() : nullptr;
    };
    REQUIRE(thread.Start(entry, &worker) == platform::ThreadStatus::Success);
    REQUIRE(thread.Join() == platform::ThreadStatus::Success);
    REQUIRE(worker.result != nullptr);
    auto here = lease.ResolveClassDefaults(&CountingBuilder, &build);
    REQUIRE(here);
    CHECK(here.Value() == worker.result && build.calls == 1);
    CHECK(test::graph::SameBytes(here.Value()->Record(ClassDefaultsConfiguration::Editor),
                                 Span<const uint8>(build.bytes, 1)));
}

// 경로 조회: 필드·비Atomic struct 멤버·두 단계 멤버는 payload, Atomic struct 통째는 payload이고 그
// 멤버는 InvalidArgument, 원소 단계(정적 배열·배열)는 Unsupported, cooked의 EditorOnly 필드·멤버는
// NotFound, struct가 아닌 값 뒤 단계는 TypeMismatch, 빈 경로는 InvalidArgument.
TEST(ClassDefaults_FindDefaultPayload) {
    SystemAllocator allocator;
    DefaultsFixture fixture(allocator);
    TypeLease lease = fixture.Lease(TypeIdOf<DefaultsHolder>());
    auto defaults = AcquireClassDefaults(lease, allocator);
    REQUIRE(defaults);
    const ClassDefaults& record = *defaults.Value();
    const auto find = [&](StringView text, ClassDefaultsConfiguration configuration) noexcept {
        PropertyPathBuffer path;
        if (!ParsePropertyPath(lease, text, path)) {
            platform::Fatal("test path parse failed");
        }

        return FindDefaultPayload(lease, record, path, configuration, allocator);
    };
    constexpr auto Editor = ClassDefaultsConfiguration::Editor;
    constexpr auto Cooked = ClassDefaultsConfiguration::Cooked;
    // count = 42(I32 LE).
    auto count = find("count", Editor);
    REQUIRE(count);
    CHECK(count.Value().wire == static_cast<uint32>(WireType::I32) &&
          count.Value().bytes.Size() == 4 && count.Value().bytes[0] == 42);
    auto member = find("point.x", Editor);
    REQUIRE(member);
    CHECK(member.Value().bytes.Size() == 4 && member.Value().bytes[0] == 3);
    auto deep = find("nest.inner.x", Editor);
    REQUIRE(deep);
    CHECK(deep.Value().bytes[0] == 3);
    auto atomic = find("pair", Editor);
    CHECK(atomic && atomic.Value().wire == static_cast<uint32>(WireType::Record));
    CHECK(find("pair.u", Editor).Error() == ErrorCode::InvalidArgument);
    CHECK(find("fixed[1]", Editor).Error() == ErrorCode::Unsupported);
    CHECK(find("editorValue", Cooked).Error() == ErrorCode::NotFound);
    CHECK(find("point.note", Cooked).Error() == ErrorCode::NotFound);
    CHECK(find("point.note", Editor));
    CHECK(find("count", Cooked));
    PropertyPathBuffer empty;
    CHECK(FindDefaultPayload(lease, record, empty, Editor, allocator).Error() ==
          ErrorCode::InvalidArgument);
}

// 전부 만들기: 구체 클래스마다 한 번(추상은 빠진다), 실패도 진단과 함께 넘긴다.
TEST(ClassDefaults_ForEach) {
    SystemAllocator allocator;
    DefaultsFixture fixture(allocator);
    struct Seen {
        bool holder = false;
        bool abstract = false;
        bool counterFailed = false;
        usize total = 0;
    } seen;
    auto visited = ForEachClassDefaults(
        fixture.types, allocator,
        [](void* user, TypeId type, const ClassDefaults* defaults,
           const ClassDefaultsDiagnostic& diagnostic) noexcept {
            auto& state = *static_cast<Seen*>(user);
            ++state.total;
            state.holder = state.holder || (type == TypeIdOf<DefaultsHolder>() && defaults);
            state.abstract = state.abstract || type == TypeIdOf<DefaultsAbstractBase>();
            state.counterFailed = state.counterFailed ||
                                  (type == TypeIdOf<DefaultsCounter>() && defaults == nullptr &&
                                   diagnostic.reason == ClassDefaultsReason::NonDeterministic);
            return Result<void>::Success();
        },
        &seen);
    CHECK(visited);
    CHECK(seen.holder && !seen.abstract && seen.counterFailed && seen.total > 3);
}

} // namespace class_defaults_tests_file
