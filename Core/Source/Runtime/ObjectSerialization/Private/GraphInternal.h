#pragma once

#include <Core/Containers/HashMap.h>
#include <Object/GeneratedFieldCodec.h>
#include <ObjectSerialization/ObjectGraph.h>

namespace rocks::graph_detail {

// graph v2 형식(Docs/Design/ObjectFoundations.ko.md 5.1절)이다. header는 magic(8) · formatVersion
// u32 · flags u32 · nodeCount u32 · externalCount u32의 24 byte이고, 그 뒤에 [NodeTable] nodeCount
// × {Guid type, u32 outer, String name}, nodeCount × Bytes(Record)가 오며 남는 byte가 없다.
inline constexpr uint8 Magic[] = {'R', 'O', 'C', 'K', 'S', 'G', 'R', 'F'};
inline constexpr uint32 FormatVersion = 2;
inline constexpr usize HeaderBytes = 24;
// flags 비트다. BaseOuter는 NodeTable과만 쓰고 outer 0 노드의 Outer가 기준 Outer임을 뜻한다.
inline constexpr uint32 NodeTableFlag = 1;
inline constexpr uint32 BaseOuterFlag = 2;
inline constexpr uint32 KnownFlags = NodeTableFlag | BaseOuterFlag;
// NodeTable 항목 하나의 고정 byte(Guid 16 + outer 4 + 이름 길이 8 — 이름 byte는 따로)다.
inline constexpr usize NodeEntryBytes = 28;
// Record 하나를 감싼 Bytes의 길이 prefix(8)와 Record header(44)다(노드마다 최소 byte).
inline constexpr usize RecordPrefixBytes = 8;
inline constexpr usize RecordHeaderBytes = 44;
// 강한 참조 wire는 U32다: 0 = null, 1..nodeCount = 노드(local ID), ExternalReferenceBit | i(1 ≤ i ≤
// externalCount) = 외부 참조 i(A0-Oe — 지금 복원은 externalCount 0만 받으므로 언제나 범위 밖이다).
inline constexpr uint32 ReferenceWire = static_cast<uint32>(WireType::U32);
inline constexpr uint32 ExternalReferenceBit = 0x80000000u;
// local ID가 외부 참조 비트와 겹치지 않는 노드 수 상한이다(maxObjects가 넘으면 옵션 오류
// InvalidArgument).
inline constexpr uint32 MaxNodeCount = ExternalReferenceBit - 1;

// 그래프 하나의 누적 작업량 예산이다. ArchiveBudget 하나를 모든 Record 트리(값 reader 포함)와
// 공유하므로 그래프 청구와 reader 청구가 같은
// 누계다(Docs/Design/ReflectionFoundations.ko.md 6.4절).
class Budget {
public:
    static Result<Budget> Create(Allocator& allocator, ObjectGraphLimits limits) noexcept {
        auto shared = SharedPtr<ArchiveBudget>::TryMake(allocator, limits.maxWork, limits.maxWork);
        if (!shared) {
            return Result<Budget>::Failure(shared.Error());
        }

        return Result<Budget>::Success(Budget(limits, Move(shared.Value())));
    }

    Result<void> Spend(uint64 count) noexcept {
        return shared_->Spend(count);
    }

    uint64 Used() const noexcept {
        return shared_->WorkUsed();
    }

    Result<void> AddFields(usize count) noexcept {
        if (count > limits.maxTotalFields - fields) {
            return Result<void>::Failure(ErrorCode::CapacityExceeded);
        }

        fields += static_cast<uint32>(count);
        return Result<void>::Success();
    }

    // 배열 원소 누계(maxTotalElements)에 더한다(넘으면 CapacityExceeded).
    Result<void> AddElements(usize count) noexcept {
        if (count > limits.maxTotalElements - elements) {
            return Result<void>::Failure(ErrorCode::CapacityExceeded);
        }

        elements += static_cast<uint32>(count);
        return Result<void>::Success();
    }

    // 공유 예산을 쓰지 않는 writer·envelope reader의 한도다(남은 work의 스냅샷). 쓴 양은 호출자가
    // WorkUsed로 청구한다.
    ArchiveLimits Archive(bool outer = false) const noexcept {
        ArchiveLimits result;
        result.maxTotalBytes = limits.maxBytes;
        result.maxValueBytes = outer ? limits.maxBytes : limits.maxValueBytes;
        result.maxWork = limits.maxWork > Used() ? limits.maxWork - Used() : 0;
        result.maxOperations = result.maxWork;
        return result;
    }

