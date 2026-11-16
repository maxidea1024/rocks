#pragma once

#include "GraphInternal.h"

// 그래프 저장의 내부 정의다. GraphSave.cpp(노드 encode·참조 쓰기·저장 진입점),
// GraphSaveSet.cpp(저장 집합·Include·NodeTable·출력 조립), GraphExternal.cpp(외부 참조 허락·위치
// 표·번호 재배정·weak 지연 덮어쓰기)가 같이 쓴다.
namespace rocks::graph_detail {

// Include 정책에서 encode 때 저장 집합 밖이던 weak 참조 하나의 지연 항목이다(Docs/Design/
// ReflectionExtensions.ko.md 5.1절 — 모든 노드를 encode한 뒤 최종 집합으로 제자리 덮어쓴다).
// position은 값(u32) 위치로, 필드 encode 동안은 필드 writer offset이고 필드를 Record에 더한 뒤
// 노드 Record offset이다(외부 참조 위치 표와 같은 재배정 — 덮어쓰기는 출력 조립 전에 노드 Record에
// 한다). target은 대상 handle, objectIndex는 값을 가진 노드 위치(0부터), [pathOffset, pathOffset +
// pathSteps)는 진단 경로 사본(지연 경로 표 안 — 1.6절 표기라 key를 복사하지 않는다)이다.
struct WeakDeferral {
    uint32 position = 0;
    ObjectHandle target{};
    uint32 objectIndex = 0;
    uint32 pathOffset = 0;
    uint32 pathSteps = 0;
};

// 저장할 노드 하나다. lease는 집합 노드면 OpenSetNodes가, Include가 더한 노드면 발견할 때 연 대상
// 타입 lease이며 참조 검사와 SaveNodeFields가 재사용한다. outer는 Registry의 Outer(없으면
// ObjectHandle{}), 이름은 NodeTable을 쓸 때만 문맥의 이름 버퍼 [nameOffset, nameOffset +
// nameBytes)에 모은다. firstPosition은 이 노드의 첫 외부 참조 위치가 위치 표에서 차지하는 자리다
// (노드 순서로 encode하므로 노드의 위치는 [firstPosition, 다음 노드의 firstPosition)이다). nodes가
// 다시 할당될 수 있으므로 lease·노드의 주소를 보관하지 않고 index로 다시 접근한다(lease가 돌려준
// 스키마·필드 정보는 레지스트리 레코드 안이라 안정적이다).
struct SaveNode {
    SaveNode(Allocator& allocator, ObjectHandle value) noexcept : handle(value), record(allocator) {
    }

    ObjectHandle handle;
    ObjectHandle outer{};
    TypeLease lease;
    TypeId type{};
    usize nameOffset = 0;
    usize nameBytes = 0;
    usize firstPosition = 0;
    Array<uint8> record;
};

// graph v2 저장 문맥이다: 강한 참조는 대상 타입을 검사한 뒤 U32로 쓴다(집합 안이면 local ID). 집합
// 밖 대상은 정책대로 거부(Reject)하거나, 노드로 더하거나(Include — Outer 사슬 먼저), hook이
// 허락하면 외부 참조로 쓰고 값 위치를 위치 표에 기록한다(Admit — 5.3절). nodes가 다시 할당될 수
// 있으므로 원소 참조를 보관하지 않는다. 참조 비용은 참조 수에 선형이다(5.6절): handle → local ID와
// handle → 외부 slot 해시 표(키는 domain을 포함한 handle 전체 — 캐시 적중이 domain 검사를 건너뛰지
// 않게), 노드·외부 대상별 TypeLease, 기대 TypeId → lease 캐시를 쓰고 조회·삽입마다 작업량 1을
// 청구한다. 위치 표는 참조 하나에 4 byte이고 그 메모리를 그래프 바이트 누계와 작업량에 청구한다.
// weak 참조(리플렉션 B9d1)는 집합 안이면 local ID, 밖이면 정책대로 null + 진단(값 2)이고, Include는
// 지연 항목(WeakDeferral과 경로 사본)을 기록해 모든 노드 encode 뒤 ResolveWeakDeferrals가 최종
// 집합으로 덮어쓴다 — 지연 항목 메모리도 위치 표처럼 그래프 바이트 누계·작업량에 청구한다.
class GraphEncodeContext final : public FieldEncodeContext {
public:
    GraphEncodeContext(Allocator& allocator, ObjectRegistry& registry,
                       const ObjectAccessContext& access, Array<SaveNode>& nodes, Budget& budget,
                       const ObjectGraphSaveOptions& options) noexcept
        : FieldEncodeContext(options.policy, options.hooks, ReferenceWire), allocator_(allocator),
          registry_(registry), access_(access), nodes_(nodes), budget_(budget),
          externalPolicy_(options.externals), writeNodeTable_(options.writeNodeTable),
          excludeUnsaveable_(options.unsaveable == UnsaveablePolicy::Exclude),
          packagePolicy_(registry.Policy().packageNativeType != nullptr),
          scratch_(allocator, budget), observation_(allocator, budget, false), locals_(allocator),
          expected_(allocator), firstAncestors_(allocator), chain_(allocator), names_(allocator),
          slots_(allocator), externals_(allocator), externalLeases_(allocator),
          positions_(allocator), weakDeferrals_(allocator), weakPaths_(allocator) {
        // Name 필드는 이 Registry의 pool로 표시 문자열을 읽는다.
        SetNames(registry.Names());
    }

