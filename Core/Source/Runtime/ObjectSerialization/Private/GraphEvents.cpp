#include "GraphEvents.h"

#include <Core/Delegate.h>
#include <Object/ArrayFieldCodec.h>
#include <Object/ObjectRegistry.h>
#include <Serialization/Record.h>
#include <Serialization/RecordList.h>

// 이벤트 바인딩 기록의 쓰기·읽기와 Commit 뒤 바인딩이다(Docs/Design/EventReflection.ko.md
// ED19–ED22).
namespace rocks::graph_detail {

namespace {

// 바인딩 대상 약한 참조의 기대 형식이다(대상 타입 제한 없음 — rocks::Object, 함수 서명은 바인딩
// 단계가 FunctionBinding으로 다시 본다).
ReferenceExpectation AnyObject() noexcept {
    return ReferenceExpectation{TypeId{}, NativeTypeKey<Object>()};
}

struct EventBindingEntry {
    ObjectHandle target;
    Guid functionId;
};

Result<void> CollectEventBinding(void* user, ObjectHandle target, Guid functionId) noexcept {
    return static_cast<Array<EventBindingEntry>*>(user)->TryPushBack(
        EventBindingEntry{target, functionId});
}

int64 DropReason(ErrorCode error) noexcept {
    switch (error) {
    case ErrorCode::NotFound:
        return EventBindingDroppedFunctionMissing;
    case ErrorCode::TypeMismatch:
        return EventBindingDroppedSignatureMismatch;
    case ErrorCode::StaleHandle:
    case ErrorCode::Destroyed:
        return EventBindingDroppedTargetGone;
    default:
        return EventBindingDroppedOther;
    }
}

} // namespace

Result<uint32> EncodeEventBindings(const RuntimeEventOps& ops, ObjectRegistry& registry,
                                   const ObjectAccessContext& access, ObjectHandle publisher,
                                   ArchiveWriter& out, FieldEncodeContext& context,
                                   Allocator& scratch, bool& written) noexcept {
    written = false;
    Array<EventBindingEntry> bindings(scratch);
    if (auto listed = ops.bindings(registry, access, publisher, &CollectEventBinding, &bindings);
        !listed) {
        return Result<uint32>::Failure(listed.Error());
    }

    if (bindings.Size() == 0) {
        return Result<uint32>::Success(static_cast<uint32>(WireType::RecordList));
    }

    if (auto counted = context.AddElements(bindings.Size()); !counted) {
        return Result<uint32>::Failure(counted.Error());
    }

    const usize maxBytes = context.NestedRecordLimits().archive.maxTotalBytes;
    auto list = RecordListWriter::Begin(out, static_cast<uint32>(bindings.Size()),
                                        detail::ArrayRecordListLimits(maxBytes));
    if (!list) {
        return Result<uint32>::Failure(list.Error());
    }

    for (const EventBindingEntry& binding : bindings) {
        if (auto begun = list.Value().BeginItem(); !begun) {
            return Result<uint32>::Failure(begun.Error());
        }

        auto record = RecordWriter::Begin(out, EventBindingRecordType, EventBindingRecordVersion,
                                          context.NestedRecordLimits());
        if (!record) {
            return Result<uint32>::Failure(record.Error());
        }

        // 대상은 그래프 약한 참조 정책 그대로다(Include 지연 덮어쓰기는 이 writer 안 자리를 쓴다).
        Result<void> step = record.Value().BeginField(EventBindingTargetMember);
        if (step) {
            step = context.WriteWeakReference(out, binding.target, AnyObject());
        }

        if (step) {
            step = record.Value().EndField(context.ReferenceWireType());
        }

        if (step) {
            step = record.Value().BeginField(EventBindingFunctionMember);
        }

        if (step) {
            step = out.WriteGuid(binding.functionId);
        }

        if (step) {
            step = record.Value().EndField(WireType::Guid);
        }

        if (step) {
            step = record.Value().End();
        }

        if (step) {
            step = context.AddFields(record.Value().FieldCount());
        }

        if (step) {
            step = list.Value().EndItem();
        }

        if (!step) {
            return Result<uint32>::Failure(step.Error());
        }
    }

    if (auto ended = list.Value().End(); !ended) {
        return Result<uint32>::Failure(ended.Error());
    }

    written = true;
    return Result<uint32>::Success(static_cast<uint32>(WireType::RecordList));
}

Result<void> DecodeEventBindings(const FieldView& field, FieldDecodeContext& context,
                                 Array<PendingEventBinding>* pending, uint32 node,
                                 TypeId publisherType, usize schemaIndex,
                                 const RuntimeEventOps* ops) noexcept {
    if (field.wireType != static_cast<uint32>(WireType::RecordList)) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    auto list = context.OpenRecordList(field, EventBindingRecordType);
    if (!list) {
        return Result<void>::Failure(
            list.Error() == ErrorCode::TypeMismatch ? ErrorCode::CorruptData : list.Error());
    }

    // 1단계: 원소 누계와 2단계 몫(보류 목록 메모리 — 항목마다 하나)을 청구한다(struct 원소 배열과
    // 같다 — 2단계에서는 둘 다 무동작이다).
    const usize count = list.Value().ItemCount();
    if (pending == nullptr) {
        if (auto counted = context.AddElements(count); !counted) {
            return counted;
        }

        if (auto reserved =
                detail::ReserveArrayApply(context, count, sizeof(PendingEventBinding), 0);
            !reserved) {
            return reserved;
        }
    }

    for (usize index = 0; index < count; ++index) {
        if (auto spent = context.Spend(1); !spent) {
            return spent;
        }

        auto scope = context.OpenListItem(list.Value(), index);
        if (!scope) {
            return Result<void>::Failure(
                scope.Error() == ErrorCode::TypeMismatch ? ErrorCode::CorruptData : scope.Error());
        }

        RecordReader* record = context.CurrentRecord();
        if (record == nullptr) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        // 기록 판은 정확히 같아야 한다(다른 판은 이 판이 모르는 형식이다).
        if (record->SchemaVersion() != EventBindingRecordVersion) {
            return Result<void>::Failure(ErrorCode::CorruptData);
        }

        bool hasTarget = false;
        bool hasFunction = false;
        DecodedReference target{};
        Guid functionId{};
        for (usize stored = 0; stored < record->FieldCount(); ++stored) {
            auto member = record->Next();
            if (!member) {
                return Result<void>::Failure(member.Error());
            }

            const bool isTarget = member.Value().id == EventBindingTargetMember;
            const bool isFunction = member.Value().id == EventBindingFunctionMember;
            if ((!isTarget && !isFunction) || (isTarget && hasTarget) ||
                (isFunction && hasFunction) ||
                member.Value().wireType != (isTarget ? context.ReferenceWireType()
                                                     : static_cast<uint32>(WireType::Guid))) {
                return Result<void>::Failure(ErrorCode::CorruptData);
            }

            auto reader = context.OpenValue(member.Value());
            if (!reader) {
                return Result<void>::Failure(reader.Error());
            }

            if (isTarget) {
                auto decoded = context.ReadWeakReference(reader.Value(), AnyObject());
                if (!decoded) {
                    return Result<void>::Failure(decoded.Error());
                }

                target = decoded.Value();
                hasTarget = true;
            } else {
                auto read = reader.Value().ReadGuid();
                if (!read) {
                    return Result<void>::Failure(read.Error());
                }

                functionId = read.Value();
                hasFunction = true;
            }
        }

        if (!hasTarget || !hasFunction || functionId == Guid{}) {
            return Result<void>::Failure(ErrorCode::CorruptData);
        }

        // 2단계: 죽은·집합 밖·Include 미해결 대상(null)은 버린다(UE도 로드 뒤 목록을 정리한다).
        if (pending != nullptr && !target.isNull) {
            if (auto pushed = pending->TryPushBack(PendingEventBinding{
                    node, publisherType, schemaIndex, ops, target.handle, functionId});
                !pushed) {
                return pushed;
            }
        }
    }

    return Result<void>::Success();
}

usize BindPendingEvents(ObjectRegistry& registry, const ObjectAccessContext& access,
                        Span<const ObjectHandle> nodes, Span<const PendingEventBinding> pending,
                        FieldCodecHooks* hooks) noexcept {
    usize bound = 0;
    for (const PendingEventBinding& binding : pending) {
        if (binding.node >= nodes.Size() || binding.ops == nullptr) {
            continue;
        }

        const ObjectHandle publisher = nodes[binding.node];
        auto lifecycle = registry.InspectLifecycle(access, publisher);
        if (!lifecycle || lifecycle.Value() != ObjectLifecycle::Alive) {
            continue;
        }

        auto made =
            binding.ops->bind(registry, access, publisher, binding.target, binding.functionId);
        if (made) {
            ++bound;
            continue;
        }

        // 같은 바인딩이 이미 있으면 정상 병합이다(ED22).
        if (made.Error() == ErrorCode::AlreadyExists || hooks == nullptr) {
            continue;
        }

        // 경로는 이벤트 필드 한 단계다(다른 노드 진단처럼 객체 타입·경로를 채운다).
        const PathStep step{binding.ops->fieldId};
        FieldDiagnostic diagnostic;
        diagnostic.kind = FieldDiagnosticKind::EventBindingDropped;
        diagnostic.objectType = binding.publisherType;
        diagnostic.objectIndex = binding.node;
        diagnostic.path = Span<const PathStep>(&step, 1);
        diagnostic.field = binding.ops->fieldId;
        diagnostic.valueType = binding.functionId;
        diagnostic.value = DropReason(made.Error());
        (void)hooks->Report(diagnostic);
    }

    return bound;
}

} // namespace rocks::graph_detail
