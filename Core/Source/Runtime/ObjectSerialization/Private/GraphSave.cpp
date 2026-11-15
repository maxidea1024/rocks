#include "GraphInternal.h"

namespace rocks::graph_detail {

// 저장 결과를 만드는 내부 통로다(ObjectGraphSaveResult의 생성자는 비공개다).
struct SaveResultAccess {
    static ObjectGraphSaveResult Make(Array<uint8>&& bytes, Array<ObjectHandle>&& nodes) noexcept {
        return ObjectGraphSaveResult(Move(bytes), Move(nodes));
    }
};

} // namespace rocks::graph_detail

namespace rocks {
namespace {

using namespace graph_detail;

// 저장할 노드 하나다. lease는 집합 노드면 OpenSetNodes가, Include가 더한 노드면 발견할 때 연 대상
// 타입 lease이며 참조 검사와 SaveNodeFields가 재사용한다. outer는 Registry의 Outer(없으면
// ObjectHandle{}), 이름은 NodeTable을 쓸 때만 문맥의 이름 버퍼 [nameOffset, nameOffset +
// nameBytes)에 모은다. nodes가 다시 할당될 수 있으므로 lease·노드의 주소를 보관하지 않고 index로
// 다시 접근한다(lease가 돌려준 스키마·필드 정보는 레지스트리 레코드 안이라 안정적이다).
struct SaveNode {
    SaveNode(Allocator& allocator, ObjectHandle value) noexcept : handle(value), record(allocator) {
    }

    ObjectHandle handle;
    ObjectHandle outer{};
    TypeLease lease;
    TypeId type{};
    usize nameOffset = 0;
    usize nameBytes = 0;
    Array<uint8> record;
};

// graph v2 저장 문맥이다: 강한 참조는 대상 타입을 검사한 뒤 U32 local ID로 쓴다. 집합 밖 대상은
// 정책대로 거부(Reject)하거나 노드로 더한다(Include — Outer 사슬 먼저). nodes가 다시 할당될 수
// 있으므로 원소 참조를 보관하지 않는다. 참조 비용은 참조 수에 선형이다(5.6절): handle → local ID
// 해시 표(키는 domain을 포함한 handle 전체 — 캐시 적중이 domain 검사를 건너뛰지 않게), 노드별
// TypeLease, 기대 TypeId → lease 캐시를 쓰고 조회·삽입마다 작업량 1을 청구한다.
class GraphEncodeContext final : public FieldEncodeContext {
public:
    GraphEncodeContext(Allocator& allocator, ObjectRegistry& registry,
                       const ObjectAccessContext& access, Array<SaveNode>& nodes, Budget& budget,
                       const ObjectGraphSaveOptions& options) noexcept
        : FieldEncodeContext(options.policy, options.hooks, ReferenceWire), allocator_(allocator),
          registry_(registry), access_(access), nodes_(nodes), budget_(budget),
          externals_(options.externals), writeNodeTable_(options.writeNodeTable),
          packagePolicy_(registry.Policy().packageNativeType != nullptr),
          scratch_(allocator, budget), observation_(allocator, budget, false), locals_(allocator),
          expected_(allocator), firstAncestors_(allocator), chain_(allocator), names_(allocator) {
    }

    void BeginObject(TypeId type, uint32 index) noexcept {
        SetObject(type, index);
    }

    // 호출자 집합을 local ID 순으로 더한다(같은 handle이 두 번이면 InvalidArgument). lease·Outer·
    // 이름은 OpenSetNodes가 채운다.
    Result<void> AddSetNodes(Span<const ObjectHandle> set) noexcept {
        if (auto reserved = nodes_.TryReserve(set.Size()); !reserved) {
            return reserved;
        }

        for (ObjectHandle handle : set) {
            // 노드 추가와 해시 표 삽입을 청구한다.
            if (auto spent = budget_.Spend(2); !spent) {
                return spent;
            }

            auto added = nodes_.TryEmplaceBack(allocator_, handle);
            if (!added) {
                return Result<void>::Failure(added.Error());
            }

            auto inserted = locals_.Insert(handle, static_cast<uint32>(nodes_.Size()));
            if (!inserted || inserted.Value() == InsertOutcome::AlreadyPresent) {
                (void)nodes_.TryPopBack();
                return Result<void>::Failure(inserted ? ErrorCode::InvalidArgument
                                                      : inserted.Error());
            }
        }

        return Result<void>::Success();
    }

