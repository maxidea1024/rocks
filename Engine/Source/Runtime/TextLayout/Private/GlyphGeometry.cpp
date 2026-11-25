// 글리프 geometry 직렬화다(Docs/Design/Font 6.5절 FT2e). 형식: u32 버전, u16 unitsPerEm, u32 윤곽
// 수, u32 edge 수, u32 점 수, 윤곽 끝(u32), edge 종류(u8), 점(i32 x·y), 모두 little-endian.
#include <TextLayout/GlyphGeometry.h>

#include <Core/Encoding/ByteReader.h>
#include <Core/Encoding/ByteWriter.h>

namespace rocks::text {

namespace {

constexpr uint32 MinEdgeKind = static_cast<uint32>(GeometryEdgeKind::Line);
constexpr uint32 MaxEdgeKind = static_cast<uint32>(GeometryEdgeKind::Cubic);

// 점 하나의 좌표 수(x·y)다.
constexpr uint64 CoordinatesPerPoint = 2;

usize PointCount(GeometryEdgeKind kind) noexcept {
    return static_cast<usize>(kind) + 1;
}

} // namespace

Result<void> WriteGlyphGeometry(const GlyphGeometry& geometry, Array<uint8>& out) noexcept {
    ByteWriter writer(out);
    writer.WriteLittleEndian<uint32>(GlyphGeometryFormatVersion);
    writer.WriteLittleEndian<uint16>(geometry.unitsPerEm);
    writer.WriteLittleEndian<uint32>(static_cast<uint32>(geometry.contourEdgeEnds.Size()));
    writer.WriteLittleEndian<uint32>(static_cast<uint32>(geometry.edgeKinds.Size()));
    writer.WriteLittleEndian<uint32>(static_cast<uint32>(geometry.points.Size()));
    for (const uint32 end : geometry.contourEdgeEnds) {
        writer.WriteLittleEndian<uint32>(end);
    }

    for (const GeometryEdgeKind kind : geometry.edgeKinds) {
        writer.WriteU8(static_cast<uint8>(kind));
    }

    for (const GeometryPoint& point : geometry.points) {
        writer.WriteLittleEndian<int32>(point.x);
        writer.WriteLittleEndian<int32>(point.y);
    }

    return writer.Status();
}

Result<void> ReadGlyphGeometry(Span<const uint8> bytes, GlyphGeometry& out) noexcept {
    out.Clear();
    ByteReader reader(bytes);
    const uint32 version = reader.ReadU32LE();
    if (reader.Failed()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (version != GlyphGeometryFormatVersion) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    const uint16 unitsPerEm = reader.ReadU16LE();
    const uint32 contourCount = reader.ReadU32LE();
    const uint32 edgeCount = reader.ReadU32LE();
    const uint32 pointCount = reader.ReadU32LE();
    // 남은 byte로 수를 먼저 검사해 큰 수로 할당하지 않는다.
    const uint64 required = static_cast<uint64>(contourCount) * sizeof(uint32) + edgeCount +
                            static_cast<uint64>(pointCount) * CoordinatesPerPoint * sizeof(int32);
    if (reader.Failed() || required != reader.Remaining()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (auto reserved = out.contourEdgeEnds.TryReserve(contourCount); !reserved) {
        return reserved;
    }

    if (auto reserved = out.edgeKinds.TryReserve(edgeCount); !reserved) {
        return reserved;
    }

    if (auto reserved = out.points.TryReserve(pointCount); !reserved) {
        return reserved;
    }

    uint32 previousEnd = 0;
    for (uint32 index = 0; index < contourCount; ++index) {
        const uint32 end = reader.ReadU32LE();
        if (end <= previousEnd || end > edgeCount) {
            out.Clear();
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        (void)out.contourEdgeEnds.TryPushBack(end);
        previousEnd = end;
    }

    usize expectedPoints = 0;
    for (uint32 index = 0; index < edgeCount; ++index) {
        const uint32 kind = reader.ReadU8();
        if (kind < MinEdgeKind || kind > MaxEdgeKind) {
            out.Clear();
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        (void)out.edgeKinds.TryPushBack(static_cast<GeometryEdgeKind>(kind));
        expectedPoints += PointCount(static_cast<GeometryEdgeKind>(kind));
    }

    if (previousEnd != edgeCount || expectedPoints != pointCount) {
        out.Clear();
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    for (uint32 index = 0; index < pointCount; ++index) {
        GeometryPoint point;
        point.x = static_cast<int32>(reader.ReadU32LE());
        point.y = static_cast<int32>(reader.ReadU32LE());
        (void)out.points.TryPushBack(point);
    }

    out.unitsPerEm = unitsPerEm;
    return Result<void>::Success();
}

} // namespace rocks::text
