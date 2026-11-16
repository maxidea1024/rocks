#include "GraphInternal.h"

// 그래프 로드의 Required 실효 값 진단(2c 단계)이다(Docs/Design/ObjectFoundations.ko.md 4절). 적용을
// 마친 staged 객체를 저장과 같은 encode 경로의 검사 모드로 읽어, 파일 값이든 기준 값(생성자 값)이든
// 실효 값이 null인 Required 위치를 RequiredMissing으로 한 번 보고한다. 출력은 버린다.
namespace rocks::graph_detail {
namespace {

// 크기·작업량 한도가 없는 writer·중첩 Record 한도다(값 한도·그래프 바이트 한도는 1단계가 파일 값에
// 걸었고 생성자 값은 신뢰 상태다 — 값 한도를 넘는 생성자 배열도 검사한다).
ArchiveLimits UnboundedArchive() noexcept {
    ArchiveLimits limits;
    limits.maxTotalBytes = NumericLimits<usize>::Max();
    limits.maxValueBytes = NumericLimits<usize>::Max();
    limits.maxOperations = NumericLimits<uint64>::Max();
    limits.maxWork = NumericLimits<uint64>::Max();
    return limits;
}

// 검사 문맥이다. staged 트랜잭션에 검사 중 표시를 세우고(문맥이 끝날 때 내린다) 진단은 로드
// hook으로 보낸다. 제외 플래그는 로드 정책의 excludedFlags(와 Transient)이고
// rejectMissingRequired는 쓰지 않는다(검사 모드는 거부하지 않고 보고한다). 작업량·원소·필드 누계를
// 세지 않는다.
class RequiredCheckContext final : public FieldEncodeContext {
public:
    RequiredCheckContext(Allocator& allocator, ConstructionContext& transaction,
                         const ObjectGraphLoadOptions& options) noexcept
        : FieldEncodeContext(FieldEncodePolicy{false, options.policy.excludedFlags}, options.hooks,
                             ReferenceWire),
          allocator_(allocator) {
        BeginRequiredCheck(transaction);
        // Name은 Required를 품지 않아 검사 대상이 아니지만, 같은 encode 경로가 트랜잭션 Registry의
        // pool로 읽도록 둔다.
        SetNames(transaction.Registry().Names());
    }

    // 진단의 objectType·objectIndex(노드 위치, 0부터)를 정한다.
    void BeginObject(TypeId type, uint32 index) noexcept {
        SetObject(type, index);
    }

    Result<void> Spend(uint64) noexcept override {
        return Result<void>::Success();
    }

    Result<void> ReserveApply(uint64) noexcept override {
        return Result<void>::Success();
    }

    Allocator& Scratch() noexcept override {
        return allocator_;
    }

    // 검사는 관찰하지 않는다(계약상 부르지 않는다 — 공통 가상 구현).
    Allocator& ObservationScratch() noexcept override {
        return allocator_;
    }

    Result<void> AddFields(usize) noexcept override {
        return Result<void>::Success();
    }

    Result<void> AddElements(usize) noexcept override {
        return Result<void>::Success();
    }

    RecordLimits NestedRecordLimits() const noexcept override {
        return {UnboundedArchive(), NumericLimits<uint32>::Max()};
    }

protected:
    // 대상을 검사하지 않고 자리값만 쓴다(대상은 같은 트랜잭션 객체 또는 2b가 확인한 외부 참조
    // 대상 — 1단계가 범위·기대 형식을 검증했고 Commit이 다시 검증한다). wire 형식은 그래프와
    // 같다(graph v2 = U32).
    Result<void> WriteReferenceWire(ArchiveWriter& out, ObjectHandle target,
                                    const ReferenceExpectation&) noexcept override {
        return out.WriteU32(target == ObjectHandle{} ? 0 : 1);
    }