    // Record 하나의 한도다. 필드 수 한도는 남은 그래프 누계다.
    RecordLimits Record() const noexcept {
        return {Archive(), RemainingFields()};
    }

    uint32 RemainingFields() const noexcept {
        return limits.maxTotalFields > fields ? limits.maxTotalFields - fields : 0;
    }

    uint32 RemainingElements() const noexcept {
        return limits.maxTotalElements > elements ? limits.maxTotalElements - elements : 0;
    }

    SharedPtr<ArchiveBudget> Shared() const noexcept {
        return shared_;
    }

    ObjectGraphLimits limits;
    uint32 fields = 0;
    uint32 elements = 0;
    usize migrationBytes = 0;

private:
    Budget(ObjectGraphLimits value, SharedPtr<ArchiveBudget> shared) noexcept
        : limits(value), shared_(Move(shared)) {
    }

    SharedPtr<ArchiveBudget> shared_;
};

// 값 임시 저장 할당이 크기 한도와 예산을 지나게 한다(codec 문맥의 Scratch — 단일 할당이
// maxValueBytes 이하, singleLimit 거짓이면 단일 할당 한도 없이 work 예산만 — 저장 문맥의
// ObservationScratch. 로드 1단계의 관찰 할당은 그 문맥의 Charge로 따로 청구한다). 이 adapter를
// 빌리는 값은 그 호출 안에서 파괴하며 결과·노드에 adapter 주소가 탈출하지 않는다.
class BudgetAllocator final : public Allocator {
public:
    BudgetAllocator(Allocator& allocator, Budget& budget, bool singleLimit = true) noexcept
        : allocator_(allocator), budget_(budget), singleLimit_(singleLimit) {
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        const usize maximum = budget_.limits.maxValueBytes;
        if (singleLimit_ && bytes != 0 && bytes - 1 > maximum) {
            return Result<void*>::Failure(ErrorCode::CapacityExceeded);
        }

        auto spent = budget_.Spend(bytes);
        if (!spent) {
            return Result<void*>::Failure(spent.Error());
        }

        return allocator_.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* value) noexcept override {
        return allocator_.Deallocate(value);
    }

private:
    Allocator& allocator_;
    Budget& budget_;
    bool singleLimit_;
};

// struct 원소 배열(RecordList)의 로드 목록 한도다(Docs/Design/ReflectionFoundations.ko.md 5.6절).
// 저장 codec(EncodeArrayField)과 같은 함수 detail::ArrayRecordListLimits로 만든다(항목 수
// MaxArrayElements, payload maxValueBytes, 항목 하나 그 − 12 — 저장의 NestedRecordLimits 전체
// 크기도 maxValueBytes라 같은 값이다). 1단계는 여기에 남은 원소 누계를 항목 수 한도로 더 건다.
inline RecordListLimits ArrayLoadLimits(const ObjectGraphLimits& limits) noexcept {
    return detail::ArrayRecordListLimits(limits.maxValueBytes);
}

// ObjectHandle 전체(registry domain·slot·generation)를 키로 쓰는 해시다(저장 쪽 handle → local ID
// 캐시 — 다른 Registry의 handle이 캐시로 domain 검사를 건너뛰지 않게).
struct HandleHash {
    uint64 operator()(const ObjectHandle& handle) const noexcept {
        return MixHash(handle.registry ^
                       MixHash((uint64{handle.slot} << 32) | uint64{handle.generation}));
    }
};

// 필드 codec 함수표를 찾는다(GUID별 1:1, encode·decode가 있어야 한다 — 없으면 Unsupported).
Result<const RuntimeFieldOps*> FindOps(const TypeLease&, Guid, Budget&) noexcept;
// 그래프에 저장할 수 있는 완전한 필드 배치인지 본다.
Result<void> ValidateLayout(const TypeLease&, Budget&) noexcept;

// 1단계가 보관하는 필드 하나다: 스키마 위치, 등록 함수표(node.lease가 보유), 원본·마이그레이션
// 결과를 빌리는 FieldView.
struct StoredField {
    usize schemaIndex = 0;
    const RuntimeFieldOps* ops = nullptr;
    FieldView view;
};

// 1단계가 모은 강한 참조 하나다. target은 local ID(1부터, null은 기록하지 않는다 — 배열은 non-null
// 원소마다 하나), expectation은 LoadNode::expectations의 위치(필드 안 서로 다른 기대마다 하나 —
// 원소마다 복제하지 않는다).
struct ReferenceToken {
    uint32 target = 0;
    uint32 expectation = 0;
};

// 1단계가 보관한 struct 마이그레이션 결과 하나다(발생 순서). 2단계가 같은 순서로 (type, from)을
// 대조해 쓴다. bytes의 heap 블록은 Array 이동에도 그대로라 view가 유효하다.
struct StoredStructMigration {
    explicit StoredStructMigration(Allocator& allocator) noexcept : bytes(allocator) {
    }