    // 노드 하나의 encode를 시작한다. fileBytes는 이 노드 앞까지의 출력 byte 누계다(위치 표 메모리
    // 상한 검사에 쓴다).
    void BeginObject(TypeId type, uint32 index, usize fileBytes) noexcept {
        SetObject(type, index);
        committedBytes_ = fileBytes;
    }

    // 필드 하나의 encode 동안 직렬화기가 준 필드 writer다(외부 참조 값의 위치를 이 writer 안
    // offset으로 기록한다 — 다른 writer에 쓴 외부 참조는 InvalidArgument). null이면 필드 밖이다.
    void SetFieldWriter(const ArchiveWriter* writer) noexcept {
        fieldWriter_ = writer;
    }

    // 외부 참조 위치 표(GraphExternal.cpp)다: 기록한 위치 수와 그 메모리 byte, [first, end) 위치에
    // base를 더하기(필드 writer offset → Record offset → 출력 offset — u32를 넘으면
    // CapacityExceeded, 위치마다 작업량 1).
    usize PositionCount() const noexcept {
        return positions_.Size();
    }

    usize PositionBytes() const noexcept {
        return positions_.Size() * sizeof(uint32);
    }

    Result<void> RebasePositions(usize first, usize end, usize base) noexcept;

    // weak 지연 항목(Include — GraphExternal.cpp)이다: 기록한 수, [first, 끝) 항목 위치에 base를
    // 더하기(필드 writer offset → Record offset — u32를 넘으면 CapacityExceeded, 항목마다 작업량
    // 1).
    usize WeakDeferralCount() const noexcept {
        return weakDeferrals_.Size();
    }

    Result<void> RebaseWeakDeferrals(usize first, usize base) noexcept;

    // 그래프 바이트 누계에 드는 저장 표 메모리다: 외부 참조 위치 표와 weak 지연 항목·경로 사본.
    usize TableBytes() const noexcept {
        return PositionBytes() + weakDeferrals_.Size() * sizeof(WeakDeferral) +
               weakPaths_.Size() * sizeof(PathStep);
    }

    // Include: 모든 노드를 encode한 뒤 weak 지연 항목을 기록 순서대로 최종 집합 기준으로 노드
    // Record에 덮어쓴다(집합 안이면 local ID, 밖이면 0 그대로 두고 그 항목의 위치로
    // ExternalReference 진단 값 2). 항목마다 작업량 2(조회·덮어쓰기)다. 출력 조립(WriteGraph) 전에
    // 부른다.
    Result<void> ResolveWeakDeferrals() noexcept;

    // 저장 결과로 넘길 외부 참조 목록(slot 순)과 위치 표다.
    Array<GraphExternal>& Externals() noexcept {
        return externals_;
    }

