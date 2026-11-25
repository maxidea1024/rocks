#include "GraphSaveInternal.h"

#include <Object/ClassRef.h>
#include <Object/TypeRegistry.h>
#include <ObjectSerialization/ClassDefaults.h>
#include <Serialization/Record.h>

// 기본값과 다른 위치 찾기다(Docs/Design/AssetSystem 12.8절 판정, RuntimeClassMetadata 'A5b' 같음):
// 살아 있는 객체를 그래프 저장과 같은 필드 루프(EncodeObjectFields)로 editor 정책 Record 하나에
// 인코딩하고 클래스 editor 레코드와 필드마다 wire byte를 비교한다. 노드 Record·외부 참조 표를
// 만들지 않는다 — 객체 참조는 null이면 0, 아니면 1로 써서 기준(늘 null = 0)과 언제나 다르다(그래프
// 밖 대상도 — 외부 참조 정책을 거치지 않는다).
namespace rocks {
namespace {

using namespace graph_detail;

// 판정 인코딩 문맥이다(Registry로 읽는다, hook 없음).
class ChangedEncodeContext final : public FieldEncodeContext {
public:
    ChangedEncodeContext(Allocator& allocator, ObjectRegistry& registry,
                         const ObjectAccessContext& access, Budget& budget, TypeId type) noexcept
        : FieldEncodeContext(FieldEncodePolicy{}, nullptr, ReferenceWire), registry_(registry),
          access_(access), budget_(budget), scratch_(allocator, budget),
          observation_(allocator, budget, false) {
        SetObject(type, 0);
        SetNames(registry.Names());
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

    Allocator& ObservationScratch() noexcept override {
        return observation_;
    }

    Result<void> AddFields(usize count) noexcept override {
        return budget_.AddFields(count);
    }

    Result<void> AddElements(usize count) noexcept override {
        return budget_.AddElements(count);
    }

    Result<TypeLease> AcquireClassReference(TypeId type, TypeId base) noexcept override {
        return detail::CheckClassReferenceLease(registry_.Types().Acquire(type), base, nullptr);
    }

    RecordLimits NestedRecordLimits() const noexcept override {
        RecordLimits limits = budget_.Record();
        limits.archive.maxTotalBytes = budget_.limits.maxValueBytes;
        return limits;
    }

protected:
    // 기준 참조는 늘 null이다 — non-null은 0이 아닌 표지(1)로 써서 언제나 다르게 한다.
    Result<void> WriteReferenceWire(ArchiveWriter& out, ObjectHandle target,
                                    const ReferenceExpectation&) noexcept override {
        return out.WriteU32(target.IsNull() ? 0u : 1u);
    }

    // weak는 그래프 저장처럼 죽은 대상(StaleHandle·Destroyed·Alive 아님)을 null(0)로 쓴다 — 저장
    // 결과와 같은 판정.
    Result<void> WriteWeakReferenceWire(ArchiveWriter& out, ObjectHandle target,
                                        const ReferenceExpectation&) noexcept override {
        if (target.IsNull()) {
            return out.WriteU32(0);
        }

        auto lifecycle = registry_.InspectLifecycle(access_, target);
        if (!lifecycle) {
            const ErrorCode error = lifecycle.Error();
            return error == ErrorCode::StaleHandle || error == ErrorCode::Destroyed
                       ? out.WriteU32(0)
                       : Result<void>::Failure(error);
        }

        return out.WriteU32(lifecycle.Value() == ObjectLifecycle::Alive ? 1u : 0u);
    }

private:
    ObjectRegistry& registry_;
    const ObjectAccessContext& access_;
    Budget& budget_;
    BudgetAllocator scratch_;
    BudgetAllocator observation_;
};

class ChangedFieldSink final : public ObjectFieldSink {
public:
    explicit ChangedFieldSink(const TypeLease& lease) noexcept : lease_(lease) {
    }