    // 집합 노드마다 실제 타입 lease(생존·문맥 검사 — AcquireType)와 Outer, NodeTable이면 이름을
    // 연다.
    Result<void> OpenSetNodes() noexcept {
        for (usize index = 0; index < nodes_.Size(); ++index) {
            if (auto spent = budget_.Spend(1); !spent) {
                return spent;
            }

            auto type = registry_.AcquireType(access_, nodes_[index].handle);
            if (!type) {
                return Result<void>::Failure(type.Error());
            }

            auto outer = registry_.GetOuter(access_, nodes_[index].handle);
            if (!outer) {
                return Result<void>::Failure(outer.Error());
            }

            nodes_[index].lease = Move(type.Value());
            nodes_[index].outer = outer.Value();
            if (auto named = AppendName(nodes_[index]); !named) {
                return named;
            }
        }

        return Result<void>::Success();
    }

    // Include가 쓰는 거부 조건을 준비한다: 노드 1의 조상(기준 Outer부터 outermost까지)과, 패키지
    // 정책 Registry면 노드 1의 outermost.
    Result<void> PrepareInclude() noexcept {
        if (externals_ != GraphExternalPolicy::Include) {
            return Result<void>::Success();
        }

        for (ObjectHandle at = nodes_[0].outer; at != ObjectHandle{};) {
            if (auto spent = budget_.Spend(2); !spent) {
                return spent;
            }

            if (auto inserted = firstAncestors_.Insert(at, true); !inserted) {
                return Result<void>::Failure(inserted.Error());
            }

            auto next = registry_.GetOuter(access_, at);
            if (!next) {
                return Result<void>::Failure(next.Error());
            }

            at = next.Value();
        }

        if (packagePolicy_) {
            auto outermost = registry_.GetOutermost(access_, nodes_[0].handle);
            if (!outermost) {
                return Result<void>::Failure(outermost.Error());
            }

            firstOutermost_ = outermost.Value();
        }

        return Result<void>::Success();
    }

    // NodeTable에 쓸 이름들이다(노드의 [nameOffset, nameOffset + nameBytes)).
    StringView Names() const noexcept {
        return names_.View();
    }

    // local ID다(집합에 없으면 0).
    uint32 LocalOf(ObjectHandle handle) const noexcept {
        const uint32* found = locals_.Find(handle);
        return found ? *found : 0;
    }

    Result<void> Spend(uint64 work) noexcept override {
        return budget_.Spend(work);
    }

    Result<void> ReserveApply(uint64) noexcept override {
        return Result<void>::Success();
    }

    Allocator& Scratch() noexcept override {
        return scratch_;
    }

    // 저장은 관찰하지 않는다(계약상 부르지 않는다 — 공통 가상 구현).
    Allocator& ObservationScratch() noexcept override {
        return observation_;
    }

    Result<void> AddFields(usize count) noexcept override {
        return budget_.AddFields(count);
    }

    Result<void> AddElements(usize count) noexcept override {
        return budget_.AddElements(count);
    }

