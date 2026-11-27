#pragma once

#include <Core/Math/Color.h>
#include <Core/Math/Float16.h>
#include <Core/Math/VertexPacking.h>
#include <GraphicsTypes/IndexFormat.h>
#include <GraphicsTypes/VertexFormat.h>
#include <Mesh/Common/MeshRenderSection.h>
#include <Mesh/Common/MeshSectionInfo.h>
#include <Mesh/StaticMesh/StaticMeshValidation.h>
#include <MeshTools/StaticMeshBuilder.h>
#include <MeshTools/StaticMeshPacking.h>

#include "MeshPackWriter.h"

// 정적·스킨 쿠킹 LOD가 함께 쓰는 패킹 단계다(Docs/Design/MeshAnimationAssets 5.5.2절 1·6.7.2절 7).
// LOD 타입은 같은 이름의 공통 필드(vertexCount·positions·tangentBasis·texCoords·colors·indexFormat·
// indices·sections)를 가진다.
namespace rocks::mesh_packing {

using graphics::IndexFormat;
using graphics::VertexFormat;

// 넘치지 않게 a × b를 구한다.
inline bool Multiply(uint64 a, uint64 b, uint64& out) noexcept {
    if (b != 0 && a > NumericLimits<uint64>::Max() / b) {
        return false;
    }

    out = a * b;
    return true;
}

inline Result<void> PutStream(PackWriter& writer, MeshRenderStream& stream, VertexFormat format,
                              uint64 bytes) noexcept {
    auto start = writer.Begin(bytes);
    if (!start) {
        return Result<void>::Failure(start.Error());
    }

    stream.vertexFormat = graphics::VertexFormatNumber(format);
    stream.offset = start.Value();
    stream.byteSize = bytes;
    return Result<void>::Success();
}

inline void PutTangentComponent(PackWriter& writer, float value, bool wide) noexcept {
    if (wide) {
        writer.Put<uint16>(static_cast<uint16>(math::PackSnorm16(value)));
    } else {
        writer.PutByte(static_cast<uint8>(math::PackSnorm8(value)));
    }
}

inline void PutTexCoord(PackWriter& writer, float value, bool full) noexcept {
    if (full) {
        writer.PutFloat(value);
    } else {
        writer.Put<uint16>(Float16::FromFloat(value).bits);
    }
}

// 위치 → tangent 기저 → UV 쌍 → 색을 쓴다(5.5.2절 1). lod의 vertexCount·texCoords·colors를 채운다.
template <class Lod>
Result<void> PackVertexStreams(const StaticMeshBuildResult& built,
                               const MeshBuildSettings& settings, MeshColorFormat colorFormat,
                               PackWriter& writer, Lod& lod) noexcept {
    const uint64 vertices = built.VertexCount();
    lod.vertexCount = built.VertexCount();
    lod.texCoords.Clear();

    // 위치.
    uint64 size = 0;
    if (!Multiply(vertices, graphics::VertexFormatSize(VertexFormat::Float3), size)) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    if (auto put = PutStream(writer, lod.positions, VertexFormat::Float3, size); !put) {
        return put;
    }

    for (const Vec3& position : built.positions) {
        writer.PutFloat(position.x);
        writer.PutFloat(position.y);
        writer.PutFloat(position.z);
    }

    // tangent 기저: (tangent, 1), (법선, 쌍법선 부호) — UE TangentX W = MAX_int, TangentZ W = 부호.
    const bool wide = settings.useHighPrecisionTangentBasis;
    const VertexFormat basis = wide ? VertexFormat::Short4Snorm : VertexFormat::Byte4Snorm;
    if (!Multiply(vertices, uint64{graphics::VertexFormatSize(basis)} * 2, size)) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    if (auto put = PutStream(writer, lod.tangentBasis, basis, size); !put) {
        return put;
    }

    for (uint32 vertex = 0; vertex < built.VertexCount(); ++vertex) {
        const Vec3& tangent = built.tangents[vertex];
        const Vec3& normal = built.normals[vertex];
        PutTangentComponent(writer, tangent.x, wide);
        PutTangentComponent(writer, tangent.y, wide);
        PutTangentComponent(writer, tangent.z, wide);
        PutTangentComponent(writer, 1.0f, wide);
        PutTangentComponent(writer, normal.x, wide);
        PutTangentComponent(writer, normal.y, wide);
        PutTangentComponent(writer, normal.z, wide);
        PutTangentComponent(writer, built.binormalSigns[vertex], wide);
    }

    // UV 채널 쌍.
    const bool full = settings.useFullPrecisionUVs;
    for (uint32 channel = 0; channel < built.texCoordChannels; channel += 2) {
        const bool pair = channel + 1 < built.texCoordChannels;
        const VertexFormat format = full ? (pair ? VertexFormat::Float4 : VertexFormat::Float2)
                                         : (pair ? VertexFormat::Half4 : VertexFormat::Half2);
        if (!Multiply(vertices, graphics::VertexFormatSize(format), size)) {
            return Result<void>::Failure(ErrorCode::CapacityExceeded);
        }

        MeshRenderStream stream;
        if (auto put = PutStream(writer, stream, format, size); !put) {
            return put;
        }

        const Vec2* first = built.texCoords.Data() + usize{channel} * built.VertexCount();
        const Vec2* second = pair ? first + built.VertexCount() : nullptr;
        for (uint32 vertex = 0; vertex < built.VertexCount(); ++vertex) {
            PutTexCoord(writer, first[vertex].x, full);
            PutTexCoord(writer, first[vertex].y, full);
            if (pair) {
                PutTexCoord(writer, second[vertex].x, full);
                PutTexCoord(writer, second[vertex].y, full);
            }
        }

        if (auto pushed = lod.texCoords.TryPushBack(stream); !pushed) {
            return pushed;
        }
    }

    // 색(8비트 sRGB — 모두 흰색이면 넣지 않는다).
    lod.colors = MeshRenderStream{};
    bool white = true;
    for (const Vec4& color : built.colors) {
        const Color encoded = LinearColor(color.x, color.y, color.z, color.w).ToSRGB();
        if (encoded.r != 255 || encoded.g != 255 || encoded.b != 255 || encoded.a != 255) {
            white = false;
            break;
        }
    }

    if (!built.colors.IsEmpty() && !white) {
        const VertexFormat format = colorFormat == MeshColorFormat::Bgra8Unorm
                                        ? VertexFormat::Bgra8Unorm
                                        : VertexFormat::UByte4Unorm;
        if (auto put = PutStream(writer, lod.colors, format,
                                 vertices * graphics::VertexFormatSize(format));
            !put) {
            return put;
        }

        for (const Vec4& color : built.colors) {
            const Color encoded = LinearColor(color.x, color.y, color.z, color.w).ToSRGB();
            if (colorFormat == MeshColorFormat::Bgra8Unorm) {
                writer.PutByte(encoded.b);
                writer.PutByte(encoded.g);
                writer.PutByte(encoded.r);
            } else {
                writer.PutByte(encoded.r);
                writer.PutByte(encoded.g);
                writer.PutByte(encoded.b);
            }

            writer.PutByte(encoded.a);
        }
    }

    return Result<void>::Success();
}

// index와 섹션을 쓴다(materialIndex는 빌드 결과, 나머지 플래그는 유효 섹션 정보 — 5.5.2절 1).
template <class Lod>
Result<void> PackIndicesAndSections(const StaticMeshBuildResult& built,
                                    Span<const MeshSectionInfo> sections, PackWriter& writer,
                                    Lod& lod) noexcept {
    lod.sections.Clear();
    uint64 size = 0;
    const bool narrow = built.VertexCount() <= MeshIndex16MaxVertices;
    const IndexFormat indexFormat = narrow ? IndexFormat::UInt16 : IndexFormat::UInt32;
    lod.indexFormat = graphics::IndexFormatNumber(indexFormat);
    if (!Multiply(built.indices.Size(), graphics::IndexFormatSize(indexFormat), size)) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    auto start = writer.Begin(size);
    if (!start) {
        return Result<void>::Failure(start.Error());
    }

    lod.indices = MeshRenderStream{MeshRenderStreamNone, start.Value(), size};
    for (const uint32 index : built.indices) {
        if (narrow) {
            writer.Put<uint16>(static_cast<uint16>(index));
        } else {
            writer.Put<uint32>(index);
        }
    }

    if (auto reserved = lod.sections.TryReserve(built.sections.Size()); !reserved) {
        return reserved;
    }

    for (usize index = 0; index < built.sections.Size(); ++index) {
        const MeshBuildSection& source = built.sections[index];
        const MeshSectionInfo info = index < sections.Size() ? sections[index] : MeshSectionInfo{};
        MeshRenderSection section;
        section.materialIndex = source.materialIndex;
        section.firstIndex = source.firstIndex;
        section.triangleCount = source.triangleCount;
        section.minVertex = source.minVertex;
        section.maxVertex = source.maxVertex;
        section.castShadow = source.castShadow;
        section.enableCollision = info.enableCollision;
        section.visibleInRayTracing = info.visibleInRayTracing;
        section.affectDistanceFieldLighting = info.affectDistanceFieldLighting;
        section.forceOpaque = info.forceOpaque;
        (void)lod.sections.TryPushBack(section);
    }

    return Result<void>::Success();
}

} // namespace rocks::mesh_packing
