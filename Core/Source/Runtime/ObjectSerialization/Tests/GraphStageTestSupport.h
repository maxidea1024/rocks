#pragma once

#include <Core/Memory/SystemAllocator.h>
#include <Object/GeneratedFieldCodec.h>
#include <ObjectSerialization/ObjectGraph.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>
#include <Platform/Thread.h>
#include "GraphTestSupport.h"

// 단계형 복원(ObjectGraphStage) 시험의 공용 타입·도우미다(GraphStageTests.cpp·
// GraphStageStateTests.cpp). 각 시험이 자기 TypeRegistry에 등록한다.
namespace rocks::test::stage {

inline constexpr TypeId NodeType{0x4753544147ULL, 1};
inline constexpr TypeId PackType{0x4753544147ULL, 2};
inline constexpr TypeId WorldType{0x4753544147ULL, 3};
inline constexpr TypeId ReenterType{0x4753544147ULL, 4};
inline constexpr Guid NumberId{0x4753544146ULL, 1};
inline constexpr Guid LinkId{0x4753544146ULL, 2};
inline constexpr Guid CacheId{0x4753544146ULL, 3};
inline constexpr Guid UnknownId{0x4753544146ULL, 9};

// 생성자 실행 수의 단조 누계다(rollback으로 줄지 않는다).
inline uint32 ConstructedNodes = 0;
// 참이면 number 필드의 staged 쓰기가 OutOfMemory로 실패한다(ApplyFields 실패 주입).
inline bool FailNumberWrite = false;

struct StageNode final : Object {
    explicit StageNode(Allocator&) noexcept {
        ++ConstructedNodes;
    }

    void Trace(TraceVisitor& visitor) const noexcept {
        visitor.Visit(link);
    }

    uint32 number = 0;
    // Required 참조다(null이면 Required 검사가 진단한다).
    ObjectRef<StageNode> link;
    // Transient(저장하지 않는다 — 호출자 prepare가 Commit 전에 쓴다).
    uint32 cache = 0;
};

// 패키지 정책 Registry의 패키지 타입이다(필드 없음).
struct StagePack final : Object {
    explicit StagePack(Allocator&) noexcept {
        ++ConstructedNodes;
    }
};

// WorldOnly 타입이다(에셋 Registry 정책이 거부한다).
struct StageWorld final : Object {
    explicit StageWorld(Allocator&) noexcept {
        ++ConstructedNodes;
    }
};

// 생성자가 같은 Stage의 CreateObjects를 다시 부르는 타입이다(재진입 Busy 시험 — 계약 위반 모의).
inline ObjectGraphStage* ReentryStage = nullptr;
inline ConstructionContext* ReentryTransaction = nullptr;
inline ObjectHandle ReentryBase{};
inline ErrorCode ReentryError = ErrorCode::None;

struct StageReenter final : Object {
    explicit StageReenter(Allocator&) noexcept {
        ++ConstructedNodes;
        if (ReentryStage != nullptr) {
            ObjectGraphStage* stage = ReentryStage;
            ReentryStage = nullptr;
            auto again = stage->CreateObjects(*ReentryTransaction, ReentryBase);
            ReentryError = again ? ErrorCode::None : again.Error();
        }
    }
};

template <auto Member>
RuntimeFieldOps NodeField(Guid id) noexcept {
    return {
        id,
        [](ObjectRegistry& registry, const ObjectAccessContext& access, ObjectHandle object,
           FieldValue& value) noexcept {
            return detail::ReadGeneratedField<StageNode>(registry, access, object, Member, value);
        },
        [](ObjectRegistry& registry, const ObjectAccessContext& access, ObjectHandle object,
           const FieldValue& value) noexcept {
            return detail::WriteGeneratedField<StageNode>(registry, access, object, Member, value);
        },
        [](ConstructionContext& transaction, ObjectHandle object,
           const FieldValue& value) noexcept {
            return detail::WriteGeneratedStagedField<StageNode>(transaction, object, Member, value);
        },
        &detail::EncodeThroughFieldValue,
        &detail::DecodeThroughFieldValue};
}

inline bool RegisterTypes(TypeRegistry& types) noexcept {
    FieldSchema fields[3];
    fields[0].id = NumberId;
    fields[0].name = {"number", 6};
    fields[0].value.kind = ValueKind::UInt32;
    fields[1].id = LinkId;
    fields[1].name = {"link", 4};
    fields[1].kind = FieldKind::ObjectReference;
    fields[1].value = {ValueKind::ObjectReference, NodeType};
    fields[1].flags = static_cast<uint32>(FieldFlags::Required);
    fields[2].id = CacheId;
    fields[2].name = {"cache", 5};
    fields[2].value.kind = ValueKind::UInt32;
    fields[2].flags = static_cast<uint32>(FieldFlags::Transient);
    RuntimeFieldOps number = NodeField<&StageNode::number>(NumberId);
    number.writeStaged = [](ConstructionContext& transaction, ObjectHandle object,
                            const FieldValue& value) noexcept -> Result<void> {
        if (FailNumberWrite) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }

        return detail::WriteGeneratedStagedField<StageNode>(transaction, object, &StageNode::number,
                                                            value);
    };
    RuntimeFieldOps cache = NodeField<&StageNode::cache>(CacheId);
    cache.encode = nullptr;
    cache.decode = nullptr;
    // 수동 함수표의 Required 필드는 Required 검사(2c)가 staged 객체를 readStaged로 읽는다.
    RuntimeFieldOps link = NodeField<&StageNode::link>(LinkId);
    link.readStaged = [](ConstructionContext& transaction, ObjectHandle object,
                         FieldValue& out) noexcept -> Result<void> {
        auto owner = transaction.Get<StageNode>(object);
        if (!owner) {
            return Result<void>::Failure(owner.Error());
        }

        out.kind = ValueKind::ObjectReference;
        out.reference = owner.Value()->link.Handle();
        return Result<void>::Success();
    };
    const RuntimeFieldOps ops[] = {number, link, cache};
    auto node = MakeRuntimeTypeOps<StageNode>();
    node.fields = ops;
    node.completeFieldLayout = true;
    auto pack = MakeRuntimeTypeOps<StagePack>();
    pack.completeFieldLayout = true;
    auto world = MakeRuntimeTypeOps<StageWorld>();
    world.completeFieldLayout = true;
    auto reenter = MakeRuntimeTypeOps<StageReenter>();
    reenter.completeFieldLayout = true;
    TypeSchema worldSchema{WorldType, 1, {}, {"StageWorld", 10}};
    worldSchema.flags = static_cast<uint32>(TypeFlags::WorldOnly);
    return types.Register({NodeType, 1, fields, {"StageNode", 9}}, Move(node)).IsOk() &&
           types.Register({PackType, 1, {}, {"StagePack", 9}}, Move(pack)).IsOk() &&
           types.Register(worldSchema, Move(world)).IsOk() &&
           types.Register({ReenterType, 1, {}, {"StageReenter", 12}}, Move(reenter)).IsOk();
}

