#include <TestSupport/Test.h>
#include "GraphExternalTestSupport.h"

// graph v2 외부 참조 저장 시험이다(Docs/Design/ObjectFoundations.ko.md 5.1·5.3절, 9절 Oe "Admit"·
// "AssignExternalIndices"): 첫 출현 결정과 집계(occurrences·commonFlags·required), slot 첫 출현 순,
// Reject·Null·hook 실패·정의되지 않은 결과, 한도와 옵션, 필드 writer 계약, 저장 byte = 손으로
// 조립한 byte(최상위 필드·배열 원소·struct 멤버·둘째 노드·NodeTable), 번호 재배정(역순·임의
// 순열·outer import·되돌리기·잘못된 표의 불변), 위치 표 메모리의 바이트·작업량 청구.
using namespace rocks;
namespace graph_external_tests_file {

using namespace rocks::test::external;

namespace {

// 저장 집합 {A, B}와 집합 밖 X·Y·Z다. A.link = X(Required), A.editor = Z(EditorOnly),
// A.links = [Y, X, null, Y], A.hold.target = Y, B.link = Y, B.editor = Z, B.links = [X]. 첫 출현
// 순서는 X(A.link) → Z(A.editor) → Y(A.links[0])다.
struct Scene {
    explicit Scene(Allocator& allocator) noexcept : roots(allocator) {
    }

    bool Build(ObjectRegistry& registry) noexcept {
        auto made = [&](ObjectHandle& out) noexcept {
            auto created = New(registry, roots);
            out = created ? created.Value() : ObjectHandle{};
            return created.IsOk();
        };

        return made(a) && made(b) && made(x) && made(y) && made(z) && SetNumber(registry, a, 1) &&
               SetNumber(registry, b, 2) && SetLink(registry, a, x) && SetEditor(registry, a, z) &&
               AddLink(registry, a, y) && AddLink(registry, a, x) &&
               AddLink(registry, a, ObjectHandle{}) && AddLink(registry, a, y) &&
               SetHold(registry, a, y) && SetLink(registry, b, y) && SetEditor(registry, b, z) &&
               AddLink(registry, b, x);
    }

    Array<RootRef<Object>> roots;
    ObjectHandle a{};
    ObjectHandle b{};
    ObjectHandle x{};
    ObjectHandle y{};
    ObjectHandle z{};
};

// 그래프를 단계형으로 복원해 Commit한다(roots = 모든 노드). nodes·uses에 노드 handle·쓰임을
// 복사한다. 실패하면 그 오류다.
ErrorCode Restore(Allocator& allocator, ObjectRegistry& registry, TypeRegistry& types,
                  Span<const uint8> bytes, Span<const TypeId> importTypes,
                  Span<const ObjectHandle> imports, Array<RootRef<Object>>& held,
                  Array<ObjectHandle>& nodes, Array<GraphExternalUse>* uses = nullptr) noexcept {
    ObjectGraphStage stage(allocator, registry, types, registry.OwnerAccess());
    ObjectGraphInput input;
    input.bytes = bytes;
    input.importTypes = importTypes;
    if (auto valid = stage.Validate(input); !valid) {
        return valid.Error();
    }

    if (uses != nullptr && !uses->TryAppend(stage.ExternalUses())) {
        return ErrorCode::OutOfMemory;
    }

    ConstructionContext transaction(registry, registry.OwnerAccess());
    if (auto created = stage.CreateObjects(transaction); !created) {
        return created.Error();
    }

    if (auto applied = stage.ApplyFields(transaction, imports); !applied) {
        return applied.Error();
    }

    if (auto checked = stage.CheckRequired(transaction); !checked) {
        return checked.Error();
    }

    if (!nodes.TryAppend(stage.Nodes()) || !held.TryResize(stage.Nodes().Size())) {
        return ErrorCode::OutOfMemory;
    }

    return ErrorOf(transaction.Commit(stage.Nodes(), held.View()));
}

bool SameExternal(const GraphExternal& external, ObjectHandle target, uint32 occurrences,
                  uint32 commonFlags, bool required) noexcept {
    return external.target == target && external.type == ItemType &&
           external.occurrences == occurrences && external.commonFlags == commonFlags &&
           external.required == required;
}

bool SameUse(const GraphExternalUse& use, const GraphExternal& external) noexcept {
    return use.occurrences == external.occurrences && use.commonFlags == external.commonFlags &&
           use.required == external.required;
}

// 저장이 성공하는 가장 작은 maxWork다(이분 탐색 — 실패하면 0).
uint64 MinimumSaveWork(Allocator& allocator, ObjectRegistry& registry, Span<const ObjectHandle> set,
                       ObjectGraphSaveOptions options) noexcept {
    if (!SaveObjectGraphSet(allocator, registry, registry.OwnerAccess(), set, options)) {
        return 0;
    }

    uint64 low = 0;
    uint64 high = ObjectGraphLimits{}.maxWork;
    while (low + 1 < high) {
        const uint64 middle = low + (high - low) / 2;
        options.limits.maxWork = middle;
        if (SaveObjectGraphSet(allocator, registry, registry.OwnerAccess(), set, options)) {
            high = middle;
        } else {
            low = middle;
        }
    }

    return high;
}

// 필드 writer 계약 시험용 타입이다: target 참조를 임시 writer에 써서 필드 writer로 복사하는 수동
// codec(외부 참조면 위치를 기록할 수 없어 InvalidArgument여야 한다).
constexpr TypeId DetourType{0x4745585454ULL, 9};
constexpr Guid DetourTargetId{0x4745585446ULL, 9};

struct Detour final : Object {
    explicit Detour(Allocator&) noexcept {
    }