    Array<uint32>& Positions() noexcept {
        return positions_;
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

    // ClassRef 값의 클래스 판정이다(리플렉션 B9d2 — C++가 직접 넣은 값을 저장 때 다시 본다). 이
    // Registry의 TypeRegistry를 소유 스레드 조회(AcquireRegisteredType)로 연다. 조회 몫은 codec이
    // Spend했다.
    Result<TypeLease> AcquireClassReference(TypeId type, TypeId base) noexcept override {
        return detail::CheckClassReferenceLease(registry_.AcquireRegisteredType(access_, type),
                                                base, nullptr);
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

    // weak 참조 쓰기(GraphSave.cpp)다.
    Result<void> WriteWeakReferenceWire(ArchiveWriter& out, ObjectHandle target,
                                        const ReferenceExpectation& expectation) noexcept override;

    // 저장 불가 대상(UnsaveablePolicy::Exclude — 리플렉션 B9h2)의 참조 하나를 null로 쓴다: 대상
    // 자신이나 집합 밖의 Outer 조상(집합 노드에서 멈춘다 — 조상마다 작업량 1) 중 하나라도 저장 불가
    // 클래스면(UE SavePackage가 저장 불가 Outer 아래 객체도 빼는 것과 같다) ExternalReference
    // 진단(value 3 = Transient, 4 = Deprecated — 처음 만난 저장 불가 클래스) 뒤 강한 참조면
    // Required 규칙을 적용한다. 그래서 Include·Admit은 그런 대상과 조상을 노드·import로 넣지
    // 않는다. 대상이 저장 불가 사슬이 아니거나 Keep이면 거짓을 돌려주고 아무것도 쓰지 않는다.
    Result<bool> WriteUnsaveable(ArchiveWriter& out, ObjectHandle target, const TypeLease& actual,
                                 bool strong) noexcept;

private:
    // Include·이름(GraphSaveSet.cpp), 기대 타입 캐시(GraphSave.cpp), 외부 참조(GraphExternal.cpp)
    // 다.
    Result<void> RejectExternal() noexcept;
    Result<uint32> IncludeNode(ObjectHandle target, TypeLease lease) noexcept;
    Result<uint32> AddNode(ObjectHandle handle, ObjectHandle outer, TypeLease lease) noexcept;
    Result<void> AppendName(SaveNode& node) noexcept;
    Result<const TypeLease*> ExpectedLease(TypeId type) noexcept;
    // Admit: 처음 보는(또는 앞 결정이 Null인) 대상의 허락을 hook에 묻고 결과대로 쓴다.
    Result<void> AdmitExternal(ArchiveWriter& out, ObjectHandle target, TypeLease lease,
                               const ReferenceExpectation& expectation) noexcept;
    // Import된 slot의 참조 하나를 집계하고 위치를 기록한 뒤 0x80000000 | slot을 쓴다.
    Result<void> WriteImported(ArchiveWriter& out, uint32 slot) noexcept;
    // 저장 집합 밖 weak 대상 하나를 정책대로 처리한다(Reject·Admit은 null + 진단, Include는 지연
    // 항목 — GraphExternal.cpp). lease는 대상의 실제 타입이다.
    Result<void> WriteExternalWeak(ArchiveWriter& out, ObjectHandle target, const TypeLease& lease,
                                   const ReferenceExpectation& expectation) noexcept;
    // Include: 지연 항목(지금 위치·경로 사본)을 기록하고 자리값 0을 쓴다.
    Result<void> DeferWeak(ArchiveWriter& out, ObjectHandle target) noexcept;
    // 위치 표·지연 항목 하나가 더 늘어도 그래프 바이트 누계 안인지 본다(늘어날 byte = added).
    Result<void> CheckTableGrowth(usize added) noexcept;

    Allocator& allocator_;
    ObjectRegistry& registry_;
    const ObjectAccessContext& access_;
    Array<SaveNode>& nodes_;
    Budget& budget_;
    GraphExternalPolicy externalPolicy_;
    bool writeNodeTable_;
    // UnsaveablePolicy::Exclude인지다(리플렉션 B9h2).
    bool excludeUnsaveable_;
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
    // Admit: Import한 대상 handle → slot(1부터), slot 순 외부 대상과 그 실제 타입 lease(대상 검사
    // 캐시), 외부 참조 값 위치 표(출현 순 — 필드 encode 동안은 필드 writer offset, 필드를 Record에
    // 더한 뒤 Record offset, 출력 조립 뒤 출력 offset).
    HashMap<ObjectHandle, uint32, HandleHash> slots_;
    Array<GraphExternal> externals_;
    Array<TypeLease> externalLeases_;
    Array<uint32> positions_;
    // Include의 weak 지연 항목(기록 순)과 그 진단 경로 사본이다.
    Array<WeakDeferral> weakDeferrals_;
    Array<PathStep> weakPaths_;
    const ArchiveWriter* fieldWriter_ = nullptr;
    usize committedBytes_ = 0;
};

// NodeTable의 outer 값과 BaseOuter 비트를 정한다(GraphSaveSet.cpp).
Result<bool> ResolveNodeTable(const Array<SaveNode>& nodes, const GraphEncodeContext& context,
                              Array<uint32>& outers, Budget& budget) noexcept;

// header(externalCount = 외부 대상 수)·NodeTable·Record를 출력 하나로 쓰고, 노드마다 그 Record의
// 출력 시작 offset을 외부 참조 위치에 더한다(Record offset → 출력 offset — GraphSaveSet.cpp).
Result<Array<uint8>> WriteGraph(Allocator& allocator, const Array<SaveNode>& nodes,
                                GraphEncodeContext& context, const Array<uint32>& outers,
                                bool baseOuter, Budget& budget) noexcept;

} // namespace rocks::graph_detail