inline RegistryOptions PackagePolicy() noexcept {
    RegistryOptions options;
    options.policy.packageNativeType = NativeTypeKey<StagePack>();
    options.policy.rejectWorldOnly = true;
    return options;
}

// type의 Record(version)다. NodeType이면 number·link(local ID)가 0이 아닐 때 쓰고, unknown이면
// 모르는 필드를 하나 더한다.
inline Array<uint8> MakeRecord(Allocator& allocator, TypeId type = NodeType, uint32 number = 0,
                               uint32 link = 0, bool unknown = false, uint32 version = 1) noexcept {
    RecordBuilder record(allocator, type, version);
    auto add = [&](Guid id, uint32 value) noexcept {
        ArchiveWriter payload(allocator);
        if (!payload.WriteU32(value) || !record.AddField(id, WireType::U32, payload.View())) {
            platform::Fatal("Stage test record field failed");
        }
    };
    if (number != 0) {
        add(NumberId, number);
    }

    if (link != 0) {
        add(LinkId, link);
    }

    if (unknown) {
        add(UnknownId, 1);
    }

    auto built = record.Build();
    if (!built) {
        platform::Fatal("Stage test record build failed");
    }

    return Move(built.Value());
}

// 노드 설명들로 그래프 byte를 만든다(nodeTable이면 NodeTable 비트, base면 BaseOuter 비트도).
inline Array<uint8> MakeGraph(Allocator& allocator, Span<const test::graph::Node> nodes,
                              bool nodeTable, bool base = false) noexcept {
    test::graph::Header header;
    header.flags =
        (nodeTable ? test::graph::NodeTableFlag : 0u) | (base ? test::graph::BaseOuterFlag : 0u);
    auto built = test::graph::Build(allocator, nodes, header);
    if (!built) {
        platform::Fatal("Stage test graph build failed");
    }

    return Move(built.Value());
}

// 그래프의 노드 표(test::graph::Node)를 단계형 입력의 노드 설명으로 옮긴다.
template <usize N>
void Describe(const test::graph::Node (&nodes)[N], GraphNodeDesc (&descs)[N]) noexcept {
    for (usize index = 0; index < N; ++index) {
        descs[index] = {nodes[index].type, nodes[index].outer, nodes[index].name};
    }
}

inline Result<ObjectHandle> New(ObjectRegistry& registry, Array<RootRef<Object>>& roots,
                                ObjectHandle outer = {}, StringView name = {},
                                TypeId type = NodeType) noexcept {
    auto made = registry.NewObject<Object>(registry.OwnerAccess(), type, outer, name);
    if (!made) {
        return Result<ObjectHandle>::Failure(made.Error());
    }

    const ObjectHandle handle = made.Value().Handle();
    if (!roots.TryPushBack(Move(made.Value()))) {
        return Result<ObjectHandle>::Failure(ErrorCode::OutOfMemory);
    }

    return Result<ObjectHandle>::Success(handle);
}

