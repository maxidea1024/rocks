#include <Core/Encoding/Sha256.h>
#include "MeshSourceInternal.h"

#include <Base/Endian.h>
#include <Core/Assert.h>
#include <MeshSource/MeshSourceValidation.h>
#include <Object/NameField.h>
#include <Platform/Memory.h>

// 원본 메시의 저장(Store)과 읽기(Load)다(Docs/Design/MeshAnimationAssets 3.4·3.7·9.1·9.2절).
// 모든 배치는 little-endian이고 정렬 패딩이 없다.
namespace rocks {

// 원본 bulk를 읽는 곳이다(로드한 패키지 또는 쿠킹·저장의 bulk 원본 — 둘 중 하나).
struct MeshSourceBulkReader {
    const Package* package = nullptr;
    PackageBulkSource* source = nullptr;

    Result<Span<const uint8>> Read(const BulkDataRef& ref) const noexcept {
        if (package != nullptr) {
            return package->BulkBytes(ref);
        }

        // bulk 원본은 크기·SHA-256을 보지 않으므로 여기서 ref와 대조한다(빈 ref는 BulkBytes와 같은
        // 오류, 다르면 BulkBytes처럼 TypeMismatch — 쿠킹은 원본 SHA로 너겟 키를 만든다).
        if (ref.IsEmpty()) {
            return Result<Span<const uint8>>::Failure(ErrorCode::InvalidArgument);
        }

        auto bytes = source->ReadBulk(ref);
        if (bytes &&
            (bytes.Value().Size() != ref.rawBytes || Sha256::Hash(bytes.Value()) != ref.sha256)) {
            return Result<Span<const uint8>>::Failure(ErrorCode::TypeMismatch);
        }

        return bytes;
    }
};

namespace {

// 저장 배치의 원소 크기다(byte).
constexpr uint64 StorageWordBytes = sizeof(uint32);
constexpr uint64 StorageInfluenceBytes = 2 * sizeof(uint16);
constexpr uint64 StorageVec3Bytes = 3 * sizeof(float);

// 저장할 레코드 하나다(위치는 attribute = MeshNoAttribute).
struct StorageRecord {
    MeshElement element = MeshElement::Invalid;
    StringView name{};
    uint32 attribute = MeshNoAttribute;
};

// 9.2절 순서: element 값, 그다음 이름의 UTF-8 byte 사전순(부호 없는 byte, 짧은 쪽이 앞).
int CompareStorageOrder(MeshElement leftElement, StringView left, MeshElement rightElement,
                        StringView right) noexcept {
    if (leftElement != rightElement) {
        return static_cast<uint8>(leftElement) < static_cast<uint8>(rightElement) ? -1 : 1;
    }

    // 레코드 이름은 None이 아니라 비어 있지 않다(Data()가 null이 아니다).
    const usize common =
        left.ByteCount() < right.ByteCount() ? left.ByteCount() : right.ByteCount();
    const int compared = platform::MemoryCompare(left.Data(), right.Data(), common);
    if (compared != 0) {
        return compared;
    }

    if (left.ByteCount() == right.ByteCount()) {
        return 0;
    }

    return left.ByteCount() < right.ByteCount() ? -1 : 1;
}

// 쓰기 커서다(크기를 미리 잡은 배열에 차례로 쓴다).
struct StorageWriter {
    uint8* at = nullptr;

    void U16(uint16 value) noexcept {
        StoreLittleEndian<uint16>(at, value);
        at += sizeof(uint16);
    }

    void U32(uint32 value) noexcept {
        StoreLittleEndian<uint32>(at, value);
        at += sizeof(uint32);
    }

    void F32(float value) noexcept {
        U32(platform::BitCast<uint32>(value));
    }

    void Bytes(const void* data, usize count) noexcept {
        if (count != 0) {
            platform::MemoryCopy(at, data, count);
            at += count;
        }
    }
};

// 읽기 커서다(남은 byte를 먼저 확인한 뒤 읽는다).
struct StorageReader {
    const uint8* at = nullptr;
    const uint8* end = nullptr;

    uint64 Remaining() const noexcept {
        return static_cast<uint64>(end - at);
    }

    uint16 U16() noexcept {
        const uint16 value = LoadLittleEndian<uint16>(at);
        at += sizeof(uint16);
        return value;
    }

    uint32 U32() noexcept {
        const uint32 value = LoadLittleEndian<uint32>(at);
        at += sizeof(uint32);
        return value;
    }

