#pragma once

#include <Core/Containers/Array.h>
#include <Object/ObjectHandle.h>
#include <Serialization/Archive.h>

// graph v2 byte를 wire 명세(Docs/Design/ObjectFoundations.ko.md 5.1절)로 직접 조립·검사하는 시험
// 공용 도구다. 저장 구현의 판단을 재사용하지 않는다(상수도 명세에서 옮겨 적는다).
namespace rocks::test::graph {

inline constexpr uint8 Magic[] = {'R', 'O', 'C', 'K', 'S', 'G', 'R', 'F'};
inline constexpr uint32 FormatVersion = 2;
inline constexpr uint32 NodeTableFlag = 1;
inline constexpr uint32 BaseOuterFlag = 2;
inline constexpr uint32 ExternalBit = 0x80000000u;
// header 위치: magic 0, formatVersion 8, flags 12, nodeCount 16, externalCount 20, NodeTable 24.
inline constexpr usize VersionOffset = 8;
inline constexpr usize FlagsOffset = 12;
inline constexpr usize NodeCountOffset = 16;
inline constexpr usize ExternalCountOffset = 20;
inline constexpr usize HeaderBytes = 24;

// 노드 하나다. NodeTable을 쓰면 type·outer·name을, 아니면 record만 쓴다.
struct Node {
    TypeId type{};
    uint32 outer = 0;
    StringView name{};
    Span<const uint8> record{};
};

// header 값이다(nodeCount는 nodes 수다 — 다른 값은 PatchU32로 바꾼다).
struct Header {
    uint32 version = FormatVersion;
    uint32 flags = NodeTableFlag;
    uint32 externalCount = 0;
};

// header · [NodeTable] · Record들을 순서대로 쓴다.
inline Result<Array<uint8>> Build(Allocator& allocator, Span<const Node> nodes,
                                  Header header = {}) noexcept {
    ArchiveWriter writer(allocator, {NumericLimits<usize>::Max(), NumericLimits<usize>::Max(),
                                     NumericLimits<uint64>::Max(), NumericLimits<uint64>::Max()});
    Result<void> written = writer.WriteRaw(Magic);
    const uint32 values[] = {header.version, header.flags, static_cast<uint32>(nodes.Size()),
                             header.externalCount};
    for (uint32 value : values) {
        if (written) {
            written = writer.WriteU32(value);
        }
    }

    for (usize index = 0; written && (header.flags & NodeTableFlag) != 0 && index < nodes.Size();
         ++index) {
        written = writer.WriteGuid(nodes[index].type);
        if (written) {
            written = writer.WriteU32(nodes[index].outer);
        }

        if (written) {
            written = writer.WriteString(nodes[index].name);
        }
    }

    for (usize index = 0; written && index < nodes.Size(); ++index) {
        written = writer.WriteBytes(nodes[index].record);
    }

    if (!written) {
        return Result<Array<uint8>>::Failure(written.Error());
    }

    return Result<Array<uint8>>::Success(writer.TakeBytes());
}

// NodeTable이 있는 노드 하나짜리 그래프다(Outer 없음, 이름 없음).
inline Result<Array<uint8>> Single(Allocator& allocator, TypeId type,
                                   Span<const uint8> record) noexcept {
    const Node node{type, 0, {}, record};
    return Build(allocator, Span<const Node>(&node, 1));
}

inline bool SameBytes(Span<const uint8> left, Span<const uint8> right) noexcept {
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

inline uint32 ReadU32(Span<const uint8> bytes, usize at) noexcept {
    uint32 value = 0;
    for (usize index = 0; index < 4; ++index) {
        value |= uint32{bytes[at + index]} << (8 * index);
    }

    return value;
}

inline uint64 ReadU64(Span<const uint8> bytes, usize at) noexcept {
    uint64 value = 0;
    for (usize index = 0; index < 8; ++index) {
        value |= uint64{bytes[at + index]} << (8 * index);
    }

    return value;
}

// 저장한 그래프(BaseOuter 없음)의 node번째 노드 Record다. header의 NodeTable 표지가 있으면 노드
// 항목(type Guid 16 + outer u32 + 이름 u64 길이·byte)을 건너뛰고, 앞 노드의 Bytes(u64 길이 +
// Record)를 지나 그 노드의 Record를 돌려준다(명세 5.1절 순서).
inline Span<const uint8> NodeRecordAt(Span<const uint8> graph, usize node) noexcept {
    constexpr usize GuidBytes = 16;
    constexpr usize OuterBytes = 4;
    constexpr usize LengthBytes = 8;
    const uint32 count = ReadU32(graph, NodeCountOffset);
    usize at = HeaderBytes;
    if ((ReadU32(graph, FlagsOffset) & NodeTableFlag) != 0) {
        for (uint32 index = 0; index < count; ++index) {
            at += GuidBytes + OuterBytes;
            at += LengthBytes + static_cast<usize>(ReadU64(graph, at));
        }
    }

    for (usize index = 0; index < node; ++index) {
        at += LengthBytes + static_cast<usize>(ReadU64(graph, at));
    }

    return graph.Slice(at + LengthBytes, static_cast<usize>(ReadU64(graph, at)));
}

inline void PatchU32(Array<uint8>& bytes, usize at, uint32 value) noexcept {
    for (usize index = 0; index < 4; ++index) {
        bytes[at + index] = static_cast<uint8>(value >> (8 * index));
    }
}

} // namespace rocks::test::graph
