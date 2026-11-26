#include <Base/Endian.h>
#include <Core/Memory/SystemAllocator.h>
#include <GraphicsTypes/IndexFormat.h>
#include <GraphicsTypes/VertexFormat.h>
#include <Mesh/StaticMesh/StaticMeshValidation.h>
#include <Platform/Memory.h>
#include <TestSupport/Test.h>

// 정적 메시 쿠킹 렌더 데이터 로드 검증 시험이다(Docs/Design/MeshAnimationAssets 5.5.1절 9): 정상
// 배치(stream 전부·색 없음·UV 홀수·서버 쿠킹·빈 LOD·정점 0개), 규칙마다 하나씩 어기기(형식·UV 배치·
// 크기·구간·index 수·섹션·bounds·LOD 수), index 값 검사, 확장 bounds.
using namespace rocks;
namespace static_mesh_validation_tests_file {

using graphics::IndexFormat;
using graphics::VertexFormat;

uint32 Number(VertexFormat format) noexcept {
    return graphics::VertexFormatNumber(format);
}

double NaN() noexcept {
    return platform::BitCast<double>(0x7ff8000000000000ull);
}

uint64 AlignUp(uint64 value) noexcept {
    return (value + MeshRenderStreamAlignment - 1) & ~(MeshRenderStreamAlignment - 1);
}

// 정상 LOD 하나의 배치를 만든다(정점 vertices개, UV 채널 texCoords개(half), 색, 16비트 index
// indexCount개). 구간은 배치 순서로 16 byte 정렬해 잇고 data.rawBytes는 끝이다.
void Layout(StaticMeshRenderLod& lod, uint32 vertices, uint32 texCoords, bool color,
            uint32 indexCount, bool fullPrecision = false, bool wideTangents = false,
            VertexFormat colorFormat = VertexFormat::Bgra8Unorm) noexcept {
    lod.vertexCount = vertices;
    uint64 offset = 0;
    auto place = [&](MeshRenderStream& stream, VertexFormat format, uint64 bytes) noexcept {
        offset = AlignUp(offset);
        stream.vertexFormat = Number(format);
        stream.offset = offset;
        stream.byteSize = bytes;
        offset += bytes;
    };
    place(lod.positions, VertexFormat::Float3, uint64{vertices} * 12);
    const VertexFormat basis = wideTangents ? VertexFormat::Short4Snorm : VertexFormat::Byte4Snorm;
    place(lod.tangentBasis, basis, uint64{vertices} * graphics::VertexFormatSize(basis) * 2);
    lod.texCoords.Clear();
    for (uint32 channel = 0; channel < texCoords; channel += 2) {
        const bool pair = channel + 1 < texCoords;
        const VertexFormat format = fullPrecision
                                        ? (pair ? VertexFormat::Float4 : VertexFormat::Float2)
                                        : (pair ? VertexFormat::Half4 : VertexFormat::Half2);
        MeshRenderStream stream;
        place(stream, format, uint64{vertices} * graphics::VertexFormatSize(format));
        (void)lod.texCoords.TryPushBack(stream);
    }

    if (color) {
        place(lod.colors, colorFormat, uint64{vertices} * 4);
    } else {
        lod.colors = MeshRenderStream{};
    }

    lod.indexFormat = graphics::IndexFormatNumber(IndexFormat::UInt16);
    offset = AlignUp(offset);
    lod.indices.vertexFormat = MeshRenderStreamNone;
    lod.indices.offset = offset;
    lod.indices.byteSize = uint64{indexCount} * 2;
    offset += lod.indices.byteSize;
    lod.data.bulkId = 1;
    lod.data.rawBytes = offset;
    lod.sections.Clear();
    MeshRenderSection section;
    section.triangleCount = indexCount / 3;
    section.maxVertex = vertices == 0 ? 0 : vertices - 1;
    (void)lod.sections.TryPushBack(section);
}

bool Fails(const StaticMeshRenderData& data, MeshIssue issue, uint32 index,
           uint64 count = 0) noexcept {
    MeshDiagnostic failure;
    auto checked = ValidateStaticMeshRenderData(data, &failure);
    return !checked && checked.Error() == ErrorCode::InvalidArgument &&
           failure.code == AssetDiagnosticCode::CorruptPackage && failure.issue == issue &&
           failure.index == index && failure.count == count;
}

struct Fixture {
    Fixture() noexcept : data(allocator) {
        StaticMeshRenderLod lod(allocator);
        Layout(lod, 4, 3, true, 6);
        (void)data.lods.TryPushBack(Move(lod));
        data.box = Box<double>(Vec3d(-1.0, -1.0, 0.0), Vec3d(1.0, 1.0, 0.0));
        data.sphereRadius = 1.5;
    }