    void Trace(TraceVisitor& visitor) const noexcept {
        visitor.Visit(target);
    }

    ObjectRef<Item> target;
};

bool RegisterDetour(TypeRegistry& types) noexcept {
    const FieldSchema fields[] = {
        Scalar(DetourTargetId, "target", 6, ValueKind::ObjectReference, ItemType)};
    RuntimeFieldOps ops = ScalarField<&Detour::target>(DetourTargetId);
    ops.encode = +[](const RuntimeFieldOps&, ObjectRegistry* registry,
                     const ObjectAccessContext& access, ObjectHandle object, ArchiveWriter& out,
                     FieldEncodeContext& context) noexcept -> Result<uint32> {
        auto owner = registry->Resolve<Detour>(access, object);
        if (!owner) {
            return Result<uint32>::Failure(owner.Error());
        }

        const ObjectHandle target = owner.Value().Get().Value()->target.Handle();
        ArchiveWriter detour(context.Scratch());
        if (auto written =
                context.WriteReference(detour, target, {ItemType, NativeTypeKey<Item>()});
            !written) {
            return Result<uint32>::Failure(written.Error());
        }

        if (auto copied = out.WriteRaw(detour.View()); !copied) {
            return Result<uint32>::Failure(copied.Error());
        }

        return Result<uint32>::Success(static_cast<uint32>(WireType::U32));
    };
    const RuntimeFieldOps all[] = {ops};
    auto runtime = MakeRuntimeTypeOps<Detour>();
    runtime.fields = all;
    runtime.completeFieldLayout = true;
    return types.Register({DetourType, 1, fields, {"Detour", 6}}, Move(runtime)).IsOk();
}

// 기대 타입이 기반(Mark)인 참조 하나를 가진 타입이다(hook 정보의 targetType·expectedType 구별).
constexpr TypeId AnchorType{0x4745585454ULL, 10};
constexpr Guid AnchorMarkId{0x4745585446ULL, 10};

struct Anchor final : Object {
    explicit Anchor(Allocator&) noexcept {
    }

    void Trace(TraceVisitor& visitor) const noexcept {
        visitor.Visit(mark);
    }

