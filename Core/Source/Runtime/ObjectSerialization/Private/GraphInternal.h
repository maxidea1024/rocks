#pragma once

#include <Core/Containers/HashMap.h>
#include <Object/GeneratedFieldCodec.h>
#include <ObjectSerialization/ObjectGraph.h>

namespace rocks::graph_detail {

inline constexpr uint8 Magic[] = {'R', 'O', 'C', 'K', 'S', 'G', 'R', 'F'};
inline constexpr usize HeaderBytes = 20;
inline constexpr usize ObjectHeaderBytes = 36;
// graph v1의 강한 참조 wire: U64 local ID(0 = null).
inline constexpr uint32 ReferenceWire = static_cast<uint32>(WireType::U64);

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

struct LoadNode {
    explicit LoadNode(Allocator& allocator) noexcept
        : migrated(allocator), fields(allocator), expectations(allocator), tokens(allocator),
          structMigrations(allocator) {
    }

    TypeId type{};
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

} // namespace rocks::graph_detail