    // 중첩 Record는 필드 값 하나라 전체 크기도 값 한도 안이어야 한다(배열 payload의 상한도 이
    // 값이다).
    RecordLimits NestedRecordLimits() const noexcept override {
        RecordLimits limits = budget_.Record();
        limits.archive.maxTotalBytes = budget_.limits.maxValueBytes;
        return limits;
    }

protected:
    Result<void> WriteReferenceWire(ArchiveWriter& out, ObjectHandle target,
                                    const ReferenceExpectation& expectation) noexcept override {
        if (target == ObjectHandle{}) {
            return out.WriteU32(0);
        }

        if (target.IsNull()) {
            return Result<void>::Failure(ErrorCode::StaleHandle);
        }

        if (auto spent = budget_.Spend(1); !spent) {
            return spent;
        }

        // 집합 노드면 그 lease로 검사하고, 아니면 대상 타입을 열어(생존·문맥 검사 포함) 검사가 끝난
        // 뒤 정책대로 처리한다.
        uint32 local = 0;
        TypeLease discovered;
        const TypeLease* actual = &discovered;
        if (const uint32* found = locals_.Find(target)) {
            // 캐시에 맞아도 참조마다 lease 없는 O(1) 생존 조회를 한다 — 참조마다 AcquireType을
            // 부르는 것과 같은 판정이다(무효·재사용 handle은 StaleHandle, 논리 파괴·cutoff는
            // Destroyed, 문맥·종료·재진입 오류는 그대로).
            auto lifecycle = registry_.InspectLifecycle(access_, target);
            if (!lifecycle) {
                return Result<void>::Failure(lifecycle.Error());
            }

            if (lifecycle.Value() != ObjectLifecycle::Alive) {
                return Result<void>::Failure(ErrorCode::Destroyed);
            }

            local = *found;
            actual = &nodes_[local - 1].lease;
        } else {
            auto acquired = registry_.AcquireType(access_, target);
            if (!acquired) {
                return Result<void>::Failure(acquired.Error());
            }

            discovered = Move(acquired.Value());
        }

        // Registry setter와 같은 native 상속 계약이다. 선언 타입의 파생 객체와 schema ID가 다른
        // 별명 등록도 허용한다. 정적 thunk의 대상 타입도 함께 만족해야 한다.
        if (expectation.staticNative && !actual->IsNativeA(expectation.staticNative)) {
            return Result<void>::Failure(ErrorCode::TypeMismatch);
        }

        if (expectation.schemaType != Guid{}) {
            auto expected = ExpectedLease(expectation.schemaType);
            if (!expected) {
                return Result<void>::Failure(expected.Error());
            }

            if (!actual->IsNativeA(expected.Value()->Ops().nativeType)) {
                return Result<void>::Failure(ErrorCode::TypeMismatch);
            }
        }

        if (local == 0) {
            if (externals_ != GraphExternalPolicy::Include) {
                return RejectExternal();
            }

            auto added = IncludeNode(target, Move(discovered));
            if (!added) {
                return Result<void>::Failure(added.Error());
            }

            local = added.Value();
        }

        return out.WriteU32(local);
    }

private:
    // 집합 밖 참조를 지금 위치의 ExternalReference 진단(value 0 = 거부) 뒤 InvalidArgument로
    // 거부한다 (hook 실패는 그 오류).
    Result<void> RejectExternal() noexcept {
        if (auto reported = Report(FieldDiagnosticKind::ExternalReference, 0); !reported) {
            return reported;
        }

        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // Include: target을 노드로 더하고 local ID를 돌려준다. target의 Outer 사슬 중 집합에 없는
    // 조상을 바깥쪽부터 먼저 더한다(집합 안 조상·기준 Outer·Outer 없음에서 멈춘다). target이나 그
    // 조상이 기준 Outer·노드 1의 조상이거나, 패키지 정책에서 target의 outermost가 노드 1의 것과
    // 다르거나, 노드 1에 기준 Outer가 있는데 사슬이 Outer 없음에 닿으면 거부한다(11절 결정 17 —
    // 그대로 더하면 NodeTable의 Outer 순서·혼합 규칙에 걸린다).
    Result<uint32> IncludeNode(ObjectHandle target, TypeLease lease) noexcept {
        if (firstAncestors_.Contains(target)) {
            auto rejected = RejectExternal();
            return Result<uint32>::Failure(rejected.Error());
        }

        if (packagePolicy_) {
            auto outermost = registry_.GetOutermost(access_, target);
            if (!outermost) {
                return Result<uint32>::Failure(outermost.Error());
            }

            if (outermost.Value() != firstOutermost_) {
                auto rejected = RejectExternal();
                return Result<uint32>::Failure(rejected.Error());
            }
        }

        auto targetOuter = registry_.GetOuter(access_, target);
        if (!targetOuter) {
            return Result<uint32>::Failure(targetOuter.Error());
        }

        // 안쪽 조상부터 모은다. 멈춘 자리(stop)가 가장 바깥 조상의 Outer다.
        chain_.Clear();
        ObjectHandle stop = targetOuter.Value();
        while (stop != ObjectHandle{}) {
            if (auto spent = budget_.Spend(2); !spent) {
                return Result<uint32>::Failure(spent.Error());
            }

            if (locals_.Contains(stop) || stop == nodes_[0].outer) {
                break;
            }

            if (firstAncestors_.Contains(stop)) {
                auto rejected = RejectExternal();
                return Result<uint32>::Failure(rejected.Error());
            }

            if (!chain_.TryPushBack(stop)) {
                return Result<uint32>::Failure(ErrorCode::OutOfMemory);
            }

            auto next = registry_.GetOuter(access_, stop);
            if (!next) {
                return Result<uint32>::Failure(next.Error());
            }

            stop = next.Value();
        }

        // 사슬이 Outer 없음에 닿았는데(다른 트리 — 정책 없는 Registry) 노드 1에 기준 Outer가 있으면
        // 더할 가장 바깥 조상이 Outer 없음 노드가 되어 기준 Outer와 섞이므로 거부한다.
        if (stop == ObjectHandle{} && nodes_[0].outer != ObjectHandle{}) {
            auto rejected = RejectExternal();
            return Result<uint32>::Failure(rejected.Error());
        }

        for (usize index = chain_.Size(); index > 0; --index) {
            const ObjectHandle ancestor = chain_[index - 1];
            auto type = registry_.AcquireType(access_, ancestor);
            if (!type) {
                return Result<uint32>::Failure(type.Error());
            }

            const ObjectHandle outer = index < chain_.Size() ? chain_[index] : stop;
            if (auto added = AddNode(ancestor, outer, Move(type.Value())); !added) {
                return added;
            }
        }

        return AddNode(target, targetOuter.Value(), Move(lease));
    }

    // 새 노드를 끝에 더하고 local ID를 돌려준다(maxObjects를 넘으면 CapacityExceeded).
    Result<uint32> AddNode(ObjectHandle handle, ObjectHandle outer, TypeLease lease) noexcept {
        if (nodes_.Size() >= budget_.limits.maxObjects) {
            return Result<uint32>::Failure(ErrorCode::CapacityExceeded);
        }

        // 노드 배열 성장(기존 원소 이동)과 해시 표 삽입을 청구한다.
        auto spent = budget_.Spend(2 + (nodes_.Size() == nodes_.Capacity() ? nodes_.Size() : 0));
        if (!spent) {
            return Result<uint32>::Failure(spent.Error());
        }

        // 노드를 먼저 더하고 이름·맵 삽입이 실패하면 되돌린다(맵의 local ID는 언제나 nodes 안이다).
        auto added = nodes_.TryEmplaceBack(allocator_, handle);
        if (!added) {
            return Result<uint32>::Failure(added.Error());
        }

        added.Value()->outer = outer;
        added.Value()->lease = Move(lease);
        if (auto named = AppendName(*added.Value()); !named) {
            (void)nodes_.TryPopBack();
            return Result<uint32>::Failure(named.Error());
        }

        const uint32 local = static_cast<uint32>(nodes_.Size());
        if (auto inserted = locals_.Insert(handle, local); !inserted) {
            (void)nodes_.TryPopBack();
            return Result<uint32>::Failure(inserted.Error());
        }

        return Result<uint32>::Success(local);
    }

    // NodeTable을 쓸 때 노드 이름을 이름 버퍼 끝에 붙이고 위치를 기록한다(이름 byte를 청구한다).
    Result<void> AppendName(SaveNode& node) noexcept {
        if (!writeNodeTable_) {
            return Result<void>::Success();
        }

        const usize offset = names_.ByteCount();
        if (auto named = registry_.GetName(access_, node.handle, names_); !named) {
            return named;
        }

        node.nameOffset = offset;
        node.nameBytes = names_.ByteCount() - offset;
        return budget_.Spend(node.nameBytes);
    }

    // 기대 TypeId의 등록 타입 lease다(저장 끝까지 캐시에 쥔다). 조회·삽입마다 작업량 1이다.
    Result<const TypeLease*> ExpectedLease(TypeId type) noexcept {
        if (auto spent = budget_.Spend(1); !spent) {
            return Result<const TypeLease*>::Failure(spent.Error());
        }

        if (const TypeLease* cached = expected_.Find(type)) {
            return Result<const TypeLease*>::Success(cached);
        }

        auto acquired = registry_.AcquireRegisteredType(access_, type);
        if (!acquired) {
            return Result<const TypeLease*>::Failure(acquired.Error());
        }

        if (auto spent = budget_.Spend(1); !spent) {
            return Result<const TypeLease*>::Failure(spent.Error());
        }

        if (auto inserted = expected_.Insert(type, Move(acquired.Value())); !inserted) {
            return Result<const TypeLease*>::Failure(inserted.Error());
        }

        return Result<const TypeLease*>::Success(expected_.Find(type));
    }

    Allocator& allocator_;
    ObjectRegistry& registry_;
    const ObjectAccessContext& access_;
    Array<SaveNode>& nodes_;
    Budget& budget_;
    GraphExternalPolicy externals_;
    bool writeNodeTable_;
    bool packagePolicy_;
    BudgetAllocator scratch_;
    BudgetAllocator observation_;
    HashMap<ObjectHandle, uint32, HandleHash> locals_;
    HashMap<TypeId, TypeLease> expected_;
    // Include: 노드 1의 조상(기준 Outer부터)과 조상 순회의 임시 사슬, 패키지 정책의 노드 1
    // outermost.
    HashMap<ObjectHandle, bool, HandleHash> firstAncestors_;
    Array<ObjectHandle> chain_;
    ObjectHandle firstOutermost_{};
    String names_;
};

// 노드 하나의 필드를 encode해 Record를 만든다. fileBytes는 지금까지의 출력 byte 누계이고 이 노드의
// Bytes(Record)와(NodeTable이면) 노드 항목을 더해 maxBytes와 비교한다.
Result<void> SaveNodeFields(Allocator& allocator, ObjectRegistry& registry,
                            const ObjectAccessContext& access, Array<SaveNode>& nodes,
                            usize nodeIndex, Budget& budget, usize& fileBytes, bool nodeTable,
                            GraphEncodeContext& context) noexcept {
    const ObjectHandle handle = nodes[nodeIndex].handle;
    // 이 노드의 필드를 encode하는 동안 owner를 고정한다(필드마다 잠그지 않는다). 저장 중 콜백이
    // Registry를 바꾸지 않는다는 계약을 어겨도, encode가 순회하는 owner 밖 버퍼(String·Soft 경로·
    // 컨테이너 원소)를 컨테이너·참조 변경 API와 등록된 필드 쓰기가 해제하지 못하고 Busy가 된다.
    // 실패·성공 모든 경로에서 범위를 벗어날 때 풀린다.
    auto pin = registry.PinForEncode(access, handle);
    if (!pin) {
        return Result<void>::Failure(pin.Error());
    }

    // 집합 노드는 encode 전에 배치를 검사했다. Include가 더한 노드는 여기서 검사한다(같은 판정을 두
    // 번 해도 결과는 같다).
    auto layout = ValidateLayout(nodes[nodeIndex].lease, budget);
    if (!layout) {
        return layout;
    }

    const auto& schema = nodes[nodeIndex].lease.Schema();
    // builder의 필드 수 한도는 이 객체 필드를 세기 전의 남은 누계다(로드와 같은 기준). 크기 한도는
    // 남은 파일 byte(이 노드의 Record 길이 prefix와 NodeTable 항목을 뺀 것)라 넘칠 Record를 끝까지
    // 만들지 않고, 노드 Record들의 합이 maxBytes로 묶인다(필드 writer도 같은 한도로 연다).
    const usize entryBytes = nodeTable ? NodeEntryBytes + nodes[nodeIndex].nameBytes : 0;
    usize reserved = 0;
    if (!TryAdd(fileBytes, RecordPrefixBytes, reserved) ||
        !TryAdd(reserved, entryBytes, reserved)) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    RecordLimits recordLimits = budget.Record();
    recordLimits.archive.maxTotalBytes =
        reserved < budget.limits.maxBytes ? budget.limits.maxBytes - reserved : 0;
    auto counted = budget.AddFields(schema.fields.Size());
    if (!counted) {
        return counted;
    }

    context.BeginObject(schema.id, static_cast<uint32>(nodeIndex));
    RecordBuilder record(allocator, schema.id, schema.version, recordLimits);
    usize payloadBytes = 0;
    for (usize fieldIndex = 0; fieldIndex < schema.fields.Size(); ++fieldIndex) {
        const auto& field = schema.fields[fieldIndex];
        // 최상위 필드의 declaringType은 객체 타입이다(상속 필드도 펼친 스키마로 다룬다). encode가
        // 노드를 더하면 nodes가 다시 할당될 수 있으므로 lease는 매번 nodes[nodeIndex]로 접근한다.
        const TypeLease& type = nodes[nodeIndex].lease;
        FieldScope scope(context, field, schema.id, type.FieldEnum(fieldIndex),
                         type.FieldStruct(fieldIndex));
        // Transient 참조는 GC 수명에는 참여하지만 저장 그래프에는 참여하지 않는다.
        if (context.IsExcluded(context.EffectiveFlags())) {
            continue;
        }

        auto accessor = FindOps(type, field.id, budget);
        if (!accessor) {
            return Result<void>::Failure(accessor.Error());
        }

        auto spent = budget.Spend(1);
        if (!spent) {
            return spent;
        }

        ArchiveLimits fieldLimits = budget.Archive();
        fieldLimits.maxTotalBytes = recordLimits.archive.maxTotalBytes;
        ArchiveWriter encoded(allocator, fieldLimits);
        auto wire =
            accessor.Value()->encode(*accessor.Value(), registry, access, handle, encoded, context);
        if (!wire) {
            return Result<void>::Failure(wire.Error());
        }

        // codec이 스키마 필드의 wire(배열은 원소에 따라 ValueArray·RecordList)를 썼는지 다시
        // 확인한다.
        auto expected = detail::FieldWireType(field, context.ReferenceWireType());
        if (!expected || expected.Value() != wire.Value()) {
            return Result<void>::Failure(ErrorCode::TypeMismatch);
        }

        spent = budget.Spend(encoded.WorkUsed());
        if (!spent) {
            return spent;
        }

        usize appended = 0;
        if (!TryAdd(28, encoded.ByteCount(), appended)) {
            return Result<void>::Failure(ErrorCode::Overflow);
        }

        // builder의 실제 작업(복사, 성장할 때만 기존 byte 이동, id 중복 검사)을 청구한다.
        const uint64 builderBefore = record.WorkUsed();
        auto added = record.AddField(field.id, wire.Value(), encoded.View());
        if (!added) {
            return added;
        }

        spent = budget.Spend(record.WorkUsed() - builderBefore);
        if (!spent) {
            return spent;
        }

        if (!TryAdd(payloadBytes, appended, payloadBytes)) {
            return Result<void>::Failure(ErrorCode::Overflow);
        }
    }

    // 이 노드가 출력에 더하는 byte: Bytes(Record) = 8 + 44 + payload, NodeTable이면 노드 항목.
    usize recordBytes = 0;
    usize total = 0;
    if (!TryAdd(payloadBytes, RecordHeaderBytes, recordBytes) ||
        !TryAdd(fileBytes, RecordPrefixBytes, total) || !TryAdd(total, recordBytes, total) ||
        !TryAdd(total, entryBytes, total)) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    if (total > budget.limits.maxBytes) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    auto spent = budget.Spend(static_cast<uint64>(recordBytes) + 1);
    if (!spent) {
        return spent;
    }

    auto output = record.Build();
    if (!output) {
        return Result<void>::Failure(output.Error());
    }

    // encode가 nodes를 재할당했을 수 있으므로 이전 원소 참조를 저장하지 않고 index로 다시 접근한다.
    nodes[nodeIndex].type = schema.id;
    nodes[nodeIndex].record = Move(output.Value());
    fileBytes = total;
    return Result<void>::Success();
}

// NodeTable의 outer 값과 BaseOuter 비트를 정한다(5.2절): 집합 안 Outer는 그 local ID(자기보다
// 앞이어야 한다), 집합 밖 Outer는 0이고 모두 기준 Outer(노드 1의 Outer)와 같아야 한다 — Outer
// 없음과 기준 Outer를 섞을 수 없다. 어기면 InvalidArgument다. outers는 노드 순서의 outer 값을
// 받는다.
Result<bool> ResolveNodeTable(const Array<SaveNode>& nodes, const GraphEncodeContext& context,
                              Array<uint32>& outers, Budget& budget) noexcept {
    if (auto spent = budget.Spend(nodes.Size()); !spent) {
        return Result<bool>::Failure(spent.Error());
    }

    if (auto reserved = outers.TryReserve(nodes.Size()); !reserved) {
        return Result<bool>::Failure(reserved.Error());
    }

    const ObjectHandle base = nodes[0].outer;
    for (usize index = 0; index < nodes.Size(); ++index) {
        const ObjectHandle outer = nodes[index].outer;
        uint32 value = 0;
        if (outer != ObjectHandle{}) {
            value = context.LocalOf(outer);
            if (value > index) {
                return Result<bool>::Failure(ErrorCode::InvalidArgument);
            }
        }

        if (value == 0 && outer != base) {
            return Result<bool>::Failure(ErrorCode::InvalidArgument);
        }

        if (!outers.TryPushBack(value)) {
            return Result<bool>::Failure(ErrorCode::OutOfMemory);
        }
    }

    return Result<bool>::Success(base != ObjectHandle{});
}

// header·NodeTable·Record를 출력 하나로 쓴다. outers가 비어 있으면 NodeTable 없이 쓴다.
Result<Array<uint8>> WriteGraph(Allocator& allocator, const Array<SaveNode>& nodes,
                                StringView names, const Array<uint32>& outers, bool baseOuter,
                                Budget& budget) noexcept {
    const bool nodeTable = outers.Size() == nodes.Size();
    const uint32 flags = (nodeTable ? NodeTableFlag : 0) | (baseOuter ? BaseOuterFlag : 0);
    ArchiveWriter output(allocator, budget.Archive(true));
    auto written = output.WriteRaw(Magic);
    const uint32 header[] = {FormatVersion, flags, static_cast<uint32>(nodes.Size()), 0};
    for (uint32 value : header) {
        if (written) {
            written = output.WriteU32(value);
        }
    }

    for (usize index = 0; written && nodeTable && index < nodes.Size(); ++index) {
        written = output.WriteGuid(nodes[index].type);
        if (written) {
            written = output.WriteU32(outers[index]);
        }

        if (written) {
            written = output.WriteString(
                StringView(names.Data() + nodes[index].nameOffset, nodes[index].nameBytes));
        }
    }

    for (usize index = 0; written && index < nodes.Size(); ++index) {
        written = output.WriteBytes(nodes[index].record.View());
    }

    if (!written) {
        return Result<Array<uint8>>::Failure(written.Error());
    }

    auto spent = budget.Spend(output.WorkUsed());
    if (!spent) {
        return Result<Array<uint8>>::Failure(spent.Error());
    }

    return Result<Array<uint8>>::Success(output.TakeBytes());
}

} // namespace

Result<ObjectGraphSaveResult> SaveObjectGraphSet(Allocator& allocator, ObjectRegistry& registry,
                                                 const ObjectAccessContext& access,
                                                 Span<const ObjectHandle> set,
                                                 ObjectGraphSaveOptions options) noexcept {
    using Output = Result<ObjectGraphSaveResult>;
    if (options.limits.maxObjects > MaxNodeCount) {
        return Output::Failure(ErrorCode::InvalidArgument);
    }

    if (set.Size() == 0) {
        return Output::Failure(ErrorCode::InvalidArgument);
    }

    for (ObjectHandle handle : set) {
        if (handle.IsNull()) {
            return Output::Failure(ErrorCode::InvalidArgument);
        }
    }

    if (options.limits.maxObjects == 0 || set.Size() > options.limits.maxObjects ||
        options.limits.maxBytes < HeaderBytes) {
        return Output::Failure(ErrorCode::CapacityExceeded);
    }

    auto budget = Budget::Create(allocator, options.limits);
    if (!budget) {
        return Output::Failure(budget.Error());
    }

    Array<SaveNode> nodes(allocator);
    GraphEncodeContext context(allocator, registry, access, nodes, budget.Value(), options);
    if (auto added = context.AddSetNodes(set); !added) {
        return Output::Failure(added.Error());
    }

    // 첫 노드의 Borrow가 저장 끝까지 GC slice를 막는다.
    auto hold = registry.Resolve<Object>(access, set[0]);
    if (!hold) {
        return Output::Failure(hold.Error());
    }

    if (auto opened = context.OpenSetNodes(); !opened) {
        return Output::Failure(opened.Error());
    }

    for (usize index = 0; index < nodes.Size(); ++index) {
        if (auto layout = ValidateLayout(nodes[index].lease, budget.Value()); !layout) {
            return Output::Failure(layout.Error());
        }
    }

    if (auto prepared = context.PrepareInclude(); !prepared) {
        return Output::Failure(prepared.Error());
    }

    usize bytes = HeaderBytes;
    for (usize index = 0; index < nodes.Size(); ++index) {
        auto saved = SaveNodeFields(allocator, registry, access, nodes, index, budget.Value(),
                                    bytes, options.writeNodeTable, context);
        if (!saved) {
            return Output::Failure(saved.Error());
        }
    }

    Array<uint32> outers(allocator);
    bool baseOuter = false;
    if (options.writeNodeTable) {
        auto resolved = ResolveNodeTable(nodes, context, outers, budget.Value());
        if (!resolved) {
            return Output::Failure(resolved.Error());
        }

        baseOuter = resolved.Value();
    }

    auto output = WriteGraph(allocator, nodes, context.Names(), outers, baseOuter, budget.Value());
    if (!output) {
        return Output::Failure(output.Error());
    }

    // 마지막 output 할당 callback까지 포함하여 모든 노드의 논리 수명을 다시 확인한 뒤 게시한다.
    Array<ObjectHandle> handles(allocator);
    if (auto reserved = handles.TryReserve(nodes.Size()); !reserved) {
        return Output::Failure(reserved.Error());
    }

    for (usize index = 0; index < nodes.Size(); ++index) {
        auto lifecycle = registry.InspectLifecycle(access, nodes[index].handle);
        if (!lifecycle) {
            return Output::Failure(lifecycle.Error());
        }

        if (lifecycle.Value() != ObjectLifecycle::Alive) {
            return Output::Failure(ErrorCode::Destroyed);
        }

        if (!handles.TryPushBack(nodes[index].handle)) {
            return Output::Failure(ErrorCode::OutOfMemory);
        }
    }

    return Output::Success(SaveResultAccess::Make(Move(output.Value()), Move(handles)));
}

Result<Array<uint8>> SaveObjectGraph(Allocator& allocator, ObjectRegistry& registry,
                                     const ObjectAccessContext& access, ObjectHandle root,
                                     ObjectGraphSaveOptions options) noexcept {
    options.externals = GraphExternalPolicy::Include;
    options.writeNodeTable = true;
    const ObjectHandle set[] = {root};
    auto saved = SaveObjectGraphSet(allocator, registry, access, set, Move(options));
    if (!saved) {
        return Result<Array<uint8>>::Failure(saved.Error());
    }

    return Result<Array<uint8>>::Success(saved.Value().TakeBytes());
}

} // namespace rocks
