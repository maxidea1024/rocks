#include "GraphSaveInternal.h"

#include <Object/OverrideSet.h>
#include <ObjectSerialization/ClassDefaults.h>

namespace rocks::graph_detail {

// 저장 결과를 만드는 내부 통로다(ObjectGraphSaveResult의 생성자는 비공개다).
struct SaveResultAccess {
    static ObjectGraphSaveResult Make(Array<uint8>&& bytes, Array<ObjectHandle>&& nodes,
                                      Array<GraphExternal>&& externals, Array<uint32>&& positions,
                                      uint32 maxExternalReferences) noexcept {
        return ObjectGraphSaveResult(Move(bytes), Move(nodes), Move(externals), Move(positions),
                                     maxExternalReferences);
    }
};

Result<void>
GraphEncodeContext::WriteReferenceWire(ArchiveWriter& out, ObjectHandle target,
                                       const ReferenceExpectation& expectation) noexcept {
    if (target == ObjectHandle{}) {
        return out.WriteU32(0);
    }

    if (target.IsNull()) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (auto spent = budget_.Spend(1); !spent) {
        return spent;
    }

    // 집합 노드·Import한 외부 대상이면 그 lease로 검사하고, 아니면 대상 타입을 열어(생존·문맥 검사
    // 포함) 검사가 끝난 뒤 정책대로 처리한다.
    uint32 local = 0;
    uint32 slot = 0;
    TypeLease discovered;
    const TypeLease* actual = &discovered;
    const uint32* found = locals_.Find(target);
    if (!found && externalPolicy_ == GraphExternalPolicy::Admit) {
        if (auto spent = budget_.Spend(1); !spent) {
            return spent;
        }

        if (const uint32* imported = slots_.Find(target)) {
            slot = *imported;
        }
    }

    if (found || slot != 0) {
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

        if (found) {
            local = *found;
            actual = &nodes_[local - 1].lease;
        } else {
            actual = &externalLeases_[slot - 1];
        }
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

    // 저장 불가 대상(Exclude)은 외부 정책과 무관하게 null + 진단이다(집합 노드는 저장 전에 거부했다
    // — 리플렉션 B9h2).
    if (local == 0) {
        auto unsaveable = WriteUnsaveable(out, target, *actual, true);
        if (!unsaveable || unsaveable.Value()) {
            return unsaveable ? Result<void>::Success() : Result<void>::Failure(unsaveable.Error());
        }
    }

    if (local != 0) {
        return out.WriteU32(local);
    }

    if (slot != 0) {
        return WriteImported(out, slot);
    }

    switch (externalPolicy_) {
    case GraphExternalPolicy::Reject:
        break;
    case GraphExternalPolicy::Admit:
        return AdmitExternal(out, target, Move(discovered), expectation);
    case GraphExternalPolicy::Include: {
        auto added = IncludeNode(target, Move(discovered));
        if (!added) {
            return Result<void>::Failure(added.Error());
        }

        return out.WriteU32(added.Value());
    }
    }

    return RejectExternal();
}

// weak 참조 하나다(Docs/Design/ReflectionExtensions.ko.md 5.1절): null·비정규 null·죽은 대상
// (StaleHandle·Destroyed — 회수·재사용·논리 파괴·cutoff)은 진단 없이 0이다. 살아 있으면 강한 참조와
// 같은 기대 형식 검사(TypeMismatch) 뒤 집합 안이면 local ID, 밖이면 WriteExternalWeak다. 다른
// Registry의 handle은 WrongContext(정책·hook 질의 전)이고 접근·문맥·종료·재진입 오류는 그대로
// 돌려준다 — weak 값이 잘못된 문맥을 가리키는 것은 정상 상태가 아니다. slot 캐시(강한 참조가
// Import한 대상)는 보지 않는다.
Result<void>
GraphEncodeContext::WriteWeakReferenceWire(ArchiveWriter& out, ObjectHandle target,
                                           const ReferenceExpectation& expectation) noexcept {
    if (target.IsNull()) {
        return out.WriteU32(0);
    }

    // 다른 Registry의 handle은 정책·hook 질의 전에 WrongContext다(조회 API는 그것을 회수된 handle과
    // 같은 StaleHandle로 알리므로 먼저 가른다 — 조용한 null로 잃지 않는다).
    if (!registry_.OwnsHandle(target)) {
        return Result<void>::Failure(ErrorCode::WrongContext);
    }

    if (auto spent = budget_.Spend(1); !spent) {
        return spent;
    }

    const auto dead = [](ErrorCode code) noexcept {
        return code == ErrorCode::StaleHandle || code == ErrorCode::Destroyed;
    };
    TypeLease discovered;
    const TypeLease* actual = &discovered;
    const uint32* found = locals_.Find(target);
    if (found) {
        // 집합 노드도 참조마다 lease 없는 생존 조회를 한다(강한 참조와 같다). 죽었으면 null이다 —
        // 그 노드 자신은 마지막 생존 재확인에서 저장을 실패시킨다.
        auto lifecycle = registry_.InspectLifecycle(access_, target);
        if (!lifecycle) {
            return dead(lifecycle.Error()) ? out.WriteU32(0)
                                           : Result<void>::Failure(lifecycle.Error());
        }

        if (lifecycle.Value() != ObjectLifecycle::Alive) {
            return out.WriteU32(0);
        }

        actual = &nodes_[*found - 1].lease;
    } else {
        auto acquired = registry_.AcquireType(access_, target);
        if (!acquired) {
            return dead(acquired.Error()) ? out.WriteU32(0)
                                          : Result<void>::Failure(acquired.Error());
        }

        discovered = Move(acquired.Value());
    }

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

    if (found) {
        return out.WriteU32(*found);
    }

    auto unsaveable = WriteUnsaveable(out, target, *actual, false);
    if (!unsaveable || unsaveable.Value()) {
        return unsaveable ? Result<void>::Success() : Result<void>::Failure(unsaveable.Error());
    }

    return WriteExternalWeak(out, target, discovered, expectation);
}

Result<bool> GraphEncodeContext::WriteUnsaveable(ArchiveWriter& out, ObjectHandle target,
                                                 const TypeLease& actual, bool strong) noexcept {
    if (!excludeUnsaveable_) {
        return Result<bool>::Success(false);
    }

    uint32 flags = actual.Schema().flags & unsaveableMask_;
    for (ObjectHandle current = target; flags == 0;) {
        auto outer = registry_.GetOuter(access_, current);
        if (!outer) {
            return Result<bool>::Failure(outer.Error());
        }

        current = outer.Value();
        if (current == ObjectHandle{} || locals_.Find(current)) {
            break;
        }

        if (auto spent = budget_.Spend(1); !spent) {
            return Result<bool>::Failure(spent.Error());
        }

        auto type = registry_.AcquireType(access_, current);
        if (!type) {
            return Result<bool>::Failure(type.Error());
        }

        flags = type.Value().Schema().flags & unsaveableMask_;
    }

    if (flags == 0) {
        return Result<bool>::Success(false);
    }

    const int64 reason = (flags & static_cast<uint32>(TypeFlags::Transient)) != 0    ? 3
                         : (flags & static_cast<uint32>(TypeFlags::Deprecated)) != 0 ? 4
                                                                                     : 5;
    if (auto reported = Report(FieldDiagnosticKind::ExternalReference, reason); !reported) {
        return Result<bool>::Failure(reported.Error());
    }

    if (strong) {
        if (auto required = CheckRequired(true); !required) {
            return Result<bool>::Failure(required.Error());
        }
    }

    if (auto written = out.WriteU32(0); !written) {
        return Result<bool>::Failure(written.Error());
    }

    return Result<bool>::Success(true);
}

// 기대 TypeId의 등록 타입 lease다(저장 끝까지 캐시에 쥔다). 조회·삽입마다 작업량 1이다.
Result<const TypeLease*> GraphEncodeContext::ExpectedLease(TypeId type) noexcept {
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

Result<void> GraphEncodeContext::BeginBaseline(const TypeSchema& schema, Span<const uint8> record,
                                               const OverrideSet* overrides) noexcept {
    auto reader = RecordReader::Open(allocator_, record, schema.id, schema.version, schema.version,
                                     budget_.Record());
    if (!reader) {
        return Result<void>::Failure(reader.Error());
    }

    if (auto reserved = baselines_.TryReserve(baselines_.Size() + 1); !reserved) {
        return reserved;
    }

    (void)baselines_.TryPushBack(BaselineLevel(nullptr, Move(reader.Value())));
    overrides_ = overrides;
    return Result<void>::Success();
}

void GraphEncodeContext::EndBaseline() noexcept {
    baselines_.Clear();
    overrides_ = nullptr;
}

Result<bool> GraphEncodeContext::BeginStructBaseline() noexcept {
    const FieldCodecFrame* frame = Current();
    if (baselines_.IsEmpty() || frame == nullptr || frame->index != FieldCodecFrame::NoIndex ||
        frame->field == nullptr || frame->field->value.kind != ValueKind::Struct ||
        frame->structure.IsEmpty() || frame->structure.IsAtomic()) {
        return Result<bool>::Success(false);
    }

    // 기준 사슬 맨 위가 바로 바깥 위치여야 한다(원소·InstancedStruct 내용·다른 struct의 멤버는
    // 기준이 없다).
    BaselineLevel& top = baselines_[baselines_.Size() - 1];
    if (top.frame != frame->parent) {
        return Result<bool>::Success(false);
    }

    auto found = top.reader.Find(frame->field->id);
    if (!found) {
        // 기준 레코드에 없는 위치(그 구성에서 제외)는 기준 없이 쓴다.
        return found.Error() == ErrorCode::NotFound ? Result<bool>::Success(false)
                                                    : Result<bool>::Failure(found.Error());
    }

    const TypeSchema& schema = frame->structure.Schema();
    auto nested = top.reader.OpenRecordField(allocator_, found.Value(), schema.id, schema.version,
                                             schema.version, budget_.RemainingFields());
    if (!nested) {
        return Result<bool>::Failure(nested.Error());
    }

    if (auto reserved = baselines_.TryReserve(baselines_.Size() + 1); !reserved) {
        return Result<bool>::Failure(reserved.Error());
    }

    (void)baselines_.TryPushBack(BaselineLevel(frame, Move(nested.Value())));
    return Result<bool>::Success(true);
}

void GraphEncodeContext::EndStructBaseline() noexcept {
    if (!baselines_.IsEmpty()) {
        (void)baselines_.TryRemoveAt(baselines_.Size() - 1);
    }
}

Result<bool> GraphEncodeContext::OmitCurrent(uint32 wire, Span<const uint8> payload,
                                             usize referenceMark) noexcept {
    const FieldCodecFrame* frame = Current();
    if (baselines_.IsEmpty() || frame == nullptr || frame->field == nullptr ||
        frame->index != FieldCodecFrame::NoIndex) {
        return Result<bool>::Success(false);
    }

    BaselineLevel& top = baselines_[baselines_.Size() - 1];
    // 바로 바깥이 기준 사슬 맨 위인 위치만 비교한다. 그 값을 쓰는 동안 외부 참조 위치·weak 지연
    // 항목이 늘었으면(Include weak 자리값 0 포함) byte가 같아도 쓴다(A5b 교차 검증 필수 1).
    if (top.frame != frame->parent || ReferenceMark() != referenceMark) {
        return Result<bool>::Success(false);
    }

    // 멤버 비교로 쓴 struct 값은 남은 멤버가 없을 때 생략한다(잎 override는 멤버로 남는다).
    const FieldSchema& field = *frame->field;
    if (field.value.kind == ValueKind::Struct && !frame->structure.IsEmpty() &&
        !frame->structure.IsAtomic()) {
        auto found = top.reader.Find(field.id);
        if (!found) {
            return found.Error() == ErrorCode::NotFound ? Result<bool>::Success(false)
                                                        : Result<bool>::Failure(found.Error());
        }

        auto header = ReadRecordHeader(payload);
        if (!header) {
            return Result<bool>::Failure(header.Error());
        }

        return Result<bool>::Success(header.Value().fieldCount == 0);
    }

    if (overrides_ != nullptr) {
        PathStep steps[MaxPropertyPathSteps];
        if (overrides_->Contains(BuildPath(steps))) {
            return Result<bool>::Success(false);
        }
    }

    auto found = top.reader.Find(field.id);
    if (!found) {
        return found.Error() == ErrorCode::NotFound ? Result<bool>::Success(false)
                                                    : Result<bool>::Failure(found.Error());
    }

    const FieldView& base = found.Value();
    if (base.wireType != wire || base.payload.Size() != payload.Size()) {
        return Result<bool>::Success(false);
    }

    for (usize index = 0; index < payload.Size(); ++index) {
        if (base.payload[index] != payload[index]) {
            return Result<bool>::Success(false);
        }
    }

    return Result<bool>::Success(true);
}

// TLV 하나의 머리 byte다(id 16 + wire 4 + 길이 8 — Record.h).
constexpr usize FieldHeaderBytes = 28;

Result<void> EncodeObjectFields(Allocator& allocator, ObjectRegistry* registry,
                                const ObjectAccessContext& access, ObjectHandle handle,
                                Budget& budget, usize maxFieldBytes, FieldEncodeContext& context,
                                RecordBuilder& record, ObjectFieldSink& sink,
                                usize& payloadBytes) noexcept {
    const auto& schema = sink.Lease().Schema();
    for (usize fieldIndex = 0; fieldIndex < schema.fields.Size(); ++fieldIndex) {
        const auto& field = schema.fields[fieldIndex];
        // 최상위 필드의 declaringType은 객체 타입이다(상속 필드도 펼친 스키마로 다룬다).
        const TypeLease& type = sink.Lease();
        FieldScope scope(context, field, schema.id, type.FieldEnum(fieldIndex),
                         type.FieldStruct(fieldIndex), FieldCodecFrame::NoIndex,
                         type.FieldKeyEnum(fieldIndex));
        // Transient 참조는 GC 수명에는 참여하지만 저장 그래프에는 참여하지 않는다.
        if (context.ExcludesCurrent()) {
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
        fieldLimits.maxTotalBytes = maxFieldBytes;
        ArchiveWriter encoded(allocator, fieldLimits);
        const usize referenceMark = context.ReferenceMark();
        sink.BeginField(encoded);
        auto wire =
            accessor.Value()->encode(*accessor.Value(), registry, access, handle, encoded, context);
        sink.EndField();
        if (!wire) {
            return Result<void>::Failure(wire.Error());
        }

        // codec이 스키마 필드의 wire(배열은 원소에 따라 ValueArray·RecordList)를 썼는지 다시
        // 확인한다.
        auto expected = detail::FieldWireType(field, context.ReferenceWireType());
        if (!expected || expected.Value() != wire.Value()) {
            return Result<void>::Failure(ErrorCode::TypeMismatch);
        }

        // encode 작업량은 생략하는 값도 청구한다(기준과 같은 큰 값이 작업량 한도를 비켜 가지 않게).
        spent = budget.Spend(encoded.WorkUsed());
        if (!spent) {
            return spent;
        }

        // 차이 쓰기(A5b): 기준과 같으면 Record에 더하지 않는다(위치·지연 항목이 늘지 않은 값만).
        auto omit = context.OmitCurrent(wire.Value(), encoded.View(), referenceMark);
        if (!omit) {
            return Result<void>::Failure(omit.Error());
        }

        if (omit.Value()) {
            continue;
        }

        usize appended = 0;
        if (!TryAdd(FieldHeaderBytes, encoded.ByteCount(), appended)) {
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

        if (auto noted = sink.FieldAdded(record.LastFieldPayloadOffset()); !noted) {
            return noted;
        }

        if (!TryAdd(payloadBytes, appended, payloadBytes)) {
            return Result<void>::Failure(ErrorCode::Overflow);
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::graph_detail

namespace rocks {
namespace {

using namespace graph_detail;

// 그래프 저장 노드의 필드 루프 쪽 일이다: 필드 writer를 문맥에 걸고, 필드를 Record에 더한 뒤 그
// 필드의 외부 참조 위치·weak 지연 항목을 필드 writer offset에서 Record offset으로 바꾼다.
class NodeFieldSink final : public ObjectFieldSink {
public:
    NodeFieldSink(Array<SaveNode>& nodes, usize nodeIndex, GraphEncodeContext& context) noexcept
        : nodes_(nodes), nodeIndex_(nodeIndex), context_(context) {
    }

    const TypeLease& Lease() noexcept override {
        return nodes_[nodeIndex_].lease;
    }

    // 이 필드의 외부 참조 위치는 [fieldPosition_, 끝), weak 지연 항목은 [fieldWeak_, 끝)이고 필드
    // writer 안 offset으로 기록된다.
    void BeginField(ArchiveWriter& writer) noexcept override {
        fieldPosition_ = context_.PositionCount();
        fieldWeak_ = context_.WeakDeferralCount();
        context_.SetFieldWriter(&writer);
    }

    void EndField() noexcept override {
        context_.SetFieldWriter(nullptr);
    }

    Result<void> FieldAdded(usize payloadOffset) noexcept override {
        if (auto rebased =
                context_.RebasePositions(fieldPosition_, context_.PositionCount(), payloadOffset);
            !rebased) {
            return rebased;
        }

        return context_.RebaseWeakDeferrals(fieldWeak_, payloadOffset);
    }

private:
    Array<SaveNode>& nodes_;
    usize nodeIndex_;
    GraphEncodeContext& context_;
    usize fieldPosition_ = 0;
    usize fieldWeak_ = 0;
};

// 노드 하나의 필드를 encode해 Record를 만든다. fileBytes는 지금까지의 출력 byte 누계이고 이 노드의
// Bytes(Record)와(NodeTable이면) 노드 항목, 외부 참조 위치 표 메모리를 더해 maxBytes와 비교한다.
// 필드마다 외부 참조 위치를 필드 writer offset에서 Record offset으로 바꾼다.
Result<void> SaveNodeFields(Allocator& allocator, ObjectRegistry& registry,
                            const ObjectAccessContext& access, Array<SaveNode>& nodes,
                            usize nodeIndex, Budget& budget, usize& fileBytes, bool nodeTable,
                            GraphBaseline baseline, const GraphOverrides* overrides,
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
    // 남은 파일 byte(이 노드의 Record 길이 prefix와 NodeTable 항목, 지금까지의 위치 표를 뺀 것)라
    // 넘칠 Record를 끝까지 만들지 않고, 노드 Record들의 합이 maxBytes로 묶인다(필드 writer도 같은
    // 한도로 연다).
    const usize entryBytes = nodeTable ? NodeEntryBytes + nodes[nodeIndex].nameBytes : 0;
    const usize firstPosition = context.PositionCount();
    usize reserved = 0;
    if (!TryAdd(fileBytes, RecordPrefixBytes, reserved) ||
        !TryAdd(reserved, entryBytes, reserved) ||
        !TryAdd(reserved, context.TableBytes(), reserved)) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    RecordLimits recordLimits = budget.Record();
    recordLimits.archive.maxTotalBytes =
        reserved < budget.limits.maxBytes ? budget.limits.maxBytes - reserved : 0;
    auto counted = budget.AddFields(schema.fields.Size());
    if (!counted) {
        return counted;
    }

    context.BeginObject(schema.id, static_cast<uint32>(nodeIndex), fileBytes);
    // 차이 쓰기(A5b): 그 구체 클래스의 기준 레코드를 연다. 레코드가 없는 타입은 전부 쓴다.
    if (baseline != GraphBaseline::None) {
        ClassDefaultsDiagnostic why;
        auto defaults = AcquireClassDefaults(nodes[nodeIndex].lease, allocator, &why);
        if (defaults) {
            const ClassDefaultsConfiguration configuration =
                baseline == GraphBaseline::Editor ? ClassDefaultsConfiguration::Editor
                                                  : ClassDefaultsConfiguration::Cooked;
            if (auto opened =
                    context.BeginBaseline(schema, defaults.Value()->Record(configuration),
                                          overrides != nullptr ? overrides->Find(handle) : nullptr);
                !opened) {
                return opened;
            }
        } else if (why.reason != ClassDefaultsReason::IncompleteLayout &&
                   why.reason != ClassDefaultsReason::ManualFields) {
            return Result<void>::Failure(defaults.Error());
        }
    }

    RecordBuilder record(allocator, schema.id, schema.version, recordLimits);
    usize payloadBytes = 0;
    NodeFieldSink sink(nodes, nodeIndex, context);
    auto encoded =
        EncodeObjectFields(allocator, &registry, access, handle, budget,
                           recordLimits.archive.maxTotalBytes, context, record, sink, payloadBytes);
    context.EndBaseline();
    if (!encoded) {
        return encoded;
    }

    // 이 노드가 출력에 더하는 byte: Bytes(Record) = 8 + 44 + payload, NodeTable이면 노드 항목.
    // 외부 참조 위치 표와 weak 지연 항목 메모리도 그래프 바이트 누계에 든다(5.3절, 리플렉션 B9
    // 5.1절).
    usize recordBytes = 0;
    usize total = 0;
    usize charged = 0;
    if (!TryAdd(payloadBytes, RecordHeaderBytes, recordBytes) ||
        !TryAdd(fileBytes, RecordPrefixBytes, total) || !TryAdd(total, recordBytes, total) ||
        !TryAdd(total, entryBytes, total) || !TryAdd(total, context.TableBytes(), charged)) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    if (charged > budget.limits.maxBytes) {
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
    nodes[nodeIndex].firstPosition = firstPosition;
    fileBytes = total;
    return Result<void>::Success();
}

} // namespace

Result<ObjectGraphSaveResult> SaveObjectGraphSet(Allocator& allocator, ObjectRegistry& registry,
                                                 const ObjectAccessContext& access,
                                                 Span<const ObjectHandle> set,
                                                 ObjectGraphSaveOptions options) noexcept {
    using Output = Result<ObjectGraphSaveResult>;
    if (options.limits.maxObjects > MaxNodeCount ||
        options.limits.maxExternalReferences > MaxExternalCount) {
        return Output::Failure(ErrorCode::InvalidArgument);
    }

    if (set.Size() == 0) {
        return Output::Failure(ErrorCode::InvalidArgument);
    }

    // 차이 쓰기 기준 구성은 저장 정책과 맞아야 한다(A5b — Editor는 EditorOnly를 쓰고 Cooked는 뺀다,
    // 포함 필터 저장은 차이 형식이 아니다).
    if (options.baseline != GraphBaseline::None) {
        const bool editorOnlyExcluded =
            (options.policy.excludedFlags & static_cast<uint32>(FieldFlags::EditorOnly)) != 0;
        if (options.policy.includeFlags != 0 ||
            editorOnlyExcluded != (options.baseline == GraphBaseline::Cooked)) {
            return Output::Failure(ErrorCode::InvalidArgument);
        }
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

        // 패키지 저장(Exclude)의 집합은 저장 불가 객체를 담지 않는다(호출자가 미리 거른다 — root
        // 포함, 리플렉션 B9h2).
        if (options.unsaveable == UnsaveablePolicy::Exclude &&
            IsUnsaveableType(nodes[index].lease, options.unsaveableTypeFlags)) {
            return Output::Failure(ErrorCode::InvalidArgument);
        }
    }

    if (auto prepared = context.PrepareInclude(); !prepared) {
        return Output::Failure(prepared.Error());
    }

    // Include는 encode 중에 노드를 끝에 더하므로 매번 크기를 다시 본다(더한 노드도 이 순서로 encode
    // 된다 — 외부 참조 위치 표가 노드 순서다).
    usize bytes = HeaderBytes;
    for (usize index = 0; index < nodes.Size(); ++index) {
        auto saved =
            SaveNodeFields(allocator, registry, access, nodes, index, budget.Value(), bytes,
                           options.writeNodeTable, options.baseline, options.overrides, context);
        if (!saved) {
            return Output::Failure(saved.Error());
        }
    }

    // Include의 weak 지연 항목을 최종 집합으로 덮어쓰고 집합 밖 항목을 기록 순서대로 진단한다
    // (노드 encode 순서와 무관한 결과 — 리플렉션 B9 5.1절).
    if (auto resolved = context.ResolveWeakDeferrals(); !resolved) {
        return Output::Failure(resolved.Error());
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

    auto output = WriteGraph(allocator, nodes, context, outers, baseOuter, budget.Value());
    if (!output) {
        return Output::Failure(output.Error());
    }

    // 마지막 output 할당 callback까지 포함하여 모든 노드와 외부 참조 대상의 논리 수명을 다시
    // 확인한 뒤 게시한다(결과의 handle이 게시 시점에 모두 생존 객체다).
    auto alive = [&](ObjectHandle handle) noexcept -> Result<void> {
        auto lifecycle = registry.InspectLifecycle(access, handle);
        if (!lifecycle) {
            return Result<void>::Failure(lifecycle.Error());
        }

        if (lifecycle.Value() != ObjectLifecycle::Alive) {
            return Result<void>::Failure(ErrorCode::Destroyed);
        }

        return Result<void>::Success();
    };

    Array<ObjectHandle> handles(allocator);
    if (auto reserved = handles.TryReserve(nodes.Size()); !reserved) {
        return Output::Failure(reserved.Error());
    }

    for (usize index = 0; index < nodes.Size(); ++index) {
        if (auto live = alive(nodes[index].handle); !live) {
            return Output::Failure(live.Error());
        }

        if (!handles.TryPushBack(nodes[index].handle)) {
            return Output::Failure(ErrorCode::OutOfMemory);
        }
    }

    for (const GraphExternal& external : context.Externals()) {
        if (auto live = alive(external.target); !live) {
            return Output::Failure(live.Error());
        }
    }

    return Output::Success(
        SaveResultAccess::Make(Move(output.Value()), Move(handles), Move(context.Externals()),
                               Move(context.Positions()), options.limits.maxExternalReferences));
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