    // weak는 Required가 될 수 없어 검사 대상이 아니다. 같은 encode 경로가 struct 값 안에서 지나갈
    // 때를 위해 자리값만 쓴다(출력은 버린다).
    Result<void> WriteWeakReferenceWire(ArchiveWriter& out, ObjectHandle target,
                                        const ReferenceExpectation&) noexcept override {
        return out.WriteU32(target.IsNull() ? 0 : 1);
    }

private:
    Allocator& allocator_;
};

// 파일에 있는 Required를 품은 필드(검사 대상 — detail::CarriesRequired) 중 가장 큰 payload다. 검사
// 출력은 그 payload를 넘지 않으므로(같은 값을 같은 형식으로 쓰고, struct 값은 Required를 품지 않는
// 멤버를 건너뛰어 더 작다 — 파일에 없던 Required 멤버의 기준 값이 더해질 때만 넘는다) 검사 writer를
// 이만큼 미리 잡아 성장 중 옛·새 버퍼가 함께 있는 피크를 피한다. 이 값은 1단계가 maxValueBytes로
// 검증한 크기다.
usize LargestStoredRequired(const Array<LoadNode>& nodes, uint32 excludedFlags) noexcept {
    usize largest = 0;
    for (const LoadNode& node : nodes) {
        const TypeSchema& schema = node.lease.Schema();
        for (const StoredField& stored : node.fields) {
            const FieldSchema& field = schema.fields[stored.schemaIndex];
            if ((field.flags & excludedFlags) == 0 && stored.view.payload.Size() > largest &&
                detail::CarriesRequired(field, node.lease.FieldStruct(stored.schemaIndex))) {
                largest = stored.view.payload.Size();
            }
        }
    }

    return largest;
}

} // namespace

Result<void> CheckRequiredFields(Allocator& allocator, ConstructionContext& transaction,
                                 const Array<LoadNode>& nodes,
                                 const ObjectGraphLoadOptions& options) noexcept {
    // 진단을 받을 곳이 없으면 검사할 일이 없다(검사는 거부하지 않고 보고만 한다).
    if (!options.hooks) {
        return Result<void>::Success();
    }

    RequiredCheckContext context(allocator, transaction, options);
    // 필드마다 같은 writer를 비워 다시 쓴다(용량은 남는다). 파일에 있는 Required를 품은 가장 큰
    // 필드 크기로 미리 잡고, 그보다 큰 검사 출력(빠진 필드의 기준 값)만 기하 성장한다.
    ArchiveWriter writer(allocator, UnboundedArchive());
    if (auto reserved = writer.Reserve(LargestStoredRequired(nodes, options.policy.excludedFlags));
        !reserved) {
        return reserved;
    }

    for (usize index = 0; index < nodes.Size(); ++index) {
        const LoadNode& node = nodes[index];
        const TypeLease& lease = node.lease;
        const TypeSchema& schema = lease.Schema();
        context.BeginObject(node.type, static_cast<uint32>(index));
        for (usize fieldIndex = 0; fieldIndex < schema.fields.Size(); ++fieldIndex) {
            const FieldSchema& field = schema.fields[fieldIndex];
            const StructView structure = lease.FieldStruct(fieldIndex);
            if (!detail::CarriesRequired(field, structure)) {
                continue;
            }

            // 최상위 필드의 declaringType은 객체 타입이다(저장과 같다).
            FieldScope scope(context, field, schema.id, lease.FieldEnum(fieldIndex), structure,
                             FieldCodecFrame::NoIndex, lease.FieldKeyEnum(fieldIndex));
            // Transient와 이 데이터에 없어야 하는 플래그(쿠킹 로드의 EditorOnly)는 저장하지
            // 않으므로 검사하지 않는다(struct 안 멤버는 EncodeStructRecord가 같은 규칙으로
            // 건너뛴다).
            if (context.IsExcluded(context.EffectiveFlags())) {
                continue;
            }

            // 1단계의 ValidateLayout이 Transient가 아닌 모든 필드의 codec을 확인했다.
            const RuntimeFieldOps* ops = lease.FieldOps(fieldIndex);
            if (!ops || !ops->encode) {
                return Result<void>::Failure(ErrorCode::Unsupported);
            }

            writer.Clear();
            auto wire = ops->encode(*ops, transaction.Registry(), transaction.Access(), node.handle,
                                    writer, context);
            if (!wire) {
                return Result<void>::Failure(wire.Error());
            }
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::graph_detail
