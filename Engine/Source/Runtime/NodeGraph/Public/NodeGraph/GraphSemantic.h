#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Hash/Sha256.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <NodeGraph/GraphDocument.h>
#include <NodeGraph/NodeTypeRegistry.h>

// 의미 바이트와 semantic hash다(Docs/Design/NodeGraph 8.4절). 배치
// 데이터(GraphDocumentLayout)는 받지 않는다 — 배치는 의미에 들어가지 않는다(GD8). 재경유는 접는다.
//
// 형식(v1, 정수는 little-endian 고정 폭, 바이트열·문자열은 u64 길이 + 내용 —
// Serialization/Archive.h):
//   머리: raw "RNGS", u32 SemanticBytesFormatVersion, 도메인 Name, Guid mainGraph, u32 그래프 수
//   그래프(GraphId 순): Guid id, u8 role, u32 domainRole, u32 노드 수(재경유 제외), 노드들,
//                       u32 간선 수(접은 간선), 간선들
//   노드(SemanticNodeWriter::WriteNode): Guid id, Name type, u32 typeSchemaVersion, bytes 속성,
//       u32 입력 기본값 수 + (Name pin, bytes 값)들, u8 enableState, u32 고아 핀 수 +
//       (Name key, u8 direction, u32 lastType, bytes 값)들
//   간선: Guid toNode, Name toPin, Guid fromNode, Name fromPin, u32 order
//   Name: string(base를 ASCII 소문자로 바꾼 것, None은 빈 문자열) + u32 number(Name::number)
//
// 문서 형식 버전(documentFormatVersion)은 쓰지 않는다.
namespace rocks::nodegraph {

namespace detail {

class SemanticEncoder;

} // namespace detail

// 지금 의미 바이트 형식 번호다. 형식을 바꾸면 올린다(모든 semantic hash가 바뀐다).
inline constexpr uint32 SemanticBytesFormatVersion = 1;

// 의미 바이트 전체의 최대 바이트다(넘으면 CapacityExceeded).
inline constexpr usize MaxSemanticBytes = 256u * 1024u * 1024u;

// GraphSchema::NormalizeSemanticNode가 노드 하나를 쓰는 writer다. WriteNode를 노드마다 정확히 한 번
// 부른다.
class ROCKS_NODEGRAPH_API SemanticNodeWriter {
public:
    // 노드 전체(기본 정규화)를 쓴다.
    Result<void> WriteNode(const GraphNode& node) noexcept;

    // 노드를 쓰되 속성 바이트를 properties로 바꿔 쓴다(도메인 정규화 — 예: 파라미터 기본값을 뺀
    // Record).
    //
    // 오류: node가 지금 쓰는 노드가 아니거나 이미 썼으면 InvalidArgument, 형식 쓰기
    // 오류(CapacityExceeded· 할당 실패).
    Result<void> WriteNode(const GraphNode& node, Span<const uint8> properties) noexcept;

    SemanticNodeWriter(const SemanticNodeWriter&) = delete;
    SemanticNodeWriter& operator=(const SemanticNodeWriter&) = delete;

private:
    friend class detail::SemanticEncoder;

    SemanticNodeWriter(detail::SemanticEncoder& encoder, const GraphNode& expected) noexcept
        : encoder_(encoder), expected_(expected) {
    }

    detail::SemanticEncoder& encoder_;
    const GraphNode& expected_;
    bool written_ = false;
};

// document의 의미 바이트를 out에 쓴다(out은 먼저 비운다). 도메인 schema가 등록돼 있으면 노드마다
// NormalizeSemanticNode를, 없으면 기본 정규화를 쓴다.
//
// 그래프·노드·입력 기본값·고아 핀·간선을 입력 배열 순서와 무관하게 정규 순서로 쓴다. document는
// ValidateGraphDocument를 통과한 문서여야 한다(불변식을 어긴 문서의 바이트는 정해지지 않는다 —
// 재경유 사슬 순환·입력 둘은 아래 오류로 거부한다).
//
// 오류: 재경유 사슬 순환 CycleDetected, 재경유 입력 간선이 둘 이상 InvalidArgument, schema가
// WriteNode를 부르지 않았거나 두 번 부름 InvalidArgument, schema가 무시한 WriteNode 쓰기 오류(그
// 오류), names 조회 오류, MaxSemanticBytes 초과
// CapacityExceeded, 할당 실패. 실패하면 out은 비어 있다. 스레드: 문서 소유 lane.
ROCKS_NODEGRAPH_API Result<void> WriteSemanticBytes(const GraphDocument& document,
                                                    const NodeTypeRegistry& registry,
                                                    Allocator& scratch, Array<uint8>& out) noexcept;

// 의미 바이트의 SHA-256이다(도메인 semantic hash — 머티리얼 시스템 5.5절). 오류는
// WriteSemanticBytes와 같다.
ROCKS_NODEGRAPH_API Result<Sha256Digest> ComputeSemanticHash(const GraphDocument& document,
                                                             const NodeTypeRegistry& registry,
                                                             Allocator& scratch) noexcept;

} // namespace rocks::nodegraph
