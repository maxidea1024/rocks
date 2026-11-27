#pragma once

#include <Core/Memory/Allocator.h>
#include <Core/Containers/Array.h>
#include <Object/FieldCodec.h>
#include <Object/FieldEncodeContext.h>
#include <Object/RuntimeTypeOps.h>

// 이벤트 바인딩 기록이다(Docs/Design/EventReflection.ko.md ED19–ED22 — EV3a1). 이벤트 필드 TLV는
// 살아 있는 리플렉션 바인딩이 하나 이상일 때만 쓰고, wire는 RecordList(항목마다 고정 schema
// EventBindingRecordType의 record: target 약한 참조 + function Guid)다. 저장(EncodeObjectFields)과
// 로드 두 단계(DecodeNode·ApplyNodeFields)가 쓰고, 로드는 대상 handle과 FunctionId를 보류 목록에
// 쌓아 Commit 뒤 바인딩 단계(BindPendingEvents)가 RuntimeEventOps::bind로 다시 만든다.
namespace rocks::graph_detail {

// 바인딩 항목 record의 schema·판과 멤버 ID다(형식 상수 — 바꾸면 기존 저장 데이터를 읽지 못한다).
inline constexpr Guid EventBindingRecordType{0x4556424e44524543, 0x0000000000000001};
inline constexpr uint32 EventBindingRecordVersion = 1;
inline constexpr Guid EventBindingTargetMember{0x4556424e44524543, 0x0000000000000101};
inline constexpr Guid EventBindingFunctionMember{0x4556424e44524543, 0x0000000000000102};

// 로드가 Commit 뒤에 다시 만들 바인딩 하나다(node = 발행자 노드 위치, schemaIndex = 이벤트 필드).
struct PendingEventBinding {
    uint32 node = 0;
    TypeId publisherType{};
    usize schemaIndex = 0;
    const RuntimeEventOps* ops = nullptr;
    ObjectHandle target{};
    Guid functionId{};
};

// publisher의 이벤트 필드(ops)에서 살아 있는 리플렉션 바인딩과 미해결 레코드를 out에 기록한다
// (쿠킹 — EditorOnly 제외 정책 — 은 미해결 레코드·EditorOnly 함수 바인딩을 EventBindingNotCookable
// 진단 뒤 InvalidArgument로 거부한다). 바인딩이 없으면
// written이 false이고 아무것도 쓰지 않는다(필드를 빼야 한다). 대상은 그래프 약한 참조 정책대로 쓴다
// (집합 밖 null + ExternalReference, Include 지연 덮어쓰기). 실패: 나열(Registry 오류)·쓰기·한도
// 오류.
Result<uint32> EncodeEventBindings(const RuntimeEventOps& ops, ObjectRegistry& registry,
                                   const ObjectAccessContext& access, ObjectHandle publisher,
                                   ArchiveWriter& out, FieldEncodeContext& context,
                                   Allocator& scratch, bool& written) noexcept;

// 이벤트 필드 하나의 바인딩 기록을 읽는다. 1단계(pending = nullptr)는 형식(판 포함)·약한 참조
// token을 검사하고 원소 누계·작업량과 2단계 보류 목록 메모리를 청구하며, 2단계는 null이 아닌
// 대상마다 (node, schemaIndex, ops, 대상 handle, FunctionId)를 pending에 쌓는다(null 대상 —
// 죽은·집합 밖·Include 미해결 — 은 버린다). 형식이 다르면(wire·schema· 멤버 누락·중복·모르는 멤버)
// CorruptData다.
Result<void> DecodeEventBindings(const FieldView& field, FieldDecodeContext& context,
                                 Array<PendingEventBinding>* pending, uint32 node,
                                 TypeId publisherType, usize schemaIndex,
                                 const RuntimeEventOps* ops) noexcept;

// Commit 뒤 보류 바인딩을 만든다(nodes = 게시된 노드 handle). keepUnresolved면 함수 없음·서명
// 다름 바인딩을 미해결 레코드로 보존한다(ED24). 실패한 바인딩은 버리고(보존한 것도 진단은 낸다)
// EventBindingDropped 진단을 hooks에 낸다(hooks가 없으면 조용히 버린다). 발행자가 그 사이 살아 있지
// 않으면 진단 없이 건너뛴다. 반환: 만든 바인딩 수.
usize BindPendingEvents(ObjectRegistry& registry, const ObjectAccessContext& access,
                        Span<const ObjectHandle> nodes, Span<const PendingEventBinding> pending,
                        FieldCodecHooks* hooks, bool keepUnresolved) noexcept;

} // namespace rocks::graph_detail