struct Seen {
    uint32 number = 0;
    ObjectHandle link{};
    uint32 cache = 0;
};

inline Seen Look(ObjectRegistry& registry, ObjectHandle handle) noexcept {
    Seen seen;
    auto borrowed = registry.Resolve<StageNode>(registry.OwnerAccess(), handle);
    if (!borrowed) {
        return seen;
    }

    auto value = borrowed.Value().Get();
    if (!value) {
        return seen;
    }

    seen.number = value.Value()->number;
    seen.link = value.Value()->link.Handle();
    seen.cache = value.Value()->cache;
    return seen;
}

inline bool NameIs(ObjectRegistry& registry, ObjectHandle handle, StringView expected,
                   Allocator& allocator) noexcept {
    String name(allocator);
    return registry.GetName(registry.OwnerAccess(), handle, name) && name.View() == expected;
}

inline bool Finds(const Result<ObjectHandle>& found, ObjectHandle expected) noexcept {
    return found && found.Value() == expected;
}

inline ErrorCode ErrorOf(const Result<void>& result) noexcept {
    return result ? ErrorCode::None : result.Error();
}

// 노드 설명 그래프 하나를 단계형으로 끝까지 진행해 Commit한다(roots = 노드 1). 실패하면 그 오류다.
inline ErrorCode RunStage(Allocator& allocator, ObjectRegistry& registry, TypeRegistry& types,
                          const ObjectGraphInput& input, ObjectHandle baseOuter = {},
                          ObjectGraphLoadOptions options = {},
                          RootRef<Object>* held = nullptr) noexcept {
    ObjectGraphStage stage(allocator, registry, types, registry.OwnerAccess(), options);
    if (auto valid = stage.Validate(input); !valid) {
        return valid.Error();
    }

    ConstructionContext transaction(registry, registry.OwnerAccess());
    if (auto created = stage.CreateObjects(transaction, baseOuter); !created) {
        return created.Error();
    }

    if (auto applied = stage.ApplyFields(transaction); !applied) {
        return applied.Error();
    }

    if (auto checked = stage.CheckRequired(transaction); !checked) {
        return checked.Error();
    }

    RootRef<Object> local[1];
    const ObjectHandle roots[] = {stage.Nodes()[0]};
    if (auto committed = transaction.Commit(roots, local); !committed) {
        return committed.Error();
    }

    if (held != nullptr) {
        *held = Move(local[0]);
    }

    return ErrorCode::None;
}

// Validate만 해 보고 그 결과다.
inline ErrorCode ValidateOnly(Allocator& allocator, ObjectRegistry& registry, TypeRegistry& types,
                              const ObjectGraphInput& input,
                              ObjectGraphLoadOptions options = {}) noexcept {
    ObjectGraphStage stage(allocator, registry, types, registry.OwnerAccess(), options);
    return ErrorOf(stage.Validate(input));
}

// 진단마다 종류와 부른 스레드를 기록한다. failRequired면 RequiredMissing 보고를 그 오류로
// 실패시키고, probe가 있으면 RequiredMissing 보고 중(Required 검사 중 표시 동안) probe의
// FindObject·Create 결과를 기록한다.
struct ThreadDiagnostics final : FieldCodecHooks {
    Result<void> Report(const FieldDiagnostic& diagnostic) noexcept override {
        if (count < 4) {
            kinds[count] = diagnostic.kind;
            threads[count] = platform::CurrentThreadId();
        }

        ++count;
        if (diagnostic.kind != FieldDiagnosticKind::RequiredMissing) {
            return Result<void>::Success();
        }

        if (probe != nullptr) {
            auto found = probe->FindObject(probeOutermost, {});
            findError = found ? ErrorCode::None : found.Error();
            auto created = probe->Create<Object>(NodeType);
            createError = created ? ErrorCode::None : created.Error();
        }

        return failRequired == ErrorCode::None ? Result<void>::Success()
                                               : Result<void>::Failure(failRequired);
    }

    FieldDiagnosticKind kinds[4] = {};
    uint64 threads[4] = {};
    usize count = 0;
    ErrorCode failRequired = ErrorCode::None;
    ConstructionContext* probe = nullptr;
    ObjectHandle probeOutermost{};
    ErrorCode findError = ErrorCode::DependencyFailed;
    ErrorCode createError = ErrorCode::DependencyFailed;
};

// 다른 스레드에서 함수 하나를 실행하고 끝날 때까지 기다린다.
template <class Function>
bool RunOnWorker(Function& function) noexcept {
    platform::Thread thread;
    auto entry = [](void* context) noexcept { (*static_cast<Function*>(context))(); };
    if (thread.Start(entry, &function) != platform::ThreadStatus::Success) {
        return false;
    }

    if (thread.WaitForCompletion(10000) != platform::WaitResult::Signaled ||
        thread.Join() != platform::ThreadStatus::Success) {
        platform::Fatal("Graph stage test worker failed to join");
    }

    return true;
}

} // namespace rocks::test::stage