    TypeId type{};
    uint32 from = 0;
    Array<uint8> bytes;
};

// 1단계가 노드 하나에 보관하는 것이다. type·outer·name은 노드 표(NodeTable)에서, version은 Record
// header에서 온다. name은 원본 byte를 빌린다.
struct LoadNode {
    explicit LoadNode(Allocator& allocator) noexcept
        : migrated(allocator), fields(allocator), expectations(allocator), tokens(allocator),
          structMigrations(allocator) {
    }

    TypeId type{};
    // 0 = 그래프 밖(BaseOuter면 기준 Outer, 아니면 Outer 없음), 1..자기 번호 − 1 = 노드.
    uint32 outer = 0;
    StringView name{};
    uint32 version = 0;
    Span<const uint8> source;
    TypeLease lease;
    Array<uint8> migrated;
    Array<StoredField> fields;
    Array<ReferenceExpectation> expectations;
    Array<ReferenceToken> tokens;
    Array<StoredStructMigration> structMigrations;
    ObjectHandle handle{};
};

// graph v2 envelope를 읽은 결과다. 노드 표(NodeTable)가 없으면 노드의 type·outer·name은 비어 있다.
struct Envelope {
    explicit Envelope(Allocator& allocator) noexcept : nodes(allocator) {
    }

    uint32 flags = 0;
    uint32 externalCount = 0;
    Array<LoadNode> nodes;
};

// header·NodeTable·Record 경계를 읽는다(정의·순서와 오류는 GraphEnvelope.cpp).
Result<Envelope> ReadEnvelope(Allocator& allocator, Span<const uint8> source,
                              Budget& budget) noexcept;

// 적용을 마친 staged 노드들의 Required 실효 값을 진단한다(Docs/Design/ObjectFoundations.ko.md 4절
// — 2c 단계, GraphRequired.cpp). Required를 품은 최상위 필드(필드 자신이 Required이거나 struct
// 필드·struct 원소 배열의 struct가 ContainsRequired — detail::CarriesRequired)만 encode 경로의
// 검사 모드로 staged 객체에서 읽고 출력은 버린다(struct 값은 Required를 품지 않는 멤버를 쓰지
// 않는다). null인 Required 위치마다 options.hooks->Report(RequiredMissing)가 한 번 온다
// (거부하지 않는다). hooks가 없으면 진단을 받을 곳이 없으므로 아무것도 하지 않는다. 작업량·크기
// 한도는 걸지 않는다(파일 값은 1단계가 한도로 검증했고 생성자 값은 신뢰 상태다). writer는 파일에
// 있는 Required를 품은 가장 큰 필드의 payload로 미리 잡는다. 실패는 hook 실패·할당·readStaged 없는
// 수동 Required 필드(Unsupported)·Required 위치 자체의 저장 불가 값(예: 생성자가 둔 Required Soft의
// 규칙 위반, MaxArrayElements를 넘는 Required 배열)·계약 위반뿐이며 호출자가 rollback한다. 검사
// 동안 transaction에 검사 중 표시가 선다(그 트랜잭션의 쓰기·GetForWrite·Commit Busy).
Result<void> CheckRequiredFields(Allocator& allocator, ConstructionContext& transaction,
                                 const Array<LoadNode>& nodes,
                                 const ObjectGraphLoadOptions& options) noexcept;

// 편의 복원의 도달 검사다(GraphReachability.cpp).
Result<void> CheckReachable(Allocator& allocator, const Array<LoadNode>& nodes, bool packagePolicy,
                            bool hasBase, Budget& budget) noexcept;

// 2단계: 생성·적용·Required 검사·prepare·Commit이다(GraphApply.cpp).
Result<RootRef<Object>> ConstructGraph(Allocator& allocator, ObjectRegistry& registry,
                                       const ObjectAccessContext& access, Array<LoadNode>& nodes,
                                       const ObjectGraphLoadOptions& options) noexcept;

} // namespace rocks::graph_detail
