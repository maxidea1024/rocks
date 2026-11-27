#include <Core/Memory/SystemAllocator.h>
#include <GraphicsTypes/PixelFormat.h>
#include <TestSupport/Test.h>
#include <WorldIntegration/Textures/PlatformTextureSource.h>

// 쿠킹 텍스처 데이터 변환 시험이다(Docs/Design/RenderBase 18.5절 7항 "변환"·8항): 다섯 차원 대응,
// volume depth·slice 규칙, 샘플러·swizzle 값, 스트리밍 mip·꼬리 bulk 읽기, 음성(모르는 차원·형식
// 번호·샘플러 enum·swizzle, mip 수 불일치, 빈 bulk ref, reader 오류 전달).
using namespace rocks;

namespace platform_texture_source_tests_file {

SystemAllocator& Allocator_() noexcept {
    static SystemAllocator allocator;
    return allocator;
}

// 메모리 bulk 표다(bulk id 1 = 스트리밍 mip 0, 2 = 꼬리). failId의 읽기는 failError로 실패한다.
struct MemoryBulk {
    uint8 mip0[16] = {};
    uint8 tail[48] = {};
    uint32 failId = 0;
    ErrorCode failError = ErrorCode::IoError;
    uint32 reads = 0;

    static Result<Span<const uint8>> Read(void* raw, const BulkDataRef& ref) noexcept {
        auto& self = *static_cast<MemoryBulk*>(raw);
        ++self.reads;
        if (ref.bulkId == self.failId) {
            return Result<Span<const uint8>>::Failure(self.failError);
        }

        if (ref.bulkId == 1) {
            return Result<Span<const uint8>>::Success(Span<const uint8>(self.mip0));
        }

        if (ref.bulkId == 2) {
            return Result<Span<const uint8>>::Success(Span<const uint8>(self.tail));
        }

        return Result<Span<const uint8>>::Failure(ErrorCode::NotFound);
    }
};

constexpr uint32 MipCount = 3;
constexpr uint32 ResidentTailMips = 2;

// 2D RGBA8 4×4·mip 셋(mip 0 스트리밍, mip 1·2 꼬리)의 플랫폼 데이터를 채운다.
void FillTwoDimensional(TexturePlatformData& data) noexcept {
    data.dimension = TextureDimension::Texture2D;
    data.pixelFormat =
        static_cast<uint8>(graphics::PixelFormatNumber(graphics::PixelFormat::RGBA8Unorm));
    data.srgb = false;
    data.sizeX = 4;
    data.sizeY = 4;
    data.sizeZ = 1;
    data.numSlices = 1;
    data.numMips = MipCount;
    data.residentTailMips = ResidentTailMips;
    data.mips.Clear();
    const uint32 sides[MipCount] = {4, 2, 1};
    const uint64 offsets[MipCount] = {0, 0, 16};
    for (uint32 level = 0; level < MipCount; ++level) {
        TextureMipData mip;
        mip.sizeX = sides[level];
        mip.sizeY = sides[level];
        mip.sizeZ = 1;
        mip.byteSize = static_cast<uint64>(sides[level]) * sides[level] * 4;
        mip.tailOffset = offsets[level];
        if (level == 0) {
            mip.bulk.bulkId = 1;
            mip.bulk.rawBytes = 16;
        }

        REQUIRE(data.mips.TryPushBack(mip));
    }

    data.tailBulk.bulkId = 2;
    data.tailBulk.rawBytes = 48;
    data.sampler.filter = TextureSamplerFilter::Trilinear;
    data.sampler.maxAniso = 8;
    data.sampler.addressU = TextureAddress::Clamp;
    data.sampler.addressV = TextureAddress::Mirror;
    data.sampler.addressW = TextureAddress::Border;
    data.sampler.borderColor = TextureBorderColor::OpaqueWhite;
    data.swizzleR = TextureSwizzle::R;
    data.swizzleG = TextureSwizzle::R;
    data.swizzleB = TextureSwizzle::R;
    data.swizzleA = TextureSwizzle::One;
}

// 2D 데이터는 스트리밍 mip을 bulk 1에서, 꼬리를 bulk 2에서 읽고 크기·샘플러·swizzle을 그대로
// 옮긴다.
TEST(WorldIntegration_PlatformTextureSourceTwoDimensional) {
    TexturePlatformData data(Allocator_());
    FillTwoDimensional(data);
    MemoryBulk bulk;
    PlatformTextureSource out(Allocator_());
    REQUIRE(MakePlatformTextureSource(data, &MemoryBulk::Read, &bulk, out));
    const render::CookedTextureSource& source = out.source;
    CHECK(source.dimension == graphics::TextureDimension::Texture2D);
    CHECK(source.format == graphics::PixelFormat::RGBA8Unorm && !source.srgb);
    CHECK(source.width == 4 && source.height == 4 && source.depth == 1 && source.slices == 1);
    CHECK(source.residentTailMips == ResidentTailMips);
    REQUIRE(source.mips.Size() == MipCount);
    CHECK(source.mips[0].bulk.Data() == bulk.mip0 && source.mips[0].bulk.Size() == 16);
    CHECK(source.mips[1].bulk.IsEmpty() && source.mips[2].tailOffset == 16);
    CHECK(source.tail.Data() == bulk.tail && source.tail.Size() == 48);
    CHECK(bulk.reads == 2);
    CHECK(source.sampler.filter == graphics::Filter::Trilinear);
    CHECK(source.sampler.maxAnisotropy == 8);
    CHECK(source.sampler.addressU == graphics::AddressMode::Clamp);
    CHECK(source.sampler.addressV == graphics::AddressMode::Mirror);
    CHECK(source.sampler.addressW == graphics::AddressMode::Border);
    CHECK(source.sampler.borderColor == graphics::BorderColor::OpaqueWhite);
    CHECK(out.swizzle.red == graphics::SwizzleSource::Red &&
          out.swizzle.green == graphics::SwizzleSource::Red &&
          out.swizzle.blue == graphics::SwizzleSource::Red &&
          out.swizzle.alpha == graphics::SwizzleSource::One);
}

// 다섯 차원 대응과 volume 규칙(depth = sizeZ, slice 1), 배열·cube의 slice = numSlices.
TEST(WorldIntegration_PlatformTextureSourceDimensions) {
    struct Case {
        TextureDimension cooked;
        graphics::TextureDimension device;
    };

    const Case cases[] = {
        {TextureDimension::Texture2D, graphics::TextureDimension::Texture2D},
        {TextureDimension::TextureCube, graphics::TextureDimension::TextureCube},
        {TextureDimension::Texture2DArray, graphics::TextureDimension::Texture2DArray},
        {TextureDimension::TextureCubeArray, graphics::TextureDimension::TextureCubeArray},
        {TextureDimension::Volume, graphics::TextureDimension::Texture3D}};
    for (const Case& value : cases) {
        CHECK(GraphicsTextureDimension(value.cooked) == value.device);
    }

    CHECK(!graphics::IsValidTextureDimension(
        GraphicsTextureDimension(static_cast<TextureDimension>(0x7f))));
    TexturePlatformData data(Allocator_());
    FillTwoDimensional(data);
    MemoryBulk bulk;
    PlatformTextureSource out(Allocator_());
    data.dimension = TextureDimension::Volume;
    data.sizeZ = 4;
    data.numSlices = 7;
    for (TextureMipData& mip : data.mips) {
        mip.sizeZ = mip.sizeX;
    }

    REQUIRE(MakePlatformTextureSource(data, &MemoryBulk::Read, &bulk, out));
    CHECK(out.source.depth == 4 && out.source.slices == 1);
    CHECK(out.source.mips[0].depth == 4 && out.source.mips[2].depth == 1);
    data.dimension = TextureDimension::TextureCubeArray;
    data.numSlices = 12;
    REQUIRE(MakePlatformTextureSource(data, &MemoryBulk::Read, &bulk, out));
    CHECK(out.source.depth == 1 && out.source.slices == 12 && out.source.mips[0].depth == 1);
}

// 음성: 번호·enum 범위 밖, mip 수 불일치, 빈 bulk ref는 CorruptData이고 reader 오류는 그대로다.
TEST(WorldIntegration_PlatformTextureSourceRejectsCorruptData) {
    TexturePlatformData data(Allocator_());
    MemoryBulk bulk;
    PlatformTextureSource out(Allocator_());
    auto expect = [&](ErrorCode code) noexcept {
        auto made = MakePlatformTextureSource(data, &MemoryBulk::Read, &bulk, out);
        CHECK(!made && made.Error() == code);
        FillTwoDimensional(data);
        bulk.failId = 0;
    };

    FillTwoDimensional(data);
    data.dimension = static_cast<TextureDimension>(0x7f);
    expect(ErrorCode::CorruptData);
    data.pixelFormat = 0xff;
    expect(ErrorCode::CorruptData);
    data.sampler.filter = static_cast<TextureSamplerFilter>(0x7f);
    expect(ErrorCode::CorruptData);
    data.sampler.addressW = static_cast<TextureAddress>(0x7f);
    expect(ErrorCode::CorruptData);
    data.sampler.borderColor = static_cast<TextureBorderColor>(0x7f);
    expect(ErrorCode::CorruptData);
    data.swizzleA = static_cast<TextureSwizzle>(0x7f);
    expect(ErrorCode::CorruptData);
    data.numMips = MipCount + 1;
    expect(ErrorCode::CorruptData);
    data.mips[0].bulk = BulkDataRef{};
    expect(ErrorCode::CorruptData);
    data.tailBulk = BulkDataRef{};
    expect(ErrorCode::CorruptData);
    bulk.failId = 1;
    bulk.failError = ErrorCode::IoError;
    expect(ErrorCode::IoError);
    bulk.failId = 2;
    bulk.failError = ErrorCode::NotFound;
    expect(ErrorCode::NotFound);
}

} // namespace platform_texture_source_tests_file