    const TypeLease& Lease() noexcept override {
        return lease_;
    }

private:
    const TypeLease& lease_;
};

bool SameChangedBytes(Span<const uint8> left, Span<const uint8> right) noexcept {
    if (left.Size() != right.Size()) {
        return false;
    }

    for (usize index = 0; index < left.Size(); ++index) {
        if (left[index] != right[index]) {
            return false;
        }
    }

    return true;
}

// 다른 위치 하나를 결과에 더한다(경로 = steps[0, count)).
Result<void> AddChanged(const PathStep* steps, usize count,
                        Array<PropertyPathBuffer>& out) noexcept {
    PropertyPathBuffer path;
    for (usize index = 0; index < count; ++index) {
        path.steps[index] = steps[index];
    }

    path.count = count;
    return out.TryPushBack(path);
}

RecordLimits ChangedReadLimits() noexcept {
    const ObjectGraphLimits graph{};
    RecordLimits limits;
    limits.archive.maxTotalBytes = graph.maxBytes;
    limits.archive.maxValueBytes = graph.maxValueBytes;
    limits.archive.maxWork = graph.maxWork;
    limits.archive.maxOperations = graph.maxWork;
    limits.maxFields = graph.maxTotalFields;
    return limits;
}

// 두 Record(같은 스키마)의 필드를 스키마 순서로 비교해 다른 위치를 더한다. 비Atomic 단일 struct는
// 멤버로 내려가고(A5b override 단위와 같은 펼침), 그 밖(단일 값·Atomic struct·배열·Map·Set·
// Optional·InstancedStruct)은 그 필드 경로다. 한쪽에만 있는 필드(제외 판정이 다른 구성 — 생기지
// 않는다)도 다름이다.
Result<void> CompareChangedRecords(Allocator& allocator, const TypeSchema& schema, StructView view,
                                   const TypeLease* lease, Span<const uint8> current,
                                   Span<const uint8> baseline, PathStep* steps, usize depth,
                                   Array<PropertyPathBuffer>& out) noexcept {
    auto now = RecordReader::Open(allocator, current, schema.id, schema.version, schema.version,
                                  ChangedReadLimits());
    auto base = RecordReader::Open(allocator, baseline, schema.id, schema.version, schema.version,
                                   ChangedReadLimits());
    if (!now || !base) {
        return Result<void>::Failure(now ? base.Error() : now.Error());
    }

    if (depth >= MaxPropertyPathSteps) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    for (usize index = 0; index < schema.fields.Size(); ++index) {
        const FieldSchema& field = schema.fields[index];
        auto left = now.Value().Find(field.id);
        auto right = base.Value().Find(field.id);
        const bool hasLeft = static_cast<bool>(left);
        const bool hasRight = static_cast<bool>(right);
        if ((!hasLeft && left.Error() != ErrorCode::NotFound) ||
            (!hasRight && right.Error() != ErrorCode::NotFound)) {
            return Result<void>::Failure(hasLeft ? right.Error() : left.Error());
        }

        if (!hasLeft && !hasRight) {
            continue;
        }

        steps[depth] = PathStep{field.id};
        if (hasLeft && hasRight && left.Value().wireType == right.Value().wireType &&
            SameChangedBytes(left.Value().payload, right.Value().payload)) {
            continue;
        }

        const StructView inner =
            lease != nullptr ? lease->FieldStruct(index) : view.FieldStruct(index);
        if (hasLeft && hasRight && field.value.kind == ValueKind::Struct && !inner.IsEmpty() &&
            !inner.IsAtomic()) {
            if (auto compared = CompareChangedRecords(allocator, inner.Schema(), inner, nullptr,
                                                      left.Value().payload, right.Value().payload,
                                                      steps, depth + 1, out);
                !compared) {
                return compared;
            }

            continue;
        }

        if (auto added = AddChanged(steps, depth + 1, out); !added) {
            return added;
        }
    }

    return Result<void>::Success();
}

} // namespace

Result<void> FindChangedFields(ObjectRegistry& registry, const ObjectAccessContext& access,
                               ObjectHandle object, Allocator& scratch,
                               Array<PropertyPathBuffer>& out) noexcept {
    out.Clear();
    auto lease = registry.AcquireType(access, object);
    if (!lease) {
        return Result<void>::Failure(lease.Error());
    }

    auto status = ClassifyClassDefaults(lease.Value(), scratch);
    if (!status) {
        return Result<void>::Failure(status.Error());
    }

    switch (status.Value().availability) {
    case ClassDefaultsAvailability::Present:
        break;
    case ClassDefaultsAvailability::WithoutRecord:
        return Result<void>::Failure(ErrorCode::Unsupported);
    case ClassDefaultsAvailability::Failed:
        return Result<void>::Failure(status.Value().diagnostic.error != ErrorCode::None
                                         ? status.Value().diagnostic.error
                                         : ErrorCode::InvalidArgument);
    }

    auto budget = Budget::Create(scratch, ObjectGraphLimits{});
    if (!budget) {
        return Result<void>::Failure(budget.Error());
    }

    const TypeSchema& schema = lease.Value().Schema();
    if (auto counted = budget.Value().AddFields(schema.fields.Size()); !counted) {
        return counted;
    }

    const RecordLimits limits = budget.Value().Record();
    // encode 동안 owner를 고정한다(그래프 저장과 같은 계약 — owner 밖 버퍼 보호).
    auto pin = registry.PinForEncode(access, object);
    if (!pin) {
        return Result<void>::Failure(pin.Error());
    }

    ChangedEncodeContext context(scratch, registry, access, budget.Value(), schema.id);
    RecordBuilder record(scratch, schema.id, schema.version, limits);
    ChangedFieldSink sink(lease.Value());
    usize payloadBytes = 0;
    if (auto encoded =
            EncodeObjectFields(scratch, &registry, access, object, budget.Value(),
                               limits.archive.maxTotalBytes, context, record, sink, payloadBytes);
        !encoded) {
        return encoded;
    }

    auto built = record.Build();
    if (!built) {
        return Result<void>::Failure(built.Error());
    }

    PathStep steps[MaxPropertyPathSteps];
    auto compared = CompareChangedRecords(
        scratch, schema, StructView{}, &lease.Value(), built.Value().View(),
        status.Value().defaults->Record(ClassDefaultsConfiguration::Editor), steps, 0, out);
    if (!compared) {
        out.Clear();
    }

    return compared;
}

} // namespace rocks
