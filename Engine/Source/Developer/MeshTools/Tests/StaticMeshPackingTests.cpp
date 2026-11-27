#include <Base/Endian.h>
#include <Core/Math/Color.h>
#include <Core/Math/Float16.h>
#include <Core/Memory/SystemAllocator.h>
#include <GraphicsTypes/IndexFormat.h>
#include <GraphicsTypes/VertexFormat.h>
#include <MeshTools/StaticMeshPacking.h>
#include <Platform/Memory.h>
#include <TestSupport/Test.h>

// 정적 메시 쿠킹 LOD 패킹 시험이다(Docs/Design/MeshAnimationAssets 5.3절·5.5.2절 1): 배치 순서와 16
// byte 정렬, 위치·tangent 기저(w = 1, 법선 w = 쌍법선 부호)·UV 쌍(half 반올림·float)·색(sRGB, BGRA·
// RGBA, 모두 흰색이면 없음)·index(16·32비트 경계)·섹션 플래그 byte, 결정성, 검증 통과.
using namespace rocks;
namespace static_mesh_packing_tests_file {

using graphics::IndexFormat;
using graphics::VertexFormat;

namespace {

uint32 Number(VertexFormat format) noexcept {
    return graphics::VertexFormatNumber(format);
}

// 정점 vertices개(UV channels개, 색 color)·삼각형 하나 이상의 합성 빌드 결과다.
void Fill(StaticMeshBuildResult& built, uint32 vertices, uint32 channels, bool color,
          float colorValue = 0.5f) noexcept {
    for (uint32 vertex = 0; vertex < vertices; ++vertex) {
        const float x = static_cast<float>(vertex);
        CHECK(built.positions.TryPushBack(Vec3(x, x * 2.0f, -x)));
        CHECK(built.normals.TryPushBack(Vec3(0.0f, 0.0f, 1.0f)));
        CHECK(built.tangents.TryPushBack(Vec3(1.0f, 0.0f, 0.0f)));
        CHECK(built.binormalSigns.TryPushBack(vertex % 2 == 0 ? 1.0f : -1.0f));
        if (color) {
            CHECK(built.colors.TryPushBack(Vec4(colorValue, 0.0f, 1.0f, 1.0f)));
        }
    }

    built.texCoordChannels = channels;
    for (uint32 channel = 0; channel < channels; ++channel) {
        for (uint32 vertex = 0; vertex < vertices; ++vertex) {
            CHECK(built.texCoords.TryPushBack(
                Vec2(static_cast<float>(channel) + 0.25f, static_cast<float>(vertex) * 0.1f)));
        }
    }

    const uint32 triangles = vertices / 3;
    for (uint32 index = 0; index < triangles * 3; ++index) {
        CHECK(built.indices.TryPushBack(index));
    }

    MeshBuildSection section;
    section.materialIndex = 2;
    section.triangleCount = triangles;
    section.maxVertex = triangles == 0 ? 0 : triangles * 3 - 1;
    section.castShadow = false;
    CHECK(built.sections.TryPushBack(section));
}

// 패킹 결과가 로드 검증을 통과하는지다(data는 덩어리 byte).
bool Validates(Allocator& allocator, StaticMeshRenderLod& lod, Span<const uint8> bytes) noexcept {
    lod.data.bulkId = 1;
    lod.data.rawBytes = bytes.Size();
    StaticMeshRenderData data(allocator);
    StaticMeshRenderLod copy(allocator);
    copy.vertexCount = lod.vertexCount;
    copy.data = lod.data;
    copy.positions = lod.positions;
    copy.tangentBasis = lod.tangentBasis;
    copy.colors = lod.colors;
    copy.indexFormat = lod.indexFormat;
    copy.indices = lod.indices;
    if (!copy.texCoords.TryAppend(lod.texCoords.View()) ||
        !copy.sections.TryAppend(lod.sections.View()) || !data.lods.TryPushBack(Move(copy))) {
        return false;
    }

    return ValidateStaticMeshRenderData(data).IsOk() &&
           ValidateStaticMeshIndices(lod, 0, bytes).IsOk();
}

bool Aligned(const MeshRenderStream& stream) noexcept {
    return stream.offset % MeshRenderStreamAlignment == 0;
}

} // namespace

// 기본 설정: Float3 위치, Byte4Snorm 기저((tangent, 1), (법선, 부호)), Half4·Half2 UV, BGRA 색,
// 16비트 index, 배치 순서와 16 byte 정렬(사이는 0), 섹션 플래그는 섹션 정보에서.
TEST(StaticMeshPacking_DefaultLayout) {
    SystemAllocator allocator;
    StaticMeshBuildResult built(allocator);
    Fill(built, 3, 3, true);
    MeshSectionInfo info;
    info.enableCollision = false;
    info.forceOpaque = true;
    const MeshSectionInfo infos[1] = {info};
    Array<uint8> bytes(allocator);
    StaticMeshRenderLod lod(allocator);
    REQUIRE(PackStaticMeshLod(built, MeshBuildSettings{}, Span<const MeshSectionInfo>(infos),
                              MeshColorFormat::Bgra8Unorm, bytes, lod));
    CHECK(lod.vertexCount == 3);
    CHECK(lod.positions.vertexFormat == Number(VertexFormat::Float3) && lod.positions.offset == 0 &&
          lod.positions.byteSize == 36);
    CHECK(lod.tangentBasis.vertexFormat == Number(VertexFormat::Byte4Snorm) &&
          lod.tangentBasis.offset == 48 && lod.tangentBasis.byteSize == 24);
    REQUIRE(lod.texCoords.Size() == 2);
    CHECK(lod.texCoords[0].vertexFormat == Number(VertexFormat::Half4) &&
          lod.texCoords[0].offset == 80 && lod.texCoords[0].byteSize == 24);
    CHECK(lod.texCoords[1].vertexFormat == Number(VertexFormat::Half2) &&
          lod.texCoords[1].byteSize == 12 && Aligned(lod.texCoords[1]));
    CHECK(lod.colors.vertexFormat == Number(VertexFormat::Bgra8Unorm) &&
          lod.colors.byteSize == 12 && Aligned(lod.colors));
    CHECK(lod.indexFormat == graphics::IndexFormatNumber(IndexFormat::UInt16) &&
          lod.indices.vertexFormat == MeshRenderStreamNone && lod.indices.byteSize == 6 &&
          Aligned(lod.indices));
    CHECK(bytes.Size() == lod.indices.offset + lod.indices.byteSize);
    // 정렬 사이 byte는 0이다(위치 36 → 48).
    for (usize at = 36; at < 48; ++at) {
        CHECK(bytes[at] == 0);
    }

    // 위치 1번 정점(1, 2, −1).
    CHECK(platform::BitCast<float>(LoadLittleEndian<uint32>(bytes.Data() + 12)) == 1.0f);
    CHECK(platform::BitCast<float>(LoadLittleEndian<uint32>(bytes.Data() + 20)) == -1.0f);
    // 기저: 정점 0 = (127, 0, 0, 127), (0, 0, 127, 127), 정점 1의 법선 w = −127.
    const uint8* basis = bytes.Data() + lod.tangentBasis.offset;
    CHECK(static_cast<int8>(basis[0]) == 127 && basis[1] == 0 &&
          static_cast<int8>(basis[3]) == 127);
    CHECK(static_cast<int8>(basis[6]) == 127 && static_cast<int8>(basis[7]) == 127);
    CHECK(static_cast<int8>(basis[8 + 7]) == -127);
    // UV: 정점 0의 (0.25, 0, 1.25, 0) half, 마지막 채널(2.25, 0).
    const uint8* uv = bytes.Data() + lod.texCoords[0].offset;
    CHECK(LoadLittleEndian<uint16>(uv) == Float16::FromFloat(0.25f).bits);
    CHECK(LoadLittleEndian<uint16>(uv + 4) == Float16::FromFloat(1.25f).bits);
    CHECK(LoadLittleEndian<uint16>(bytes.Data() + lod.texCoords[1].offset) ==
          Float16::FromFloat(2.25f).bits);
    // 색: BGRA 순서, sRGB 8비트.
    const Color expected = LinearColor(0.5f, 0.0f, 1.0f, 1.0f).ToSRGB();
    const uint8* colors = bytes.Data() + lod.colors.offset;
    CHECK(colors[0] == expected.b && colors[1] == expected.g && colors[2] == expected.r &&
          colors[3] == expected.a);
    // 섹션.
    REQUIRE(lod.sections.Size() == 1);
    const MeshRenderSection& section = lod.sections[0];
    CHECK(section.materialIndex == 2 && section.triangleCount == 1 && section.maxVertex == 2 &&
          !section.castShadow && !section.enableCollision && section.forceOpaque &&
          section.visibleInRayTracing && section.affectDistanceFieldLighting);
    CHECK(Validates(allocator, lod, bytes.View()));
}

// 고정밀: Short4Snorm 기저(w = 32767), Float4·Float2 UV.
TEST(StaticMeshPacking_HighPrecision) {
    SystemAllocator allocator;
    StaticMeshBuildResult built(allocator);
    Fill(built, 3, 3, false);
    MeshBuildSettings settings;
    settings.useHighPrecisionTangentBasis = true;
    settings.useFullPrecisionUVs = true;
    Array<uint8> bytes(allocator);
    StaticMeshRenderLod lod(allocator);
    REQUIRE(PackStaticMeshLod(built, settings, {}, MeshColorFormat::Bgra8Unorm, bytes, lod));
    CHECK(lod.tangentBasis.vertexFormat == Number(VertexFormat::Short4Snorm) &&
          lod.tangentBasis.byteSize == 48);
    const uint8* basis = bytes.Data() + lod.tangentBasis.offset;
    CHECK(static_cast<int16>(LoadLittleEndian<uint16>(basis + 6)) == 32767);
    REQUIRE(lod.texCoords.Size() == 2);
    CHECK(lod.texCoords[0].vertexFormat == Number(VertexFormat::Float4) &&
          lod.texCoords[1].vertexFormat == Number(VertexFormat::Float2));
    CHECK(platform::BitCast<float>(
              LoadLittleEndian<uint32>(bytes.Data() + lod.texCoords[0].offset)) == 0.25f);
    CHECK(lod.colors.vertexFormat == MeshRenderStreamNone && lod.colors.byteSize == 0);
    CHECK(Validates(allocator, lod, bytes.View()));
}

// 색: 원본에 없으면·모두 흰색이면 stream이 없다. OpenGL 형식은 RGBA 순서다.
TEST(StaticMeshPacking_Colors) {
    SystemAllocator allocator;
    {
        StaticMeshBuildResult built(allocator);
        Fill(built, 3, 0, true, 1.0f);
        for (Vec4& color : built.colors) {
            color = Vec4(1.0f, 1.0f, 1.0f, 1.0f);
        }

        Array<uint8> bytes(allocator);
        StaticMeshRenderLod lod(allocator);
        REQUIRE(PackStaticMeshLod(built, MeshBuildSettings{}, {}, MeshColorFormat::Bgra8Unorm,
                                  bytes, lod));
        CHECK(lod.colors.vertexFormat == MeshRenderStreamNone && lod.texCoords.IsEmpty());
        CHECK(Validates(allocator, lod, bytes.View()));
    }

    StaticMeshBuildResult built(allocator);
    Fill(built, 3, 1, true, 0.25f);
    Array<uint8> bytes(allocator);
    StaticMeshRenderLod lod(allocator);
    REQUIRE(PackStaticMeshLod(built, MeshBuildSettings{}, {}, MeshColorFormat::UByte4Unorm, bytes,
                              lod));
    CHECK(lod.colors.vertexFormat == Number(VertexFormat::UByte4Unorm));
    const Color expected = LinearColor(0.25f, 0.0f, 1.0f, 1.0f).ToSRGB();
    const uint8* colors = bytes.Data() + lod.colors.offset;
    CHECK(colors[0] == expected.r && colors[1] == expected.g && colors[2] == expected.b &&
          colors[3] == expected.a);
    CHECK(Validates(allocator, lod, bytes.View()));
}

// index 폭 경계: 정점 65535개는 16비트, 65536개는 32비트(0xFFFF는 GLES primitive restart).
TEST(StaticMeshPacking_IndexWidth) {
    SystemAllocator allocator;
    const uint32 counts[2] = {MeshIndex16MaxVertices, MeshIndex16MaxVertices + 1};
    for (uint32 vertices : counts) {
        StaticMeshBuildResult built(allocator);
        Fill(built, vertices, 0, false);
        Array<uint8> bytes(allocator);
        StaticMeshRenderLod lod(allocator);
        REQUIRE(PackStaticMeshLod(built, MeshBuildSettings{}, {}, MeshColorFormat::Bgra8Unorm,
                                  bytes, lod));
        const bool narrow = vertices <= MeshIndex16MaxVertices;
        CHECK(lod.indexFormat ==
              graphics::IndexFormatNumber(narrow ? IndexFormat::UInt16 : IndexFormat::UInt32));
        CHECK(lod.indices.byteSize == uint64{built.indices.Size()} * (narrow ? 2u : 4u));
        CHECK(Validates(allocator, lod, bytes.View()));
    }
}

// 정점 0개(모든 삼각형이 퇴화)도 정상 배치다. 같은 입력이면 같은 byte다.
TEST(StaticMeshPacking_EmptyAndDeterministic) {
    SystemAllocator allocator;
    StaticMeshBuildResult empty(allocator);
    MeshBuildSection section;
    REQUIRE(empty.sections.TryPushBack(section));
    Array<uint8> none(allocator);
    StaticMeshRenderLod lod(allocator);
    REQUIRE(
        PackStaticMeshLod(empty, MeshBuildSettings{}, {}, MeshColorFormat::Bgra8Unorm, none, lod));
    CHECK(lod.vertexCount == 0 && lod.positions.byteSize == 0 && lod.indices.byteSize == 0 &&
          lod.sections.Size() == 1);
    CHECK(Validates(allocator, lod, none.View()));

    StaticMeshBuildResult built(allocator);
    Fill(built, 9, 4, true);
    Array<uint8> first(allocator);
    Array<uint8> second(allocator);
    StaticMeshRenderLod a(allocator);
    StaticMeshRenderLod b(allocator);
    REQUIRE(
        PackStaticMeshLod(built, MeshBuildSettings{}, {}, MeshColorFormat::Bgra8Unorm, first, a));
    REQUIRE(
        PackStaticMeshLod(built, MeshBuildSettings{}, {}, MeshColorFormat::Bgra8Unorm, second, b));
    CHECK(first.Size() == second.Size() &&
          platform::MemoryEquals(first.Data(), second.Data(), first.Size()));
    CHECK(a.texCoords.Size() == 2 && a.texCoords[1].vertexFormat == Number(VertexFormat::Half4));
}

} // namespace static_mesh_packing_tests_file
