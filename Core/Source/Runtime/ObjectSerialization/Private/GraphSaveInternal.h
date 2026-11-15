#pragma once

#include "GraphInternal.h"

// 그래프 저장의 내부 정의다. GraphSave.cpp(노드 encode·참조 쓰기·저장 진입점)와
// GraphSaveSet.cpp(저장 집합·Include·NodeTable·출력 조립)가 같이 쓴다.
namespace rocks::graph_detail {

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

    // 저장 집합(GraphSaveSet.cpp)이다.
    Result<void> AddSetNodes(Span<const ObjectHandle> set) noexcept;
    Result<void> OpenSetNodes() noexcept;
    Result<void> PrepareInclude() noexcept;

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
    // 참조 쓰기(GraphSave.cpp)다.
    Result<void> WriteReferenceWire(ArchiveWriter& out, ObjectHandle target,
                                    const ReferenceExpectation& expectation) noexcept override;

private:
    // Include·이름(GraphSaveSet.cpp)과 기대 타입 캐시(GraphSave.cpp)다.
    Result<void> RejectExternal() noexcept;
    Result<uint32> IncludeNode(ObjectHandle target, TypeLease lease) noexcept;
    Result<uint32> AddNode(ObjectHandle handle, ObjectHandle outer, TypeLease lease) noexcept;
    Result<void> AppendName(SaveNode& node) noexcept;
    Result<const TypeLease*> ExpectedLease(TypeId type) noexcept;

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

// NodeTable의 outer 값과 BaseOuter 비트를 정한다(GraphSaveSet.cpp).
Result<bool> ResolveNodeTable(const Array<SaveNode>& nodes, const GraphEncodeContext& context,
                              Array<uint32>& outers, Budget& budget) noexcept;

// header·NodeTable·Record를 출력 하나로 쓴다(GraphSaveSet.cpp).
Result<Array<uint8>> WriteGraph(Allocator& allocator, const Array<SaveNode>& nodes,
                                StringView names, const Array<uint32>& outers, bool baseOuter,
                                Budget& budget) noexcept;

} // namespace rocks::graph_detail
