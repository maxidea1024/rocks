#include <Base/Endian.h>
#include <Base/NumericLimits.h>
#include <Core/Math/MathUtility.h>
#include <GraphicsTypes/IndexFormat.h>
#include <GraphicsTypes/VertexFormat.h>
#include <Mesh/StaticMesh/StaticMeshValidation.h>

// 정적 메시 로드 검증과 확장 bounds다(Docs/Design/MeshAnimationAssets 5.5.1절 9).
namespace rocks {
namespace {

using graphics::IndexFormat;
using graphics::IndexFormatFromNumber;
using graphics::VertexFormat;

// tangent 기저 stream의 정점당 원소 수다(tangent, 법선 — 5.3절).
constexpr uint64 TangentBasisElements = 2;

// 삼각형 하나의 index 수다.
constexpr uint64 IndicesPerTriangle = 3;

// 형식 번호를 형식으로 바꾼다(번호표 밖이면 false — enum 바탕 타입(uint8)으로 잘리기 전에 본다).
bool DecodeFormat(uint32 number, VertexFormat& out) noexcept {
    auto decoded = graphics::VertexFormatFromNumber(number);
    if (!decoded) {
        return false;
    }

    out = decoded.Value();
    return true;
}

bool RenderFiniteValue(double value) noexcept {
    return value - value == 0.0;
}

bool RenderFiniteVector(const Vec3d& value) noexcept {
    return RenderFiniteValue(value.x) && RenderFiniteValue(value.y) && RenderFiniteValue(value.z);
}

Result<void> Corrupt(MeshDiagnostic* failure, MeshIssue issue, uint32 lod,
                     uint64 count = 0) noexcept {
    if (failure != nullptr) {
        *failure = MeshDiagnostic{};
        failure->code = AssetDiagnosticCode::CorruptPackage;
        failure->error = ErrorCode::InvalidArgument;
        failure->issue = issue;
        failure->index = lod;
        failure->count = count;
    }

    return Result<void>::Failure(ErrorCode::InvalidArgument);
}

bool IsTexCoordFormat(VertexFormat format) noexcept {
    switch (format) {
    case VertexFormat::Half2:
    case VertexFormat::Half4:
    case VertexFormat::Float2:
    case VertexFormat::Float4:
        return true;
    default:
        return false;
    }
}

bool IsHalfTexCoord(VertexFormat format) noexcept {
    return format == VertexFormat::Half2 || format == VertexFormat::Half4;
}

bool IsPairTexCoord(VertexFormat format) noexcept {
    return format == VertexFormat::Half4 || format == VertexFormat::Float4;
}

bool IsEmptyStream(const MeshRenderStream& stream) noexcept {
    return stream.vertexFormat == MeshRenderStreamNone && stream.offset == 0 &&
           stream.byteSize == 0;
}

// 배치 순서로 stream 구간을 본다(정렬, 앞 구간 끝 이상, 넘침 없음, data 안).
class StreamWalk {
public:
    explicit StreamWalk(uint64 dataBytes) noexcept : dataBytes_(dataBytes) {
    }