    StaticMeshRenderLod& Lod() noexcept {
        return data.lods[0];
    }

    SystemAllocator allocator;
    StaticMeshRenderData data;
};

// 정상 배치: stream 전부, 색 없음, UV 0·1·2·8채널, float UV, 서버 쿠킹(data 없음), LOD 0개, 정점
// 0개(모든 삼각형이 퇴화), 빈 섹션.
TEST(StaticMeshValidation_AcceptsValidLayouts) {
    Fixture fixture;
    CHECK(ValidateStaticMeshRenderData(fixture.data));
    const uint32 channels[] = {0, 1, 2, 8};
    for (uint32 count : channels) {
        Layout(fixture.Lod(), 4, count, false, 6);
        CHECK(ValidateStaticMeshRenderData(fixture.data));
        Layout(fixture.Lod(), 4, count, true, 6, true);
        CHECK(ValidateStaticMeshRenderData(fixture.data));
    }

    Layout(fixture.Lod(), 0, 2, true, 0);
    fixture.Lod().sections[0].maxVertex = 0;
    CHECK(ValidateStaticMeshRenderData(fixture.data));
    // 빈 섹션(삼각형 0, 정점 범위 0, 0 — index 끝 위치도 받는다).
    Layout(fixture.Lod(), 4, 2, true, 6);
    MeshRenderSection empty;
    empty.firstIndex = 6;
    (void)fixture.Lod().sections.TryPushBack(empty);
    CHECK(ValidateStaticMeshRenderData(fixture.data));
    // Short4Snorm tangent 기저(정점마다 16 byte)·UByte4Unorm 색(GL 프로파일).
    Layout(fixture.Lod(), 4, 3, true, 6, false, true, VertexFormat::UByte4Unorm);
    CHECK(fixture.Lod().tangentBasis.byteSize == 4 * 16);
    CHECK(ValidateStaticMeshRenderData(fixture.data));
    // 16비트 index는 정점 65535개까지 받는다.
    Layout(fixture.Lod(), MeshIndex16MaxVertices, 0, false, 6);
    CHECK(ValidateStaticMeshRenderData(fixture.data));
    Layout(fixture.Lod(), 4, 2, true, 6);
    // 섹션 슬롯 번호는 보지 않는다(범위 밖은 기본 머티리얼).
    fixture.Lod().sections[0].materialIndex = 0xFFFFFFFFu;
    CHECK(ValidateStaticMeshRenderData(fixture.data));
    // 서버 쿠킹: data·stream 없음, 섹션·정점 수는 남는다(index 구간은 보지 않는다).
    StaticMeshRenderLod& lod = fixture.Lod();
    lod.data = BulkDataRef{};
    lod.positions = lod.tangentBasis = lod.colors = lod.indices = MeshRenderStream{};
    lod.texCoords.Clear();
    CHECK(ValidateStaticMeshRenderData(fixture.data));
    lod.indexFormat = 0;
    CHECK(ValidateStaticMeshRenderData(fixture.data));
    fixture.data.lods.Clear();
    CHECK(ValidateStaticMeshRenderData(fixture.data));
    // 16비트·32비트 index 둘 다 받는다.
    Fixture wide;
    wide.Lod().indexFormat = graphics::IndexFormatNumber(IndexFormat::UInt32);
    wide.Lod().indices.byteSize = 6 * 4;
    wide.Lod().data.rawBytes = wide.Lod().indices.offset + wide.Lod().indices.byteSize;
    CHECK(ValidateStaticMeshRenderData(wide.data));
}

// 형식 번호: 위치·tangent·UV·색·index 형식과 index stream의 형식 0.
TEST(StaticMeshValidation_RejectsFormats) {
    struct Case {
        void (*mutate)(StaticMeshRenderLod&) noexcept;
        uint64 count;
    };

    const Case cases[] = {
        {[](StaticMeshRenderLod& l) noexcept {
             l.positions.vertexFormat = Number(VertexFormat::Float4);
         },
         0},
        {[](StaticMeshRenderLod& l) noexcept { l.positions.vertexFormat = 0; }, 0},
        {[](StaticMeshRenderLod& l) noexcept {
             l.tangentBasis.vertexFormat = Number(VertexFormat::Float4);
         },
         0},
        {[](StaticMeshRenderLod& l) noexcept {
             l.colors.vertexFormat = Number(VertexFormat::Float4);
         },
         0},
        {[](StaticMeshRenderLod& l) noexcept { l.colors.vertexFormat = 99; }, 0},
        {[](StaticMeshRenderLod& l) noexcept {
             l.indices.vertexFormat = Number(VertexFormat::UInt1);
         },
         0},
        {[](StaticMeshRenderLod& l) noexcept { l.indexFormat = 0; }, 0},
        {[](StaticMeshRenderLod& l) noexcept { l.indexFormat = 3; }, 0},
        {[](StaticMeshRenderLod& l) noexcept {
             l.texCoords[1].vertexFormat = Number(VertexFormat::Short2);
         },
         1},
        // 번호표 밖 번호는 uint8로 잘리기 전에 거부한다(0x103 → Float3가 아니다).
        {[](StaticMeshRenderLod& l) noexcept {
             l.positions.vertexFormat = Number(VertexFormat::Float3) + 256;
         },
         0},
        {[](StaticMeshRenderLod& l) noexcept {
             l.tangentBasis.vertexFormat = Number(VertexFormat::Byte4Snorm) + 256;
         },
         0},
        {[](StaticMeshRenderLod& l) noexcept {
             l.colors.vertexFormat = Number(VertexFormat::Bgra8Unorm) + 256;
         },
         0},
        {[](StaticMeshRenderLod& l) noexcept {
             l.texCoords[0].vertexFormat = Number(VertexFormat::Half4) + 256;
         },
         0},
    };
    for (const Case& value : cases) {
        Fixture fixture;
        value.mutate(fixture.Lod());
        CHECK(Fails(fixture.data, MeshIssue::RenderStreamFormat, 0, value.count));
    }

    // 16비트 index LOD의 정점이 65535개를 넘으면 0xFFFF가 index가 될 수 있다(primitive restart).
    Fixture large;
    Layout(large.Lod(), MeshIndex16MaxVertices + 1, 0, false, 6);
    CHECK(Fails(large.data, MeshIssue::RenderStreamFormat, 0));
}

// UV 배치: stream 수 상한, 정밀도 섞임, 2성분 stream이 마지막이 아님.
TEST(StaticMeshValidation_RejectsTexCoordLayout) {
    Fixture fixture;
    Layout(fixture.Lod(), 4, 8, true, 6);
    MeshRenderStream extra = fixture.Lod().texCoords[3];
    REQUIRE(fixture.Lod().texCoords.TryPushBack(extra));
    CHECK(Fails(fixture.data, MeshIssue::RenderTexCoordLayout, 0));
    Layout(fixture.Lod(), 4, 4, true, 6);
    fixture.Lod().texCoords[1].vertexFormat = Number(VertexFormat::Float4);
    fixture.Lod().texCoords[1].byteSize = 4 * 16;
    CHECK(Fails(fixture.data, MeshIssue::RenderTexCoordLayout, 0, 1));
    Layout(fixture.Lod(), 4, 4, true, 6);
    fixture.Lod().texCoords[0].vertexFormat = Number(VertexFormat::Half2);
    CHECK(Fails(fixture.data, MeshIssue::RenderTexCoordLayout, 0, 0));
}

// 크기: 위치·tangent(원소 둘)·UV·색 byte 수.
TEST(StaticMeshValidation_RejectsSizes) {
    struct Case {
        void (*mutate)(StaticMeshRenderLod&) noexcept;
        uint64 count;
    };

    const Case cases[] = {
        {[](StaticMeshRenderLod& l) noexcept { l.positions.byteSize -= 12; }, 0},
        {[](StaticMeshRenderLod& l) noexcept { l.tangentBasis.byteSize = 4 * 4; }, 0},
        {[](StaticMeshRenderLod& l) noexcept { l.texCoords[1].byteSize += 4; }, 1},
        {[](StaticMeshRenderLod& l) noexcept { l.colors.byteSize = 0; }, 0},
        {[](StaticMeshRenderLod& l) noexcept { l.vertexCount = 5; }, 0},
    };
    for (const Case& value : cases) {
        Fixture fixture;
        value.mutate(fixture.Lod());
        CHECK(Fails(fixture.data, MeshIssue::RenderStreamSize, 0, value.count));
    }

    // 정점 수 × 원소 byte가 uint64를 넘치면 크기 불일치다.
    Fixture huge;
    huge.Lod().vertexCount = 0xFFFFFFFFu;
    huge.Lod().indexFormat = graphics::IndexFormatNumber(IndexFormat::UInt32);
    huge.Lod().indices.byteSize = 6 * 4;
    CHECK(Fails(huge.data, MeshIssue::RenderStreamSize, 0));
}

// 구간: 정렬, 겹침·역순, data 끝 넘음, offset 넘침, 색 없음인데 구간, 서버 LOD에 남은 구간.
TEST(StaticMeshValidation_RejectsRanges) {
    struct Case {
        void (*mutate)(StaticMeshRenderLod&) noexcept;
        uint64 count;
    };

    const Case cases[] = {
        {[](StaticMeshRenderLod& l) noexcept { l.positions.offset = 8; }, 0},
        {[](StaticMeshRenderLod& l) noexcept { l.tangentBasis.offset = 0; }, 0},
        {[](StaticMeshRenderLod& l) noexcept { l.texCoords[0].offset = l.tangentBasis.offset; }, 0},
        {[](StaticMeshRenderLod& l) noexcept { l.texCoords[1].offset = l.texCoords[0].offset; }, 1},
        {[](StaticMeshRenderLod& l) noexcept { l.data.rawBytes -= 1; }, 0},
        {[](StaticMeshRenderLod& l) noexcept { l.indices.offset = 0xFFFFFFFFFFFFFFF0ull; }, 0},
        {[](StaticMeshRenderLod& l) noexcept { l.colors.vertexFormat = MeshRenderStreamNone; }, 0},
    };
    for (const Case& value : cases) {
        Fixture fixture;
        value.mutate(fixture.Lod());
        CHECK(Fails(fixture.data, MeshIssue::RenderStreamRange, 0, value.count));
    }

    Fixture server;
    server.Lod().data = BulkDataRef{};
    CHECK(Fails(server.data, MeshIssue::RenderStreamRange, 0));
    server.Lod().positions = server.Lod().tangentBasis = server.Lod().colors =
        server.Lod().indices = MeshRenderStream{};
    // 서버 LOD의 UV stream 목록은 비어야 한다.
    CHECK(Fails(server.data, MeshIssue::RenderTexCoordLayout, 0));
    server.Lod().texCoords.Clear();
    server.Lod().indexFormat = 7;
    CHECK(Fails(server.data, MeshIssue::RenderStreamFormat, 0));
    server.Lod().indexFormat = 0;
    CHECK(ValidateStaticMeshRenderData(server.data));
    // 서버 LOD도 섹션 정점 범위는 본다.
    server.Lod().sections[0].maxVertex = 4;
    CHECK(Fails(server.data, MeshIssue::RenderSectionRange, 0, 0));
}

// index 수: 폭의 배수가 아님, 3의 배수가 아님.
TEST(StaticMeshValidation_RejectsIndexCount) {
    Fixture odd;
    odd.Lod().indices.byteSize = 11;
    CHECK(Fails(odd.data, MeshIssue::RenderIndexCount, 0));
    Fixture four;
    four.Lod().indices.byteSize = 8;
    CHECK(Fails(four.data, MeshIssue::RenderIndexCount, 0));
}

// 섹션: index 구간 밖, 정점 범위 역순·밖. 둘째 LOD·둘째 섹션의 위치를 진단이 가리킨다.
TEST(StaticMeshValidation_RejectsSections) {
    Fixture fixture;
    StaticMeshRenderLod second(fixture.allocator);
    Layout(second, 4, 2, false, 6);
    MeshRenderSection extra;
    extra.firstIndex = 3;
    extra.triangleCount = 1;
    extra.maxVertex = 3;
    REQUIRE(second.sections.TryPushBack(extra));
    REQUIRE(fixture.data.lods.TryPushBack(Move(second)));
    CHECK(ValidateStaticMeshRenderData(fixture.data));
    fixture.data.lods[1].sections[1].firstIndex = 4;
    CHECK(Fails(fixture.data, MeshIssue::RenderSectionRange, 1, 1));
    fixture.data.lods[1].sections[1].firstIndex = 3;
    fixture.data.lods[1].sections[1].minVertex = 3;
    fixture.data.lods[1].sections[1].maxVertex = 2;
    CHECK(Fails(fixture.data, MeshIssue::RenderSectionRange, 1, 1));
    fixture.data.lods[1].sections[1].minVertex = 0;
    fixture.data.lods[1].sections[1].maxVertex = 4;
    CHECK(Fails(fixture.data, MeshIssue::RenderSectionRange, 1, 1));
    // 빈 섹션: index 끝 밖 시작, 0, 0이 아닌 정점 범위.
    fixture.data.lods[1].sections[1] = MeshRenderSection{};
    fixture.data.lods[1].sections[1].firstIndex = 7;
    CHECK(Fails(fixture.data, MeshIssue::RenderSectionRange, 1, 1));
    fixture.data.lods[1].sections[1].firstIndex = 0;
    fixture.data.lods[1].sections[1].maxVertex = 1;
    CHECK(Fails(fixture.data, MeshIssue::RenderSectionRange, 1, 1));
    fixture.data.lods[1].sections[1].maxVertex = 0;
    CHECK(ValidateStaticMeshRenderData(fixture.data));
}

// bounds·LOD 수.
TEST(StaticMeshValidation_RejectsBoundsAndLodCount) {
    Fixture fixture;
    fixture.data.box.min.y = NaN();
    CHECK(Fails(fixture.data, MeshIssue::RenderBounds, MeshNoIndex));
    fixture.data.box.min.y = 2.0;
    CHECK(Fails(fixture.data, MeshIssue::RenderBounds, MeshNoIndex));
    fixture.data.box.min.y = -1.0;
    fixture.data.sphereRadius = -0.5;
    CHECK(Fails(fixture.data, MeshIssue::RenderBounds, MeshNoIndex));
    fixture.data.sphereRadius = NaN();
    CHECK(Fails(fixture.data, MeshIssue::RenderBounds, MeshNoIndex));
    fixture.data.sphereRadius = 1.5;
    for (uint32 lod = 1; lod < StaticMeshMaxLods; ++lod) {
        StaticMeshRenderLod copy(fixture.allocator);
        Layout(copy, 4, 3, true, 6);
        REQUIRE(fixture.data.lods.TryPushBack(Move(copy)));
    }

    CHECK(ValidateStaticMeshRenderData(fixture.data));
    StaticMeshRenderLod ninth(fixture.allocator);
    Layout(ninth, 4, 3, true, 6);
    REQUIRE(fixture.data.lods.TryPushBack(Move(ninth)));
    CHECK(Fails(fixture.data, MeshIssue::RenderLodCount, MeshNoIndex));
}

// index 값: 정점 수 미만이어야 한다(16·32비트), data 크기가 rawBytes와 다르면 구간 위반.
TEST(StaticMeshValidation_IndexValues) {
    Fixture fixture;
    StaticMeshRenderLod& lod = fixture.Lod();
    Array<uint8> bytes(fixture.allocator);
    REQUIRE(bytes.TryResize(static_cast<usize>(lod.data.rawBytes)));
    const uint16 indices[6] = {0, 1, 2, 2, 1, 3};
    for (uint32 position = 0; position < 6; ++position) {
        StoreLittleEndian<uint16>(bytes.Data() + lod.indices.offset + position * 2,
                                  indices[position]);
    }

    CHECK(ValidateStaticMeshIndices(lod, 0, bytes.View()));
    StoreLittleEndian<uint16>(bytes.Data() + lod.indices.offset + 4 * 2, 4);
    MeshDiagnostic failure;
    auto out = ValidateStaticMeshIndices(lod, 2, bytes.View(), &failure);
    CHECK(!out && failure.code == AssetDiagnosticCode::CorruptPackage &&
          failure.issue == MeshIssue::RenderIndexValue && failure.index == 2 && failure.count == 4);
    auto shorter =
        ValidateStaticMeshIndices(lod, 0, bytes.View().Slice(0, bytes.Size() - 1), &failure);
    CHECK(!shorter && failure.issue == MeshIssue::RenderStreamRange);
    // 32비트.
    lod.indexFormat = graphics::IndexFormatNumber(IndexFormat::UInt32);
    lod.indices.byteSize = 6 * 4;
    lod.data.rawBytes = lod.indices.offset + lod.indices.byteSize;
    REQUIRE(bytes.TryResize(static_cast<usize>(lod.data.rawBytes)));
    const uint32 wide[6] = {0, 1, 2, 2, 1, 3};
    for (uint32 position = 0; position < 6; ++position) {
        StoreLittleEndian<uint32>(bytes.Data() + lod.indices.offset + position * 4, wide[position]);
    }

    CHECK(ValidateStaticMeshIndices(lod, 0, bytes.View()));
    StoreLittleEndian<uint32>(bytes.Data() + lod.indices.offset, 0x00010000u);
    auto high = ValidateStaticMeshIndices(lod, 0, bytes.View(), &failure);
    CHECK(!high && failure.issue == MeshIssue::RenderIndexValue && failure.count == 0);
}

// 확장 bounds: 확장이 없으면 저장 값 그대로(작은 반지름 유지), 있으면 상자 확장 + 반 대각선
// 반지름(UE CalculateExtendedBounds).
TEST(StaticMeshValidation_ExtendedBounds) {
    SystemAllocator allocator;
    StaticMesh mesh(allocator);
    mesh.renderData.box = Box<double>(Vec3d(-1.0, -2.0, -3.0), Vec3d(1.0, 2.0, 3.0));
    mesh.renderData.sphereRadius = 2.5;
    MeshBounds same = StaticMeshExtendedBounds(mesh);
    CHECK(same.sphereRadius == 2.5 && same.box.min.y == -2.0 && same.box.max.z == 3.0);
    mesh.positiveBoundsExtension = Vec3d(1.0, 0.0, 0.0);
    mesh.negativeBoundsExtension = Vec3d(0.0, 0.0, 1.0);
    MeshBounds extended = StaticMeshExtendedBounds(mesh);
    CHECK(extended.box.min.x == -1.0 && extended.box.max.x == 2.0 && extended.box.min.z == -4.0 &&
          extended.box.max.z == 3.0);
    // 반 크기 (1.5, 2, 3.5) → √(2.25 + 4 + 12.25) = √18.5.
    const double expected = 4.301162633521313;
    CHECK(extended.sphereRadius > expected - 1e-12 && extended.sphereRadius < expected + 1e-12);
}

// 진단 이름(렌더 데이터 규칙).
TEST(StaticMeshValidation_IssueNames) {
    CHECK(StringView(MeshIssueName(MeshIssue::RenderLodCount)) == StringView("RenderLodCount"));
    CHECK(StringView(MeshIssueName(MeshIssue::RenderSectionRange)) ==
          StringView("RenderSectionRange"));
    CHECK(StringView(MeshIssueName(MeshIssue::RenderTexCoordLayout)) ==
          StringView("RenderTexCoordLayout"));
}

} // namespace static_mesh_validation_tests_file
