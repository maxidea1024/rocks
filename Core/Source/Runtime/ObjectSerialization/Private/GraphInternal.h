#pragma once

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

    SharedPtr<ArchiveBudget> Shared() const noexcept {
        return shared_;
    }

    ObjectGraphLimits limits;
    uint32 fields = 0;
    usize migrationBytes = 0;

private:
    Budget(ObjectGraphLimits value, SharedPtr<ArchiveBudget> shared) noexcept
        : limits(value), shared_(Move(shared)) {
    }

    SharedPtr<ArchiveBudget> shared_;
};

// 값 임시 저장 할당이 크기 한도와 예산을 지나게 한다(codec 문맥의 Scratch). 이 adapter를 빌리는
// 값은 그 호출 안에서 파괴하며 결과·노드에 adapter 주소가 탈출하지 않는다.
class BudgetAllocator final : public Allocator {
public:
    BudgetAllocator(Allocator& allocator, Budget& budget) noexcept
        : allocator_(allocator), budget_(budget) {
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        const usize maximum = budget_.limits.maxValueBytes;
        if (bytes != 0 && bytes - 1 > maximum) {
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

// 1단계가 모은 강한 참조 하나다. target은 local ID(1부터, null은 기록하지 않는다), expectation은
// LoadNode::expectations의 위치(필드마다 하나).
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