    bool Next(const MeshRenderStream& stream) noexcept {
        if (stream.offset % MeshRenderStreamAlignment != 0 || stream.offset < end_ ||
            stream.byteSize > dataBytes_ || stream.offset > dataBytes_ - stream.byteSize) {
            return false;
        }

        end_ = stream.offset + stream.byteSize;
        return true;
    }

private:
    uint64 dataBytes_;
    uint64 end_ = 0;
};

// count × elementBytes × elements가 넘치지 않고 byteSize와 같은지다.
bool SizeMatches(const MeshRenderStream& stream, uint64 count, uint64 elementBytes,
                 uint64 elements) noexcept {
    const uint64 unit = elementBytes * elements;
    if (unit != 0 && count > NumericLimits<uint64>::Max() / unit) {
        return false;
    }

    return stream.byteSize == count * unit;
}

// data가 빈 LOD(장치 없는 프로파일 — 5.5.2절 3)는 stream이 모두 형식 0·구간 0이다.
Result<void> ValidateStreamlessLod(const StaticMeshRenderLod& lod, uint32 index,
                                   MeshDiagnostic* failure) noexcept {
    if (!IsEmptyStream(lod.positions) || !IsEmptyStream(lod.tangentBasis) ||
        !IsEmptyStream(lod.colors) || !IsEmptyStream(lod.indices)) {
        return Corrupt(failure, MeshIssue::RenderStreamRange, index);
    }

    if (!lod.texCoords.IsEmpty()) {
        return Corrupt(failure, MeshIssue::RenderTexCoordLayout, index);
    }

    if (lod.indexFormat != 0 && !IndexFormatFromNumber(lod.indexFormat)) {
        return Corrupt(failure, MeshIssue::RenderStreamFormat, index);
    }

    return Result<void>::Success();
}

Result<void> ValidateStreams(const StaticMeshRenderLod& lod, uint32 index,
                             MeshDiagnostic* failure) noexcept {
    const uint64 vertices = lod.vertexCount;
    VertexFormat position = VertexFormat::Float1;
    VertexFormat tangent = VertexFormat::Float1;
    VertexFormat color = VertexFormat::Float1;
    const bool colored = lod.colors.vertexFormat != MeshRenderStreamNone;
    auto indexFormat = IndexFormatFromNumber(lod.indexFormat);
    if (!DecodeFormat(lod.positions.vertexFormat, position) || position != VertexFormat::Float3 ||
        !DecodeFormat(lod.tangentBasis.vertexFormat, tangent) ||
        (tangent != VertexFormat::Byte4Snorm && tangent != VertexFormat::Short4Snorm) ||
        (colored && (!DecodeFormat(lod.colors.vertexFormat, color) ||
                     (color != VertexFormat::Bgra8Unorm && color != VertexFormat::UByte4Unorm))) ||
        lod.indices.vertexFormat != MeshRenderStreamNone || !indexFormat) {
        return Corrupt(failure, MeshIssue::RenderStreamFormat, index);
    }

    // 16비트 index는 정점 MeshIndex16MaxVertices개까지다(0xFFFF는 OpenGL ES primitive restart).
    if (indexFormat.Value() == IndexFormat::UInt16 && vertices > MeshIndex16MaxVertices) {
        return Corrupt(failure, MeshIssue::RenderStreamFormat, index);
    }

    // UV stream: 수 상한, 형식, 정밀도 하나, 2성분은 마지막만.
    if (lod.texCoords.Size() > MeshMaxTexCoordStreams) {
        return Corrupt(failure, MeshIssue::RenderTexCoordLayout, index);
    }

    VertexFormat texCoords[MeshMaxTexCoordStreams] = {};
    for (usize stream = 0; stream < lod.texCoords.Size(); ++stream) {
        VertexFormat& format = texCoords[stream];
        if (!DecodeFormat(lod.texCoords[stream].vertexFormat, format) ||
            !IsTexCoordFormat(format)) {
            return Corrupt(failure, MeshIssue::RenderStreamFormat, index, stream);
        }

        const bool last = stream + 1 == lod.texCoords.Size();
        if (IsHalfTexCoord(format) != IsHalfTexCoord(texCoords[0]) ||
            (!last && !IsPairTexCoord(format))) {
            return Corrupt(failure, MeshIssue::RenderTexCoordLayout, index, stream);
        }
    }

    // 크기.
    const uint32 tangentBytes = graphics::VertexFormatSize(tangent);
    if (!SizeMatches(lod.positions, vertices, graphics::VertexFormatSize(VertexFormat::Float3),
                     1) ||
        !SizeMatches(lod.tangentBasis, vertices, tangentBytes, TangentBasisElements)) {
        return Corrupt(failure, MeshIssue::RenderStreamSize, index);
    }

    for (usize stream = 0; stream < lod.texCoords.Size(); ++stream) {
        const uint32 bytes = graphics::VertexFormatSize(texCoords[stream]);
        if (!SizeMatches(lod.texCoords[stream], vertices, bytes, 1)) {
            return Corrupt(failure, MeshIssue::RenderStreamSize, index, stream);
        }
    }

    if (!colored) {
        if (!IsEmptyStream(lod.colors)) {
            return Corrupt(failure, MeshIssue::RenderStreamRange, index);
        }
    } else if (!SizeMatches(lod.colors, vertices, graphics::VertexFormatSize(color), 1)) {
        return Corrupt(failure, MeshIssue::RenderStreamSize, index);
    }

    const uint64 width = graphics::IndexFormatSize(indexFormat.Value());
    if (lod.indices.byteSize % width != 0 ||
        (lod.indices.byteSize / width) % IndicesPerTriangle != 0) {
        return Corrupt(failure, MeshIssue::RenderIndexCount, index);
    }

    // 구간(배치 순서).
    StreamWalk walk(lod.data.rawBytes);
    if (!walk.Next(lod.positions) || !walk.Next(lod.tangentBasis)) {
        return Corrupt(failure, MeshIssue::RenderStreamRange, index);
    }

    for (usize stream = 0; stream < lod.texCoords.Size(); ++stream) {
        if (!walk.Next(lod.texCoords[stream])) {
            return Corrupt(failure, MeshIssue::RenderStreamRange, index, stream);
        }
    }

    if ((colored && !walk.Next(lod.colors)) || !walk.Next(lod.indices)) {
        return Corrupt(failure, MeshIssue::RenderStreamRange, index);
    }

    return Result<void>::Success();
}

Result<void> ValidateSections(const StaticMeshRenderLod& lod, uint32 index, bool streams,
                              MeshDiagnostic* failure) noexcept {
    uint64 indexCount = 0;
    if (streams) {
        indexCount = lod.indices.byteSize /
                     graphics::IndexFormatSize(IndexFormatFromNumber(lod.indexFormat).Value());
    }

    for (usize section = 0; section < lod.sections.Size(); ++section) {
        const MeshRenderSection& value = lod.sections[section];
        const uint64 end =
            uint64{value.firstIndex} + uint64{value.triangleCount} * IndicesPerTriangle;
        // 빈 섹션은 정점 범위 0, 0이다(MA2a 빌드 결과와 같다).
        if (value.triangleCount == 0) {
            if ((streams && end > indexCount) || value.minVertex != 0 || value.maxVertex != 0) {
                return Corrupt(failure, MeshIssue::RenderSectionRange, index, section);
            }

            continue;
        }

        if ((streams && end > indexCount) || value.minVertex > value.maxVertex ||
            value.maxVertex >= lod.vertexCount) {
            return Corrupt(failure, MeshIssue::RenderSectionRange, index, section);
        }
    }

    return Result<void>::Success();
}

} // namespace

Result<void> ValidateStaticMeshRenderData(const StaticMeshRenderData& renderData,
                                          MeshDiagnostic* failure) noexcept {
    if (renderData.lods.Size() > StaticMeshMaxLods) {
        return Corrupt(failure, MeshIssue::RenderLodCount, MeshNoIndex);
    }

    const Box<double>& box = renderData.box;
    if (!RenderFiniteVector(box.min) || !RenderFiniteVector(box.max) || box.min.x > box.max.x ||
        box.min.y > box.max.y || box.min.z > box.max.z ||
        !RenderFiniteValue(renderData.sphereRadius) || renderData.sphereRadius < 0.0) {
        return Corrupt(failure, MeshIssue::RenderBounds, MeshNoIndex);
    }

    for (usize lodIndex = 0; lodIndex < renderData.lods.Size(); ++lodIndex) {
        const StaticMeshRenderLod& lod = renderData.lods[lodIndex];
        const auto index = static_cast<uint32>(lodIndex);
        const bool streams = !lod.data.IsEmpty();
        Result<void> streamed = streams ? ValidateStreams(lod, index, failure)
                                        : ValidateStreamlessLod(lod, index, failure);
        if (!streamed) {
            return streamed;
        }

        if (auto sections = ValidateSections(lod, index, streams, failure); !sections) {
            return sections;
        }
    }

    return Result<void>::Success();
}

Result<void> ValidateStaticMeshIndices(const StaticMeshRenderLod& lod, uint32 lodIndex,
                                       Span<const uint8> data, MeshDiagnostic* failure) noexcept {
    if (data.Size() != lod.data.rawBytes || lod.indices.byteSize > data.Size() ||
        lod.indices.offset > data.Size() - lod.indices.byteSize) {
        return Corrupt(failure, MeshIssue::RenderStreamRange, lodIndex);
    }

    auto format = IndexFormatFromNumber(lod.indexFormat);
    if (!format) {
        return Corrupt(failure, MeshIssue::RenderStreamFormat, lodIndex);
    }

    const uint8* bytes = data.Data() + lod.indices.offset;
    const bool wide = format.Value() == IndexFormat::UInt32;
    const uint64 width = graphics::IndexFormatSize(format.Value());
    const uint64 count = lod.indices.byteSize / width;
    for (uint64 position = 0; position < count; ++position) {
        const uint8* at = bytes + position * width;
        const uint32 value = wide ? LoadLittleEndian<uint32>(at) : LoadLittleEndian<uint16>(at);
        if (value >= lod.vertexCount) {
            return Corrupt(failure, MeshIssue::RenderIndexValue, lodIndex, position);
        }
    }

    return Result<void>::Success();
}

Result<void> ValidateStaticMesh(const StaticMesh& mesh, const Package& package, Allocator& scratch,
                                MeshDiagnostic* meshFailure,
                                CollisionDiagnostic* collisionFailure) noexcept {
    if (auto collision = ValidateSimpleCollision(mesh.simpleCollision, collisionFailure);
        !collision) {
        return collision;
    }

    if (auto bulk =
            ValidateSimpleCollisionBulk(mesh.simpleCollision, package, scratch, collisionFailure);
        !bulk) {
        return bulk;
    }

    return ValidateStaticMeshRenderData(mesh.renderData, meshFailure);
}

StaticMeshBounds StaticMeshExtendedBounds(const StaticMesh& mesh) noexcept {
    StaticMeshBounds bounds;
    bounds.box = mesh.renderData.box;
    bounds.sphereRadius = mesh.renderData.sphereRadius;
    const Vec3d& positive = mesh.positiveBoundsExtension;
    const Vec3d& negative = mesh.negativeBoundsExtension;
    const bool extended = positive.x != 0.0 || positive.y != 0.0 || positive.z != 0.0 ||
                          negative.x != 0.0 || negative.y != 0.0 || negative.z != 0.0;
    if (!extended) {
        return bounds;
    }

    bounds.box.min = Vec3d(bounds.box.min.x - negative.x, bounds.box.min.y - negative.y,
                           bounds.box.min.z - negative.z);
    bounds.box.max = Vec3d(bounds.box.max.x + positive.x, bounds.box.max.y + positive.y,
                           bounds.box.max.z + positive.z);
    const double halfX = (bounds.box.max.x - bounds.box.min.x) * 0.5;
    const double halfY = (bounds.box.max.y - bounds.box.min.y) * 0.5;
    const double halfZ = (bounds.box.max.z - bounds.box.min.z) * 0.5;
    bounds.sphereRadius = math::Sqrt(halfX * halfX + halfY * halfY + halfZ * halfZ);
    return bounds;
}

} // namespace rocks