    ObjectRef<Mark> mark;
};

bool RegisterAnchor(TypeRegistry& types) noexcept {
    const FieldSchema fields[] = {
        Scalar(AnchorMarkId, "mark", 4, ValueKind::ObjectReference, MarkType)};
    const RuntimeFieldOps ops[] = {ScalarField<&Anchor::mark>(AnchorMarkId)};
    auto runtime = MakeRuntimeTypeOps<Anchor>();
    runtime.fields = ops;
    runtime.completeFieldLayout = true;
    return types.Register({AnchorType, 1, fields, {"Anchor", 6}}, Move(runtime)).IsOk();
}

// Item Record다(필드 순서 = 스키마 순서). links는 U32 ValueArray, hold는 Hold 중첩 Record다.
Array<uint8> ItemRecord(Allocator& allocator, uint32 number, uint32 link, Span<const uint32> links,
                        uint32 holdTarget) noexcept {
    auto u32 = [&](uint32 value) noexcept {
        ArchiveWriter payload(allocator);
        if (!payload.WriteU32(value)) {
            platform::Fatal("External test payload failed");
        }

        return payload.TakeBytes();
    };
    RecordBuilder hold(allocator, HoldType, 1);
    ArchiveWriter array(allocator);
    bool ok = hold.AddField(HoldTargetId, WireType::U32, u32(holdTarget).View()).IsOk() &&
              hold.AddField(HoldTagId, WireType::U32, u32(0).View()).IsOk() &&
              array.WriteU32(static_cast<uint32>(WireType::U32)).IsOk() &&
              array.WriteU32(static_cast<uint32>(links.Size())).IsOk();
    for (uint32 value : links) {
        ok = ok && array.WriteU32(value).IsOk();
    }

    auto holdBytes = hold.Build();
    RecordBuilder record(allocator, ItemType, 1);
    ok = ok && holdBytes && record.AddField(NumberId, WireType::U32, u32(number).View()).IsOk() &&
         record.AddField(LinkId, WireType::U32, u32(link).View()).IsOk() &&
         record.AddField(EditorId, WireType::U32, u32(0).View()).IsOk() &&
         record.AddField(LinksId, WireType::ValueArray, array.View()).IsOk() &&
         record.AddField(HoldId, WireType::Record, holdBytes.Value().View()).IsOk();
    auto built = record.Build();
    if (!ok || !built) {
        platform::Fatal("External test record failed");
    }

    return Move(built.Value());
}

} // namespace

// Admit: hook은 대상의 첫 출현에서 한 번 불리고(Import면 같은 대상의 이후 출현은 hook 없이 같은
// slot), slot은 첫 출현 순이다. 대상마다 출현 수·위치 유효 플래그의 AND·Required 여부를 모으고,
// 복원 1단계의 ExternalUses가 같은 집계다. 복원은 imports로 같은 대상들을 잇는다(단일 참조·배열
// 원소· struct 멤버·둘째 노드).
TEST(GraphExternal_AdmitDecidesAtFirstOccurrenceAndAggregates) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry source(allocator, types, {0x474578, 1});
        ObjectRegistry target(allocator, types, {0x474578, 2});
        {
            Scene scene(allocator);
            REQUIRE(scene.Build(source));
            Admitter hooks;
            const ObjectHandle set[] = {scene.a, scene.b};
            auto saved = SaveObjectGraphSet(allocator, source, source.OwnerAccess(), set,
                                            AdmitOptions(&hooks));
            REQUIRE(saved);
            CHECK(hooks.reports == 0);
            REQUIRE(hooks.calls == 3);
            const Admitter::Call& first = hooks.entries[0];
            CHECK(first.target == scene.x && first.slot == 1 && first.required &&
                  first.flags == Required && first.objectIndex == 0 && first.depth == 1 &&
                  first.field == LinkId && !first.indexed && first.expectedType == ItemType &&
                  first.targetType == ItemType);
            const Admitter::Call& second = hooks.entries[1];
            CHECK(second.target == scene.z && second.slot == 2 && !second.required &&
                  second.flags == EditorOnly && second.field == EditorId);
            const Admitter::Call& third = hooks.entries[2];
            CHECK(third.target == scene.y && third.slot == 3 && !third.required &&
                  third.flags == 0 && third.field == LinksId && third.indexed && third.index == 0);

            const Span<const GraphExternal> externals = saved.Value().Externals();
            REQUIRE(externals.Size() == 3);
            // X: A.link(Required)·A.links[1]·B.links[0], Z: EditorOnly 위치에서만, Y: A.links[0]·
            // A.links[3]·A.hold.target·B.link(Required).
            CHECK(SameExternal(externals[0], scene.x, 3, 0, true));
            CHECK(SameExternal(externals[1], scene.z, 2, EditorOnly, false));
            CHECK(SameExternal(externals[2], scene.y, 4, 0, true));
            CHECK(test::graph::ReadU32(saved.Value().Bytes(), test::graph::ExternalCountOffset) ==
                  3);

            Array<RootRef<Object>> made(allocator);
            auto x = New(target, made);
            auto z = New(target, made);
            auto y = New(target, made);
            REQUIRE(x && z && y);
            const ObjectHandle imports[] = {x.Value(), z.Value(), y.Value()};
            const Array<TypeId> importTypes = ImportTypes(allocator, 3);
            Array<RootRef<Object>> held(allocator);
            Array<ObjectHandle> nodes(allocator);
            Array<GraphExternalUse> uses(allocator);
            REQUIRE(Restore(allocator, target, types, saved.Value().Bytes(), importTypes.View(),
                            imports, held, nodes, &uses) == ErrorCode::None);
            REQUIRE(uses.Size() == 3 && nodes.Size() == 2);
            for (usize index = 0; index < 3; ++index) {
                CHECK(SameUse(uses[index], externals[index]));
            }

            const Seen a = Look(target, nodes[0]);
            CHECK(a.number == 1 && a.link == x.Value() && a.editor == z.Value() &&
                  a.hold == y.Value() && a.linkCount == 4 && a.links[0] == y.Value() &&
                  a.links[1] == x.Value() && a.links[2] == ObjectHandle{} &&
                  a.links[3] == y.Value());
            const Seen b = Look(target, nodes[1]);
            CHECK(b.number == 2 && b.link == y.Value() && b.editor == z.Value() &&
                  b.linkCount == 1 && b.links[0] == x.Value());
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// Reject 정책은 hook 없이 진단(value 0) 뒤 InvalidArgument, Admit에 hook이 없으면 모두 Reject다.
// hook의 Reject는 그 위치의 진단(value 0) 뒤 InvalidArgument, 실패는 그 오류, 정의되지 않은 결과는
// InvalidArgument다. Null은 그 위치를 0으로 쓰고 진단(value 1)을 남기며 slot을 쓰지 않고(다음
// 대상이 같은 slot을 받는다) 같은 대상의 다음 출현에서 hook을 다시 부른다. Required 위치의 Null은
// rejectMissingRequired면 RequiredMissing 뒤 InvalidArgument다.
TEST(GraphExternal_RejectNullAndHookFailure) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry source(allocator, types, {0x474578, 3});
        ObjectRegistry target(allocator, types, {0x474578, 4});
        {
            Scene scene(allocator);
            REQUIRE(scene.Build(source));
            const ObjectHandle set[] = {scene.a, scene.b};
            const auto access = source.OwnerAccess();
            {
                Admitter hooks;
                ObjectGraphSaveOptions options = AdmitOptions(&hooks);
                options.externals = GraphExternalPolicy::Reject;
                CHECK(SaveObjectGraphSet(allocator, source, access, set, options).Error() ==
                      ErrorCode::InvalidArgument);
                CHECK(hooks.calls == 0 && hooks.reports == 1);
                CHECK(hooks.diagnostics[0].kind == FieldDiagnosticKind::ExternalReference &&
                      hooks.diagnostics[0].value == 0 && hooks.diagnostics[0].field == LinkId);
                CHECK(SaveObjectGraphSet(allocator, source, access, set, AdmitOptions(nullptr))
                          .Error() == ErrorCode::InvalidArgument);
            }

            {
                Admitter hooks;
                const ObjectHandle rejects[] = {scene.y};
                hooks.rejects = rejects;
                CHECK(SaveObjectGraphSet(allocator, source, access, set, AdmitOptions(&hooks))
                          .Error() == ErrorCode::InvalidArgument);
                CHECK(hooks.calls == 3 && hooks.reports == 1);
                CHECK(hooks.diagnostics[0].kind == FieldDiagnosticKind::ExternalReference &&
                      hooks.diagnostics[0].value == 0 && hooks.diagnostics[0].objectIndex == 0 &&
                      hooks.diagnostics[0].field == LinksId);
            }

            {
                Admitter hooks;
                hooks.failure = ErrorCode::DependencyFailed;
                CHECK(SaveObjectGraphSet(allocator, source, access, set, AdmitOptions(&hooks))
                          .Error() == ErrorCode::DependencyFailed);
                CHECK(hooks.calls == 1 && hooks.reports == 0);
                Admitter odd;
                odd.undefined = true;
                CHECK(SaveObjectGraphSet(allocator, source, access, set, AdmitOptions(&odd))
                          .Error() == ErrorCode::InvalidArgument);
                CHECK(odd.calls == 1 && odd.reports == 0);
            }

            {
                // Z(EditorOnly 위치 둘)만 Null이다: hook은 X·Z·Y·Z 네 번, Z는 slot을 쓰지 않는다.
                Admitter hooks;
                const ObjectHandle nulls[] = {scene.z};
                hooks.nulls = nulls;
                auto saved =
                    SaveObjectGraphSet(allocator, source, access, set, AdmitOptions(&hooks));
                REQUIRE(saved);
                REQUIRE(hooks.calls == 4);
                CHECK(hooks.entries[0].target == scene.x && hooks.entries[0].slot == 1);
                CHECK(hooks.entries[1].target == scene.z && hooks.entries[1].slot == 2);
                CHECK(hooks.entries[2].target == scene.y && hooks.entries[2].slot == 2);
                CHECK(hooks.entries[3].target == scene.z && hooks.entries[3].slot == 3 &&
                      hooks.entries[3].objectIndex == 1);
                REQUIRE(hooks.reports == 2);
                for (usize index = 0; index < 2; ++index) {
                    CHECK(hooks.diagnostics[index].kind == FieldDiagnosticKind::ExternalReference &&
                          hooks.diagnostics[index].value == 1 &&
                          hooks.diagnostics[index].objectIndex == index &&
                          hooks.diagnostics[index].field == EditorId);
                }

                const Span<const GraphExternal> externals = saved.Value().Externals();
                REQUIRE(externals.Size() == 2);
                CHECK(externals[0].target == scene.x && externals[1].target == scene.y);
                CHECK(test::graph::ReadU32(saved.Value().Bytes(),
                                           test::graph::ExternalCountOffset) == 2);
                Array<RootRef<Object>> made(allocator);
                auto x = New(target, made);
                auto y = New(target, made);
                REQUIRE(x && y);
                const ObjectHandle imports[] = {x.Value(), y.Value()};
                const Array<TypeId> importTypes = ImportTypes(allocator, 2);
                Array<RootRef<Object>> held(allocator);
                Array<ObjectHandle> nodes(allocator);
                REQUIRE(Restore(allocator, target, types, saved.Value().Bytes(), importTypes.View(),
                                imports, held, nodes) == ErrorCode::None);
                const Seen a = Look(target, nodes[0]);
                CHECK(a.link == x.Value() && a.editor == ObjectHandle{} && a.links[0] == y.Value());
                CHECK(Look(target, nodes[1]).editor == ObjectHandle{});
            }

            {
                // Required 위치(A.link)의 Null: rejectMissingRequired면 진단 둘 뒤 InvalidArgument,
                // 아니면 null로 쓴다.
                Admitter hooks;
                const ObjectHandle nulls[] = {scene.x};
                hooks.nulls = nulls;
                ObjectGraphSaveOptions options = AdmitOptions(&hooks);
                options.policy.rejectMissingRequired = true;
                CHECK(SaveObjectGraphSet(allocator, source, access, set, options).Error() ==
                      ErrorCode::InvalidArgument);
                REQUIRE(hooks.reports == 2);
                CHECK(hooks.diagnostics[0].kind == FieldDiagnosticKind::ExternalReference &&
                      hooks.diagnostics[0].value == 1 && hooks.diagnostics[0].field == LinkId);
                CHECK(hooks.diagnostics[1].kind == FieldDiagnosticKind::RequiredMissing &&
                      hooks.diagnostics[1].field == LinkId);
                Admitter lenient;
                lenient.nulls = nulls;
                CHECK(SaveObjectGraphSet(allocator, source, access, set, AdmitOptions(&lenient)));
            }
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 한도와 옵션: 서로 다른 외부 대상이 maxExternalReferences를 넘으면 hook의 Import 뒤
// CapacityExceeded이고, maxExternalReferences가 2^31 − 1을 넘으면 저장·복원 모두 옵션
// InvalidArgument다. 대상 검사(기대 형식)는 hook보다 먼저다. 외부 참조를 직렬화기가 준 필드 writer
// 밖에 쓰는 codec은 InvalidArgument(노드 참조는 그대로 된다).
TEST(GraphExternal_LimitsOptionsAndWriterContract) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        REQUIRE(RegisterDetour(types));
        ObjectRegistry source(allocator, types, {0x474578, 5});
        {
            Scene scene(allocator);
            REQUIRE(scene.Build(source));
            const ObjectHandle set[] = {scene.a, scene.b};
            const auto access = source.OwnerAccess();
            Admitter hooks;
            ObjectGraphSaveOptions options = AdmitOptions(&hooks);
            options.limits.maxExternalReferences = 2;
            CHECK(SaveObjectGraphSet(allocator, source, access, set, options).Error() ==
                  ErrorCode::CapacityExceeded);
            CHECK(hooks.calls == 3);
            options.limits.maxExternalReferences = 3;
            CHECK(SaveObjectGraphSet(allocator, source, access, set, options));
            options.limits.maxExternalReferences = 0x80000000u;
            CHECK(SaveObjectGraphSet(allocator, source, access, set, options).Error() ==
                  ErrorCode::InvalidArgument);
            auto saved = SaveObjectGraphSet(allocator, source, access, set, AdmitOptions(&hooks));
            REQUIRE(saved);
            ObjectGraphLoadOptions huge;
            huge.limits.maxExternalReferences = 0x80000000u;
            ObjectGraphStage stage(allocator, source, types, access, huge);
            ObjectGraphInput input;
            input.bytes = saved.Value().Bytes();
            const Array<TypeId> importTypes = ImportTypes(allocator, 3);
            input.importTypes = importTypes.View();
            CHECK(stage.Validate(input).Error() == ErrorCode::InvalidArgument);
            CHECK(stage.ExternalUses().Size() == 0);
            // 편의 복원: 노드 하나(A — 외부 대상 X·Z·Y)를 imports 셋으로 복원하면 성공하고, 같은
            // 입력에 옵션 한도만 넘기면 InvalidArgument다(개수 불일치와 구별된다).
            const ObjectHandle single[] = {scene.a};
            auto lone = SaveObjectGraphSet(allocator, source, access, single, AdmitOptions(&hooks));
            REQUIRE(lone);
            REQUIRE(lone.Value().Externals().Size() == 3);
            const ObjectHandle imports[] = {scene.x, scene.z, scene.y};
            ObjectGraphLoadOptions load;
            load.imports = imports;
            CHECK(LoadObjectGraph(allocator, source, types, access, lone.Value().Bytes(), load));
            huge.imports = imports;
            CHECK(LoadObjectGraph(allocator, source, types, access, lone.Value().Bytes(), huge)
                      .Error() == ErrorCode::InvalidArgument);

            // 필드 writer 계약: 노드 참조는 임시 writer에 써도 되고 외부 참조는 InvalidArgument다.
            Array<RootRef<Object>> roots(allocator);
            auto detour = New(source, roots, DetourType);
            REQUIRE(detour);
            {
                auto owner = source.Resolve<Detour>(access, detour.Value());
                REQUIRE(owner);
                REQUIRE(source.SetReference(access, detour.Value(),
                                            owner.Value().Get().Value()->target, scene.x));
            }

            const ObjectHandle inside[] = {detour.Value(), scene.x};
            ObjectGraphSaveOptions plain;
            plain.writeNodeTable = true;
            CHECK(SaveObjectGraphSet(allocator, source, access, inside, plain));
            const ObjectHandle alone[] = {detour.Value()};
            Admitter admit;
            CHECK(SaveObjectGraphSet(allocator, source, access, alone, AdmitOptions(&admit))
                      .Error() == ErrorCode::InvalidArgument);
            CHECK(admit.calls == 1);
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// hook 정보의 타입 칸: 기대 타입이 기반(Mark)인 위치에 파생(Item) 대상이면 targetType = Item,
// expectedType = Mark다(GraphExternal.type도 실제 타입). 마지막 생존 재확인은 외부 대상도 본다: 한
// 번만 나오는 외부 대상을 hook이 허락을 묻는 동안 파괴 요청하면(계약 위반 모의 — 이후 출현이 없어
// 참조마다의 생존 조회로는 드러나지 않는다) 저장이 Destroyed다.
TEST(GraphExternal_HookTypesAndFinalLivenessOfTargets) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        REQUIRE(RegisterAnchor(types));
        ObjectRegistry source(allocator, types, {0x474578, 10});
        {
            const auto access = source.OwnerAccess();
            Array<RootRef<Object>> roots(allocator);
            auto anchor = New(source, roots, AnchorType);
            auto item = New(source, roots);
            REQUIRE(anchor && item);
            {
                auto owner = source.Resolve<Anchor>(access, anchor.Value());
                REQUIRE(owner);
                REQUIRE(source.SetReference(access, anchor.Value(),
                                            owner.Value().Get().Value()->mark, item.Value()));
            }

            const ObjectHandle set[] = {anchor.Value()};
            Admitter hooks;
            auto saved = SaveObjectGraphSet(allocator, source, access, set, AdmitOptions(&hooks));
            REQUIRE(saved);
            REQUIRE(hooks.calls == 1);
            CHECK(hooks.entries[0].targetType == ItemType &&
                  hooks.entries[0].expectedType == MarkType);
            REQUIRE(saved.Value().Externals().Size() == 1);
            CHECK(saved.Value().Externals()[0].type == ItemType);

            Admitter destroying;
            destroying.destroyIn = &source;
            destroying.destroyTarget = item.Value();
            CHECK(SaveObjectGraphSet(allocator, source, access, set, AdmitOptions(&destroying))
                      .Error() == ErrorCode::Destroyed);
            CHECK(destroying.calls == 1);
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 저장 byte가 명세와 같다(5.1·5.3절): header externalCount = slot 수, 외부 참조 값 0x80000000 |
// slot(최상위 필드·ValueArray 원소·중첩 Record 멤버·둘째 노드, NodeTable 뒤). 번호 재배정은 그
// 값들과 header만 제자리에서 바꾸고(손으로 조립한 재배정 byte와 같다), 원래 번호로 다시 매기면 처음
// byte로 돌아온다.
TEST(GraphExternal_SavedBytesFollowTheSpecification) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry source(allocator, types, {0x474578, 6});
        {
            Array<RootRef<Object>> roots(allocator);
            auto a = New(source, roots, ItemType, {}, "A");
            auto b = New(source, roots);
            auto x = New(source, roots);
            auto y = New(source, roots);
            REQUIRE(a && b && x && y);
            REQUIRE(SetNumber(source, a.Value(), 7));
            REQUIRE(SetLink(source, a.Value(), x.Value()));
            REQUIRE(AddLink(source, a.Value(), x.Value()));
            REQUIRE(AddLink(source, a.Value(), ObjectHandle{}));
            REQUIRE(SetHold(source, a.Value(), y.Value()));
            REQUIRE(SetLink(source, b.Value(), y.Value()));
            Admitter hooks;
            const ObjectHandle set[] = {a.Value(), b.Value()};
            auto saved = SaveObjectGraphSet(allocator, source, source.OwnerAccess(), set,
                                            AdmitOptions(&hooks));
            REQUIRE(saved);
            REQUIRE(saved.Value().Externals().Size() == 2);

            constexpr uint32 Bit = test::graph::ExternalBit;
            auto expected = [&](uint32 xIndex, uint32 yIndex, uint32 count) noexcept {
                const uint32 aLinks[] = {Bit | xIndex, 0};
                const Array<uint8> recordA =
                    ItemRecord(allocator, 7, Bit | xIndex, aLinks, Bit | yIndex);
                const Array<uint8> recordB = ItemRecord(allocator, 0, Bit | yIndex, {}, 0);
                const test::graph::Node nodes[] = {{ItemType, 0, "A", recordA.View()},
                                                   {ItemType, 0, {}, recordB.View()}};
                test::graph::Header header;
                header.externalCount = count;
                auto built = test::graph::Build(allocator, nodes, header);
                if (!built) {
                    platform::Fatal("External test graph failed");
                }

                return Move(built.Value());
            };

            const Array<uint8> original = expected(1, 2, 2);
            CHECK(test::graph::SameBytes(saved.Value().Bytes(), original.View()));
            const uint32 shuffled[] = {4, 1};
            REQUIRE(saved.Value().AssignExternalIndices(shuffled, 5));
            CHECK(test::graph::SameBytes(saved.Value().Bytes(), expected(4, 1, 5).View()));
            const uint32 identity[] = {1, 2};
            REQUIRE(saved.Value().AssignExternalIndices(identity, 2));
            CHECK(test::graph::SameBytes(saved.Value().Bytes(), original.View()));
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 번호 재배정: 잘못된 표(길이·0·count 초과·중복)는 InvalidArgument, count가 maxExternalReferences를
// 넘으면 CapacityExceeded이고 실패하면 byte가 그대로다. 역순·임의 순열(outer import로 count > slot
// 수 — 쓰이지 않는 번호의 쓰임은 0)로 재배정한 뒤 복원이 같은 대상을 잇는다. TakeBytes 뒤에는
// InvalidArgument다(외부 참조 목록은 남는다).
TEST(GraphExternal_AssignExternalIndicesValidatesAndPermutes) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry source(allocator, types, {0x474578, 7});
        ObjectRegistry target(allocator, types, {0x474578, 8});
        {
            Scene scene(allocator);
            REQUIRE(scene.Build(source));
            Admitter hooks;
            const ObjectHandle set[] = {scene.a, scene.b};
            auto saved = SaveObjectGraphSet(allocator, source, source.OwnerAccess(), set,
                                            AdmitOptions(&hooks));
            REQUIRE(saved);
            Array<uint8> original(allocator);
            REQUIRE(original.TryAppend(saved.Value().Bytes()));

            ObjectGraphSaveResult& result = saved.Value();
            const uint32 shortTable[] = {1, 2};
            const uint32 zero[] = {0, 1, 2};
            const uint32 beyond[] = {1, 4, 2};
            const uint32 twice[] = {1, 1, 2};
            const uint32 valid[] = {1, 2, 3};
            CHECK(result.AssignExternalIndices(shortTable, 3).Error() ==
                  ErrorCode::InvalidArgument);
            CHECK(result.AssignExternalIndices(zero, 3).Error() == ErrorCode::InvalidArgument);
            CHECK(result.AssignExternalIndices(beyond, 3).Error() == ErrorCode::InvalidArgument);
            CHECK(result.AssignExternalIndices(twice, 3).Error() == ErrorCode::InvalidArgument);
            CHECK(result.AssignExternalIndices(valid, 2).Error() == ErrorCode::InvalidArgument);
            CHECK(result.AssignExternalIndices(valid, ObjectGraphLimits{}.maxExternalReferences + 1)
                      .Error() == ErrorCode::CapacityExceeded);
            CHECK(test::graph::SameBytes(result.Bytes(), original.View()));

            Array<RootRef<Object>> made(allocator);
            Array<ObjectHandle> objects(allocator);
            for (usize index = 0; index < 6; ++index) {
                auto created = New(target, made);
                REQUIRE(created && objects.TryPushBack(created.Value()));
            }

            // 역순: 외부 번호 1 = Y, 2 = Z, 3 = X.
            const uint32 reversed[] = {3, 2, 1};
            REQUIRE(result.AssignExternalIndices(reversed, 3));
            CHECK(test::graph::ReadU32(result.Bytes(), test::graph::ExternalCountOffset) == 3);
            {
                const ObjectHandle imports[] = {objects[0], objects[1], objects[2]};
                const Array<TypeId> importTypes = ImportTypes(allocator, 3);
                Array<RootRef<Object>> held(allocator);
                Array<ObjectHandle> nodes(allocator);
                REQUIRE(Restore(allocator, target, types, result.Bytes(), importTypes.View(),
                                imports, held, nodes) == ErrorCode::None);
                const Seen a = Look(target, nodes[0]);
                CHECK(a.link == objects[2] && a.editor == objects[1] && a.links[0] == objects[0] &&
                      a.links[1] == objects[2] && a.hold == objects[0]);
                CHECK(Look(target, nodes[1]).links[0] == objects[2]);
            }

            // 임의 순열과 outer import: X → 5, Z → 1, Y → 3, count 6(2·4·6은 쓰이지 않는다).
            const uint32 scattered[] = {5, 1, 3};
            REQUIRE(result.AssignExternalIndices(scattered, 6));
            CHECK(test::graph::ReadU32(result.Bytes(), test::graph::ExternalCountOffset) == 6);
            {
                const Array<TypeId> importTypes = ImportTypes(allocator, 6);
                Array<RootRef<Object>> held(allocator);
                Array<ObjectHandle> nodes(allocator);
                Array<GraphExternalUse> uses(allocator);
                REQUIRE(Restore(allocator, target, types, result.Bytes(), importTypes.View(),
                                objects.View(), held, nodes, &uses) == ErrorCode::None);
                const Seen a = Look(target, nodes[0]);
                CHECK(a.link == objects[4] && a.editor == objects[0] && a.links[0] == objects[2] &&
                      a.links[1] == objects[4] && a.links[3] == objects[2]);
                REQUIRE(uses.Size() == 6);
                CHECK(uses[1].occurrences == 0 && uses[3].occurrences == 0 &&
                      uses[5].occurrences == 0 &&
                      uses[5].commonFlags == NumericLimits<uint32>::Max() && !uses[5].required);
                CHECK(uses[4].occurrences == 3 && uses[0].occurrences == 2 &&
                      uses[2].occurrences == 4);
            }

            Array<uint8> taken = result.TakeBytes();
            CHECK(taken.Size() == original.Size());
            CHECK(result.AssignExternalIndices(valid, 3).Error() == ErrorCode::InvalidArgument);
            CHECK(result.Externals().Size() == 3);
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 위치 표 메모리(참조 하나에 4 byte)는 그래프 바이트 누계에 든다: 외부 참조 N개짜리 그래프는 출력
// byte S에 대해 maxBytes = S + 4N이면 저장되고 1 작으면 CapacityExceeded다. 작업량에도 든다: 같은
// 집합 {A, x}에서 A의 원소 N개가 집합 밖 y를 가리킬 때와 집합 안 x를 가리킬 때의 최소 작업량 차이가
// 정확히 7N + 2 + G다 — 외부 참조 원소마다 slot 조회 1 + 위치 4 byte + offset 변환 2(필드 →
// Record, Record → 출력), 첫 출현의 외부 대상 표 삽입 2, G = 위치 표가 자랄 때 옮긴
// byte(Array 성장 규칙 ArrayGrowthCapacity로 계산한다). 4 byte 청구를 빼면 차이가 4N 줄어 맞지
// 않는다.
TEST(GraphExternal_PositionTableChargesBytesAndWork) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry source(allocator, types, {0x474578, 9});
        {
            constexpr usize Count = 4000;
            Array<RootRef<Object>> roots(allocator);
            auto outward = New(source, roots);
            auto inward = New(source, roots);
            auto x = New(source, roots);
            auto y = New(source, roots);
            REQUIRE(outward && inward && x && y);
            for (usize index = 0; index < Count; ++index) {
                REQUIRE(AddLink(source, outward.Value(), y.Value()));
                REQUIRE(AddLink(source, inward.Value(), x.Value()));
            }

            Admitter hooks;
            const ObjectHandle external[] = {outward.Value(), x.Value()};
            const ObjectHandle internal[] = {inward.Value(), x.Value()};
            auto saved = SaveObjectGraphSet(allocator, source, source.OwnerAccess(), external,
                                            AdmitOptions(&hooks));
            REQUIRE(saved);
            REQUIRE(saved.Value().Externals().Size() == 1 &&
                    saved.Value().Externals()[0].occurrences == Count);
            const usize bytes = saved.Value().Bytes().Size();
            ObjectGraphSaveOptions options = AdmitOptions(&hooks);
            options.limits.maxBytes = bytes + 4 * Count;
            CHECK(SaveObjectGraphSet(allocator, source, source.OwnerAccess(), external, options));
            options.limits.maxBytes = bytes + 4 * Count - 1;
            CHECK(SaveObjectGraphSet(allocator, source, source.OwnerAccess(), external, options)
                      .Error() == ErrorCode::CapacityExceeded);

            // 위치 표(Array<uint32>)가 꽉 찬 채로 더할 때마다 기존 위치를 옮기며 그 byte를
            // 청구한다.
            uint64 growth = 0;
            usize capacity = 0;
            for (usize size = 0; size < Count; ++size) {
                if (size == capacity) {
                    growth += size * sizeof(uint32);
                    capacity = ArrayGrowthCapacity(capacity, size + 1, sizeof(uint32));
                }
            }

            const uint64 outwardWork =
                MinimumSaveWork(allocator, source, external, AdmitOptions(&hooks));
            const uint64 inwardWork =
                MinimumSaveWork(allocator, source, internal, AdmitOptions(&hooks));
            REQUIRE(outwardWork != 0 && inwardWork != 0);
            CHECK(outwardWork - inwardWork == 7 * Count + 2 + growth);
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}
} // namespace graph_external_tests_file