    float F32() noexcept {
        return platform::BitCast<float>(U32());
    }
};

// bytes byte짜리 배열이다(64비트 대상만 빌드하므로 uint64 크기가 usize에 들어간다).
Result<Array<uint8>> SizedBytes(Allocator& allocator, uint64 bytes) noexcept {
    static_assert(sizeof(usize) == sizeof(uint64), "MeshSource storage assumes 64-bit sizes");
    Array<uint8> out(allocator);
    if (auto resized = out.TryResize(static_cast<usize>(bytes)); !resized) {
        return Result<Array<uint8>>::Failure(resized.Error());
    }

    return Result<Array<uint8>>::Success(Move(out));
}

// 위상 blob(9.1절)을 만든다.
Result<Array<uint8>> EncodeTopology(const MeshSource& mesh) noexcept {
    const uint64 words = static_cast<uint64>(mesh.CornerVertices().Size()) +
                         2 * static_cast<uint64>(mesh.Edges().Size()) +
                         (static_cast<uint64>(mesh.PolygonCornerEnds().Size()) + 1) +
                         mesh.PolygonCornerList().Size() + mesh.PolygonGroups().Size() +
                         3 * static_cast<uint64>(mesh.TriangleCorners().Size()) +
                         mesh.TrianglePolygons().Size();
    auto out = SizedBytes(mesh.GetAllocator(), words * StorageWordBytes);
    if (!out) {
        return out;
    }

    StorageWriter writer{out.Value().Data()};
    for (uint32 vertex : mesh.CornerVertices()) {
        writer.U32(vertex);
    }

    for (const MeshEdgeVertices& edge : mesh.Edges()) {
        writer.U32(edge.first);
        writer.U32(edge.second);
    }

    writer.U32(0);
    for (uint32 end : mesh.PolygonCornerEnds()) {
        writer.U32(end);
    }

    for (uint32 corner : mesh.PolygonCornerList()) {
        writer.U32(corner);
    }

    for (uint32 group : mesh.PolygonGroups()) {
        writer.U32(group);
    }

    for (const MeshTriangleCorners& triangle : mesh.TriangleCorners()) {
        writer.U32(triangle.corners[0]);
        writer.U32(triangle.corners[1]);
        writer.U32(triangle.corners[2]);
    }

    for (uint32 polygon : mesh.TrianglePolygons()) {
        writer.U32(polygon);
    }

    return out;
}

// 위치 레코드의 값(Vec3 × 정점 수)이다.
Result<Array<uint8>> EncodePositions(const MeshSource& mesh) noexcept {
    auto out = SizedBytes(mesh.GetAllocator(), mesh.Positions().Size() * StorageVec3Bytes);
    if (!out) {
        return out;
    }

    StorageWriter writer{out.Value().Data()};
    for (const Vec3& position : mesh.Positions()) {
        writer.F32(position.x);
        writer.F32(position.y);
        writer.F32(position.z);
    }

    return out;
}

// 속성 하나의 값 bulk(3.4.1절)를 만든다. 가변 길이 값은 범위 순서대로 압축해 쓴다.
Result<Array<uint8>> EncodeAttribute(Allocator& allocator,
                                     const MeshSource::Attribute& attribute) noexcept {
    uint64 bytes = 0;
    for (const MeshSource::Channel& channel : attribute.channels) {
        switch (attribute.type) {
        case MeshValueType::UInt32:
            bytes += channel.words.Size() * StorageWordBytes;
            break;
        case MeshValueType::Bool:
            bytes += channel.bytes.Size();
            break;
        case MeshValueType::Name:
            for (const MeshSource::ValueRange& range : channel.ranges) {
                bytes += StorageWordBytes + range.count;
            }

            break;
        case MeshValueType::BoneInfluences:
            bytes += (channel.ranges.Size() + 1) * StorageWordBytes;
            for (const MeshSource::ValueRange& range : channel.ranges) {
                bytes += range.count * StorageInfluenceBytes;
            }

            break;
        case MeshValueType::SparseVec3:
            bytes +=
                StorageWordBytes + channel.sparse.Size() * (StorageWordBytes + StorageVec3Bytes);
            break;
        default:
            // Float·Vec2·Vec3·Vec4(저장된 속성의 값 타입은 늘 유효하다).
            bytes += channel.floats.Size() * StorageWordBytes;
            break;
        }
    }

    auto out = SizedBytes(allocator, bytes);
    if (!out) {
        return out;
    }

    StorageWriter writer{out.Value().Data()};
    for (const MeshSource::Channel& channel : attribute.channels) {
        switch (attribute.type) {
        case MeshValueType::UInt32:
            for (uint32 value : channel.words) {
                writer.U32(value);
            }

            break;
        case MeshValueType::Bool:
            writer.Bytes(channel.bytes.Data(), channel.bytes.Size());
            break;
        case MeshValueType::Name:
            for (const MeshSource::ValueRange& range : channel.ranges) {
                writer.U32(range.count);
                writer.Bytes(channel.bytes.Data() + range.offset, range.count);
            }

            break;
        case MeshValueType::BoneInfluences: {
            uint32 start = 0;
            writer.U32(start);
            for (const MeshSource::ValueRange& range : channel.ranges) {
                start += range.count;
                writer.U32(start);
            }

            for (const MeshSource::ValueRange& range : channel.ranges) {
                for (uint32 index = 0; index < range.count; ++index) {
                    const BoneInfluence& influence = channel.influences[range.offset + index];
                    writer.U16(influence.boneIndex);
                    writer.U16(influence.weight);
                }
            }

            break;
        }
        case MeshValueType::SparseVec3:
            writer.U32(static_cast<uint32>(channel.sparse.Size()));
            for (const MeshSparseVec3& entry : channel.sparse) {
                writer.U32(entry.index);
            }

            for (const MeshSparseVec3& entry : channel.sparse) {
                writer.F32(entry.value.x);
                writer.F32(entry.value.y);
                writer.F32(entry.value.z);
            }

            break;
        default:
            for (float value : channel.floats) {
                writer.F32(value);
            }

            break;
        }
    }

    return out;
}

// 로드 배치 위반이다(CorruptPackage / CorruptData).
Result<void> CorruptLayout(MeshDiagnostic* failure, MeshIssue issue,
                           MeshElement element = MeshElement::Invalid,
                           uint32 attribute = MeshNoAttribute,
                           uint32 index = MeshNoIndex) noexcept {
    return rocks::mesh_source_detail::Fail(failure, AssetDiagnosticCode::CorruptPackage,
                                           ErrorCode::CorruptData, issue, element, attribute,
                                           index);
}

// ref의 bulk를 읽는다. 못 읽으면 CorruptPackage(BulkUnreadable)와 읽기 오류다.
Result<Span<const uint8>> ReadStorageBulk(const MeshSourceBulkReader& reader,
                                          const BulkDataRef& ref, MeshDiagnostic* failure,
                                          MeshElement element, uint32 attribute) noexcept {
    auto bytes = reader.Read(ref);
    if (!bytes) {
        (void)rocks::mesh_source_detail::Fail(failure, AssetDiagnosticCode::CorruptPackage,
                                              bytes.Error(), MeshIssue::BulkUnreadable, element,
                                              attribute);
    }

    return bytes;
}

// 위상 blob을 mesh에 읽어 넣는다(요소 수는 이미 들어 있다).
Result<void> DecodeTopology(Span<const uint8> bytes, MeshSource& mesh,
                            MeshDiagnostic* failure) noexcept {
    const uint64 corners = mesh.Count(MeshElement::Corner);
    const uint64 edges = mesh.Count(MeshElement::Edge);
    const uint64 polygons = mesh.Count(MeshElement::Polygon);
    const uint64 triangles = mesh.Count(MeshElement::Triangle);
    const uint64 prefixWords = corners + 2 * edges + polygons + 1;
    if (bytes.Size() < prefixWords * StorageWordBytes) {
        return CorruptLayout(failure, MeshIssue::TopologyLength);
    }

    // 둘레 시작 표를 먼저 읽어 둘레 코너 전체 수를 안다.
    StorageReader starts{bytes.Data() + (corners + 2 * edges) * StorageWordBytes,
                         bytes.Data() + bytes.Size()};
    if (starts.U32() != 0) {
        return CorruptLayout(failure, MeshIssue::PolygonStarts, MeshElement::Polygon,
                             MeshNoAttribute, 0);
    }

    uint32 previous = 0;
    for (uint64 polygon = 0; polygon < polygons; ++polygon) {
        const uint32 end = starts.U32();
        if (end < previous || end > MeshMaxElements) {
            return CorruptLayout(failure, MeshIssue::PolygonStarts, MeshElement::Polygon,
                                 MeshNoAttribute, static_cast<uint32>(polygon));
        }

        previous = end;
    }

    const uint64 perimeter = previous;
    const uint64 totalWords = prefixWords + perimeter + polygons + 4 * triangles;
    if (bytes.Size() != totalWords * StorageWordBytes) {
        return CorruptLayout(failure, MeshIssue::TopologyLength);
    }

    Array<uint32>& cornerVertices = MeshSourceAccess::CornerVertices(mesh);
    Array<MeshEdgeVertices>& edgeList = MeshSourceAccess::Edges(mesh);
    Array<uint32>& polygonEnds = MeshSourceAccess::PolygonEnds(mesh);
    Array<uint32>& polygonCorners = MeshSourceAccess::PolygonCorners(mesh);
    Array<uint32>& polygonGroups = MeshSourceAccess::PolygonGroups(mesh);
    Array<MeshTriangleCorners>& triangleCorners = MeshSourceAccess::TriangleCorners(mesh);
    Array<uint32>& trianglePolygons = MeshSourceAccess::TrianglePolygons(mesh);
    if (auto resized = cornerVertices.TryResize(static_cast<usize>(corners)); !resized) {
        return resized;
    }

    if (auto resized = edgeList.TryResize(static_cast<usize>(edges)); !resized) {
        return resized;
    }

    if (auto resized = polygonEnds.TryResize(static_cast<usize>(polygons)); !resized) {
        return resized;
    }

    if (auto resized = polygonCorners.TryResize(static_cast<usize>(perimeter)); !resized) {
        return resized;
    }

    if (auto resized = polygonGroups.TryResize(static_cast<usize>(polygons)); !resized) {
        return resized;
    }

    if (auto resized = triangleCorners.TryResize(static_cast<usize>(triangles)); !resized) {
        return resized;
    }

    if (auto resized = trianglePolygons.TryResize(static_cast<usize>(triangles)); !resized) {
        return resized;
    }

    StorageReader reader{bytes.Data(), bytes.Data() + bytes.Size()};
    for (uint32& vertex : cornerVertices) {
        vertex = reader.U32();
    }

    for (MeshEdgeVertices& edge : edgeList) {
        edge.first = reader.U32();
        edge.second = reader.U32();
    }

    (void)reader.U32();
    for (uint32& end : polygonEnds) {
        end = reader.U32();
    }

    for (uint32& corner : polygonCorners) {
        corner = reader.U32();
    }

    for (uint32& group : polygonGroups) {
        group = reader.U32();
    }

    for (MeshTriangleCorners& triangle : triangleCorners) {
        triangle.corners[0] = reader.U32();
        triangle.corners[1] = reader.U32();
        triangle.corners[2] = reader.U32();
    }

    for (uint32& polygon : trianglePolygons) {
        polygon = reader.U32();
    }

    return Result<void>::Success();
}

// 위치 레코드의 값을 읽는다.
Result<void> DecodePositions(Span<const uint8> bytes, MeshSource& mesh, MeshDiagnostic* failure,
                             uint32 record) noexcept {
    const uint64 vertices = mesh.Count(MeshElement::Vertex);
    if (bytes.Size() != vertices * StorageVec3Bytes) {
        return CorruptLayout(failure, MeshIssue::AttributeLength, MeshElement::Vertex, record);
    }

    Array<Vec3>& positions = MeshSourceAccess::Positions(mesh);
    if (auto resized = positions.TryResize(static_cast<usize>(vertices)); !resized) {
        return resized;
    }

    StorageReader reader{bytes.Data(), bytes.Data() + bytes.Size()};
    for (Vec3& position : positions) {
        position.x = reader.F32();
        position.y = reader.F32();
        position.z = reader.F32();
    }

    return Result<void>::Success();
}

// 고정 크기 값(Float·Vec*·UInt32·Bool)을 읽는다.
Result<void> DecodeFixedValues(Span<const uint8> bytes, MeshSource::Attribute& attribute,
                               uint32 count, MeshDiagnostic* failure, uint32 record) noexcept {
    const uint64 components = rocks::mesh_source_detail::FloatComponents(attribute.type);
    uint64 elementBytes = 1;
    if (components != 0) {
        elementBytes = components * StorageWordBytes;
    } else if (attribute.type == MeshValueType::UInt32) {
        elementBytes = StorageWordBytes;
    }

    const uint64 channelBytes = elementBytes * count;
    if (bytes.Size() != channelBytes * attribute.channels.Size()) {
        return CorruptLayout(failure, MeshIssue::AttributeLength, attribute.element, record);
    }

    StorageReader reader{bytes.Data(), bytes.Data() + bytes.Size()};
    for (MeshSource::Channel& channel : attribute.channels) {
        if (components != 0) {
            if (auto resized = channel.floats.TryResize(static_cast<usize>(count * components));
                !resized) {
                return resized;
            }

            for (float& value : channel.floats) {
                value = reader.F32();
            }
        } else if (attribute.type == MeshValueType::UInt32) {
            if (auto resized = channel.words.TryResize(count); !resized) {
                return resized;
            }

            for (uint32& value : channel.words) {
                value = reader.U32();
            }
        } else {
            if (auto appended = channel.bytes.TryAppend(Span<const uint8>(reader.at, count));
                !appended) {
                return appended;
            }

            reader.at += count;
        }
    }

    return Result<void>::Success();
}

// Name 값(uint32 길이 + UTF-8, N × C개)을 읽는다. 내용 규칙은 검증이 본다. 값마다 길이 word가
// 있으므로 bulk가 N × C × 4 byte보다 짧으면 범위 표를 잡기 전에 손상으로 본다(변조된 요소 수가
// bulk와 무관한 큰 할당을 만들지 못하게 — 3.7절 4).
Result<void> DecodeNameValues(Span<const uint8> bytes, MeshSource::Attribute& attribute,
                              uint32 count, MeshDiagnostic* failure, uint32 record) noexcept {
    const uint64 minimumBytes =
        static_cast<uint64>(count) * attribute.channels.Size() * StorageWordBytes;
    if (bytes.Size() < minimumBytes) {
        return CorruptLayout(failure, MeshIssue::AttributeLength, attribute.element, record);
    }

    StorageReader reader{bytes.Data(), bytes.Data() + bytes.Size()};
    for (MeshSource::Channel& channel : attribute.channels) {
        if (auto resized = channel.ranges.TryResize(count); !resized) {
            return resized;
        }

        for (uint32 index = 0; index < count; ++index) {
            if (reader.Remaining() < StorageWordBytes) {
                return CorruptLayout(failure, MeshIssue::AttributeLength, attribute.element, record,
                                     index);
            }

            const uint32 length = reader.U32();
            if (length > MaxNameFieldBytes) {
                return CorruptLayout(failure, MeshIssue::NameValueLength, attribute.element, record,
                                     index);
            }

            if (reader.Remaining() < length) {
                return CorruptLayout(failure, MeshIssue::AttributeLength, attribute.element, record,
                                     index);
            }

            // 저장소 offset은 uint32다(bulk 하나가 4 GiB를 넘는 Name 값은 담지 않는다).
            if (length > NumericLimits<uint32>::Max() - channel.bytes.Size()) {
                return Result<void>::Failure(ErrorCode::CapacityExceeded);
            }

            MeshSource::ValueRange& range = channel.ranges[index];
            range.offset = static_cast<uint32>(channel.bytes.Size());
            range.count = length;
            if (auto appended = channel.bytes.TryAppend(Span<const uint8>(reader.at, length));
                !appended) {
                return appended;
            }

            reader.at += length;
        }
    }

    if (reader.Remaining() != 0) {
        return CorruptLayout(failure, MeshIssue::AttributeLength, attribute.element, record);
    }

    return Result<void>::Success();
}

// BoneInfluences 값(시작 표 N + 1 + 영향)을 읽는다.
Result<void> DecodeInfluences(Span<const uint8> bytes, MeshSource::Attribute& attribute,
                              uint32 count, MeshDiagnostic* failure, uint32 record) noexcept {
    MeshSource::Channel& channel = attribute.channels[0];
    const uint64 tableBytes = (static_cast<uint64>(count) + 1) * StorageWordBytes;
    if (bytes.Size() < tableBytes) {
        return CorruptLayout(failure, MeshIssue::AttributeLength, attribute.element, record);
    }

    if (auto resized = channel.ranges.TryResize(count); !resized) {
        return resized;
    }

    StorageReader reader{bytes.Data(), bytes.Data() + bytes.Size()};
    if (reader.U32() != 0) {
        return CorruptLayout(failure, MeshIssue::InfluenceStarts, attribute.element, record, 0);
    }

    uint32 previous = 0;
    for (uint32 index = 0; index < count; ++index) {
        const uint32 next = reader.U32();
        if (next < previous || next - previous > MeshMaxSourceInfluences) {
            return CorruptLayout(failure, MeshIssue::InfluenceStarts, attribute.element, record,
                                 index);
        }

        channel.ranges[index] = MeshSource::ValueRange{previous, next - previous};
        previous = next;
    }

    if (bytes.Size() != tableBytes + previous * StorageInfluenceBytes) {
        return CorruptLayout(failure, MeshIssue::AttributeLength, attribute.element, record);
    }

    if (auto resized = channel.influences.TryResize(previous); !resized) {
        return resized;
    }

    for (BoneInfluence& influence : channel.influences) {
        influence.boneIndex = reader.U16();
        influence.weight = reader.U16();
    }

    return Result<void>::Success();
}

// SparseVec3 값(K + 번호 K + Vec3 K)을 읽는다.
Result<void> DecodeSparse(Span<const uint8> bytes, MeshSource::Attribute& attribute, uint32 count,
                          MeshDiagnostic* failure, uint32 record) noexcept {
    MeshSource::Channel& channel = attribute.channels[0];
    if (bytes.Size() < StorageWordBytes) {
        return CorruptLayout(failure, MeshIssue::AttributeLength, attribute.element, record);
    }

    StorageReader indices{bytes.Data(), bytes.Data() + bytes.Size()};
    const uint64 entries = indices.U32();
    if (bytes.Size() != StorageWordBytes + entries * (StorageWordBytes + StorageVec3Bytes)) {
        return CorruptLayout(failure, MeshIssue::AttributeLength, attribute.element, record);
    }

    if (auto resized = channel.sparse.TryResize(static_cast<usize>(entries)); !resized) {
        return resized;
    }

    StorageReader values{bytes.Data() + StorageWordBytes + entries * StorageWordBytes,
                         bytes.Data() + bytes.Size()};
    for (uint64 entry = 0; entry < entries; ++entry) {
        MeshSparseVec3& item = channel.sparse[static_cast<usize>(entry)];
        item.index = indices.U32();
        item.value.x = values.F32();
        item.value.y = values.F32();
        item.value.z = values.F32();
        if (item.index >= count) {
            return CorruptLayout(failure, MeshIssue::SparseIndexRange, attribute.element, record,
                                 item.index);
        }

        if (entry != 0 && item.index <= channel.sparse[static_cast<usize>(entry - 1)].index) {
            return CorruptLayout(failure, MeshIssue::SparseIndexOrder, attribute.element, record,
                                 item.index);
        }

        if (rocks::mesh_source_detail::IsPositiveZero(item.value)) {
            return CorruptLayout(failure, MeshIssue::SparseZeroEntry, attribute.element, record,
                                 item.index);
        }
    }

    return Result<void>::Success();
}

// 레코드의 값 bulk를 attribute에 읽는다.
Result<void> DecodeAttributeValues(Span<const uint8> bytes, MeshSource::Attribute& attribute,
                                   uint32 count, MeshDiagnostic* failure, uint32 record) noexcept {
    switch (attribute.type) {
    case MeshValueType::Name:
        return DecodeNameValues(bytes, attribute, count, failure, record);
    case MeshValueType::BoneInfluences:
        return DecodeInfluences(bytes, attribute, count, failure, record);
    case MeshValueType::SparseVec3:
        return DecodeSparse(bytes, attribute, count, failure, record);
    default:
        return DecodeFixedValues(bytes, attribute, count, failure, record);
    }
}

} // namespace

uint32& MeshSourceAccess::CountRef(MeshSource& mesh, MeshElement element) noexcept {
    switch (element) {
    case MeshElement::Vertex:
        return mesh.vertexCount_;
    case MeshElement::Corner:
        return mesh.cornerCount_;
    case MeshElement::Edge:
        return mesh.edgeCount_;
    case MeshElement::Polygon:
        return mesh.polygonCount_;
    case MeshElement::Triangle:
        return mesh.triangleCount_;
    default:
        ROCKS_ASSERT(element == MeshElement::PolygonGroup);
        return mesh.polygonGroupCount_;
    }
}

Result<void> MeshSource::Store(MemoryBulkSource& bulk, NamePool& names, MeshSourceData& data,
                               MeshDiagnostic* failure) const noexcept {
    if (auto valid = ValidateMeshSource(*this, MeshValidateOptions{}, failure); !valid) {
        return valid;
    }

    // 9.2절 순서로 레코드를 줄 세운다(위치 레코드 포함).
    Array<StorageRecord> order(*allocator_);
    if (auto reserved = order.TryReserve(attributes_.Size() + 1); !reserved) {
        return reserved;
    }

    ROCKS_ASSERT(order
                     .TryPushBack(StorageRecord{MeshElement::Vertex, MeshAttributeName::Position,
                                                MeshNoAttribute})
                     .IsOk());
    for (usize index = 0; index < attributes_.Size(); ++index) {
        ROCKS_ASSERT(order
                         .TryPushBack(StorageRecord{attributes_[index].element,
                                                    attributes_[index].name.View(),
                                                    static_cast<uint32>(index)})
                         .IsOk());
    }

    order.Sort([](const StorageRecord& left, const StorageRecord& right) noexcept {
        return CompareStorageOrder(left.element, left.name, right.element, right.name) < 0;
    });

    Array<MeshAttributeRecord> records(data.attributes.GetAllocator());
    if (auto reserved = records.TryReserve(order.Size()); !reserved) {
        return reserved;
    }

    auto topology = EncodeTopology(*this);
    if (!topology) {
        return Result<void>::Failure(topology.Error());
    }

    auto topologyRef = bulk.Add(Move(topology.Value()));
    if (!topologyRef) {
        return Result<void>::Failure(topologyRef.Error());
    }

    for (const StorageRecord& entry : order) {
        MeshAttributeRecord record;
        auto token = names.TryIntern(entry.name);
        if (!token) {
            return Result<void>::Failure(token.Error());
        }

        record.name = token.Value();
        record.element = entry.element;
        const bool position = entry.attribute == MeshNoAttribute;
        if (position) {
            record.type = MeshValueType::Vec3;
            record.channels = 1;
        } else {
            record.type = attributes_[entry.attribute].type;
            record.channels = static_cast<uint8>(attributes_[entry.attribute].channels.Size());
        }

        auto values = position ? EncodePositions(*this)
                               : EncodeAttribute(*allocator_, attributes_[entry.attribute]);
        if (!values) {
            return Result<void>::Failure(values.Error());
        }

        auto ref = bulk.Add(Move(values.Value()));
        if (!ref) {
            return Result<void>::Failure(ref.Error());
        }

        record.data = ref.Value();
        ROCKS_ASSERT(records.TryPushBack(record).IsOk());
    }

    data.formatVersion = MeshSourceFormatVersion;
    data.vertexCount = vertexCount_;
    data.cornerCount = cornerCount_;
    data.edgeCount = edgeCount_;
    data.polygonCount = polygonCount_;
    data.triangleCount = triangleCount_;
    data.polygonGroupCount = polygonGroupCount_;
    data.topology = topologyRef.Value();
    data.attributes = Move(records);
    return Result<void>::Success();
}

Result<void> MeshSource::Load(const MeshSourceData& data, const Package& package, NamePool& names,
                              MeshDiagnostic* failure) noexcept {
    MeshSourceBulkReader reader;
    reader.package = &package;
    return LoadFrom(data, reader, names, failure);
}

Result<void> MeshSource::Load(const MeshSourceData& data, PackageBulkSource& bulk, NamePool& names,
                              MeshDiagnostic* failure) noexcept {
    MeshSourceBulkReader reader;
    reader.source = &bulk;
    return LoadFrom(data, reader, names, failure);
}

Result<void> MeshSource::LoadFrom(const MeshSourceData& data, const MeshSourceBulkReader& reader,
                                  NamePool& names, MeshDiagnostic* failure) noexcept {
    if (data.formatVersion != MeshSourceFormatVersion) {
        return rocks::mesh_source_detail::Fail(failure, AssetDiagnosticCode::UnsupportedVersion,
                                               ErrorCode::Unsupported, MeshIssue::FormatVersion);
    }

    const uint32 counts[MeshElementKindCount] = {data.vertexCount,   data.cornerCount,
                                                 data.edgeCount,     data.polygonCount,
                                                 data.triangleCount, data.polygonGroupCount};
    MeshSource loaded(*allocator_);
    for (uint32 kind = 0; kind < MeshElementKindCount; ++kind) {
        const auto element = static_cast<MeshElement>(kind + 1);
        const uint32 limit =
            element == MeshElement::PolygonGroup ? MeshMaxPolygonGroups : MeshMaxElements;
        if (counts[kind] > limit) {
            return CorruptLayout(failure, MeshIssue::ElementCountTooLarge, element);
        }

        MeshSourceAccess::CountRef(loaded, element) = counts[kind];
    }

    auto topology =
        ReadStorageBulk(reader, data.topology, failure, MeshElement::Invalid, MeshNoAttribute);
    if (!topology) {
        return Result<void>::Failure(topology.Error());
    }

    if (auto decoded = DecodeTopology(topology.Value(), loaded, failure); !decoded) {
        return decoded;
    }

    Array<Attribute>& attributes = MeshSourceAccess::Attributes(loaded);
    if (auto reserved = attributes.TryReserve(data.attributes.Size()); !reserved) {
        return reserved;
    }

    bool positionFound = false;
    MeshElement previousElement = MeshElement::Invalid;
    String previousName(*allocator_);
    for (usize position = 0; position < data.attributes.Size(); ++position) {
        const MeshAttributeRecord& record = data.attributes[position];
        const auto recordIndex = static_cast<uint32>(position);
        if (!rocks::mesh_source_detail::IsValidElement(record.element) ||
            !rocks::mesh_source_detail::IsValidValueType(record.type) || record.channels == 0 ||
            (rocks::mesh_source_detail::IsSingleChannelType(record.type) && record.channels != 1)) {
            return CorruptLayout(failure, MeshIssue::AttributeRecordKind, MeshElement::Invalid,
                                 recordIndex);
        }

        Attribute attribute(*allocator_);
        attribute.element = record.element;
        attribute.type = record.type;
        {
            auto text = AcquireNameFieldText(&names, record.name, *allocator_);
            if (!text) {
                if (text.Error() == ErrorCode::InvalidArgument) {
                    return CorruptLayout(failure, MeshIssue::AttributeRecordName, record.element,
                                         recordIndex);
                }

                return Result<void>::Failure(text.Error());
            }

            if (text.Value().View().IsEmpty()) {
                return CorruptLayout(failure, MeshIssue::AttributeRecordName, record.element,
                                     recordIndex);
            }

            if (auto assigned = attribute.name.TryAssign(text.Value().View()); !assigned) {
                return assigned;
            }
        }

        const StringView name = attribute.name.View();
        if (position != 0 &&
            CompareStorageOrder(previousElement, previousName.View(), record.element, name) >= 0) {
            return CorruptLayout(failure, MeshIssue::AttributeOrder, record.element, recordIndex);
        }

        previousElement = record.element;
        if (auto assigned = previousName.TryAssign(name); !assigned) {
            return assigned;
        }

        auto values = ReadStorageBulk(reader, record.data, failure, record.element, recordIndex);
        if (!values) {
            return Result<void>::Failure(values.Error());
        }

        const rocks::mesh_source_detail::EngineName engineName =
            rocks::mesh_source_detail::ClassifyAttributeName(name);
        if (!positionFound && record.element == MeshElement::Vertex &&
            engineName == rocks::mesh_source_detail::EngineName::Position) {
            if (record.type != MeshValueType::Vec3 || record.channels != 1) {
                return CorruptLayout(failure, MeshIssue::MissingPosition, record.element,
                                     recordIndex);
            }

            if (auto decoded = DecodePositions(values.Value(), loaded, failure, recordIndex);
                !decoded) {
                return decoded;
            }

            positionFound = true;
            continue;
        }

        attribute.engineColor = record.element == MeshElement::Corner &&
                                record.type == MeshValueType::Vec4 &&
                                engineName == rocks::mesh_source_detail::EngineName::Color;
        if (auto reserved = attribute.channels.TryReserve(record.channels); !reserved) {
            return reserved;
        }

        for (uint32 channel = 0; channel < record.channels; ++channel) {
            ROCKS_ASSERT(attribute.channels.TryEmplaceBack(*allocator_).IsOk());
        }

        if (auto decoded = DecodeAttributeValues(
                values.Value(), attribute, loaded.Count(record.element), failure, recordIndex);
            !decoded) {
            return decoded;
        }

        ROCKS_ASSERT(attributes.TryPushBack(Move(attribute)).IsOk());
    }

    if (!positionFound) {
        return CorruptLayout(failure, MeshIssue::MissingPosition, MeshElement::Vertex);
    }

    MeshDiagnostic content;
    if (auto valid = ValidateMeshSource(loaded, MeshValidateOptions{}, &content); !valid) {
        // 저장된 원본이 모델 규칙을 어긴 것은 손상이다(한도 초과는 그대로 — AssetSystem 19장).
        const ErrorCode error =
            valid.Error() == ErrorCode::InvalidArgument ? ErrorCode::CorruptData : valid.Error();
        if (content.code != AssetDiagnosticCode::None) {
            content.code = AssetDiagnosticCode::CorruptPackage;
            content.error = error;
        }

        if (failure != nullptr) {
            *failure = content;
        }

        return Result<void>::Failure(error);
    }

    *this = Move(loaded);
    return Result<void>::Success();
}

} // namespace rocks
