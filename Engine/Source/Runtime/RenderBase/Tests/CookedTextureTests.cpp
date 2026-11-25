#include "RecordingDevice.h"

#include <Base/NumericLimits.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Move.h>
#include <RenderBase/CookedTexture.h>
#include <RenderBase/RenderLane.h>
#include <TestSupport/Test.h>

// 쿠킹 텍스처 업로드 시험이다(Docs/Design/RenderBase 6.5절 "업로드", 18.5절 7항): 다섯 차원의 desc·
// 크기, 꼬리 정렬 패딩을 뺀 묶기, 블록 압축 크기(BC·ASTC 올림), 샘플러(이방성 0 → 4·16 자르기),
// 데이터 계약 위반(InvalidArgument + 원인)과 장치 못 함(UnsupportedCapability + 원인), 렌더 lane
// 작업의 Ready 통지.
using namespace rocks;
using namespace rocks::render;
using rocks::render::testing::RecordingDevice;

namespace cooked_texture_tests_file {

SystemAllocator& Allocator_() noexcept {
    static SystemAllocator allocator;
    return allocator;
}

// 넉넉한 장치 한도와 cube array 기능이다.
constexpr uint32 LimitSide = 16384;
constexpr uint32 LimitLayers = 2048;

graphics::DeviceCaps FullCaps() noexcept {
    graphics::DeviceCaps caps;
    caps.limits.textureSize2D = LimitSide;
    caps.limits.textureSize3D = LimitSide;
    caps.limits.textureSizeCube = LimitSide;
    caps.limits.textureArrayLayers = LimitLayers;
    caps.features.textureCubeArray = true;
    return caps;
}

constexpr graphics::PixelFormatCaps AllFormatCaps =
    graphics::PixelFormatCaps::Sampled | graphics::PixelFormatCaps::Texture3D |
    graphics::PixelFormatCaps::TextureCube | graphics::PixelFormatCaps::Filterable |
    graphics::PixelFormatCaps::Mipmaps;

// 시험 텍스처 하나다: mip 사슬과 꼬리 덩어리(쿠킹과 같은 16 byte 정렬, 사이 0)를 만든다. 스트리밍
// mip byte는 mip마다 값 (mip + 1) × 16 + slice 위치로 채운다.
struct Fixture {
    explicit Fixture(Allocator& allocator) noexcept
        : storage(allocator), tail(allocator), mips(allocator) {
    }

    // storage는 스트리밍 mip byte를 담는다(포인터가 바뀌지 않게 먼저 크기를 정한다).
    Array<uint8> storage;
    Array<uint8> tail;
    Array<CookedTextureMip> mips;
    CookedTextureSource source;

    bool Make(graphics::TextureDimension dimension, graphics::PixelFormat format, uint32 width,
              uint32 height, uint32 depth, uint32 slices, uint32 mipCount,
              uint32 resident) noexcept {
        source.dimension = dimension;
        source.format = format;
        source.srgb = graphics::GetPixelFormatInfo(format).srgb;
        source.width = width;
        source.height = height;
        source.depth = depth;
        source.slices = slices;
        source.residentTailMips = resident;
        const bool volume = dimension == graphics::TextureDimension::Texture3D;
        const uint32 streamed = mipCount - resident;
        uint64 streamedBytes = 0;
        for (uint32 level = 0; level < mipCount; ++level) {
            const uint32 mipDepth = volume ? graphics::MipExtent(depth, level) : 1u;
            const uint64 bytes =
                graphics::PixelFormatImageBytes(format, graphics::MipExtent(width, level),
                                                graphics::MipExtent(height, level)) *
                mipDepth * slices;
            if (level < streamed) {
                streamedBytes += bytes;
            }
        }

        if (!storage.TryResize(static_cast<usize>(streamedBytes))) {
            return false;
        }

        usize streamedOffset = 0;
        for (uint32 level = 0; level < mipCount; ++level) {
            CookedTextureMip mip;
            mip.width = graphics::MipExtent(width, level);
            mip.height = graphics::MipExtent(height, level);
            mip.depth = volume ? graphics::MipExtent(depth, level) : 1u;
            mip.byteSize =
                graphics::PixelFormatImageBytes(format, mip.width, mip.height) * mip.depth * slices;
            const usize bytes = static_cast<usize>(mip.byteSize);
            const uint8 fill = static_cast<uint8>((level + 1) * 16);
            if (level < streamed) {
                for (usize index = 0; index < bytes; ++index) {
                    storage[streamedOffset + index] = static_cast<uint8>(fill + index % 16);
                }

                mip.bulk = Span<const uint8>(storage.Data() + streamedOffset, bytes);
                streamedOffset += bytes;
            } else {
                const usize aligned =
                    static_cast<usize>((tail.Size() + CookedTextureTailAlignment - 1) &
                                       ~(CookedTextureTailAlignment - 1));
                if (!tail.TryResize(aligned)) {
                    return false;
                }

                mip.tailOffset = aligned;
                for (usize index = 0; index < bytes; ++index) {
                    if (!tail.TryPushBack(static_cast<uint8>(fill + index % 16))) {
                        return false;
                    }
                }
            }

            if (!mips.TryPushBack(mip)) {
                return false;
            }
        }

        source.mips = mips.View();
        source.tail = tail.View();
        source.sampler.filter = graphics::Filter::Trilinear;
        source.sampler.maxAnisotropy = 0;
        return true;
    }
};

Result<graphics::TextureDesc> Check(const CookedTextureSource& source,
                                    CookedTextureIssue& issue) noexcept {
    return CheckCookedTexture(source, FullCaps(), AllFormatCaps, &issue);
}

bool Rejects(const CookedTextureSource& source, ErrorCode code,
             CookedTextureIssue expected) noexcept {
    CookedTextureIssue issue = CookedTextureIssue::None;
    auto checked = Check(source, issue);
    return !checked && checked.Error() == code && issue == expected;
}

TEST(RenderBase_CookedTextureDescForEveryDimension) {
    struct Case {
        graphics::TextureDimension dimension;
        uint32 width;
        uint32 height;
        uint32 depth;
        uint32 slices;
        uint32 arraySize;
        uint32 mips;
    };

    constexpr Case Cases[] = {{graphics::TextureDimension::Texture2D, 8, 4, 1, 1, 1, 4},
                              {graphics::TextureDimension::Texture2DArray, 4, 4, 1, 3, 3, 3},
                              {graphics::TextureDimension::TextureCube, 4, 4, 1, 6, 1, 3},
                              {graphics::TextureDimension::TextureCubeArray, 2, 2, 1, 12, 2, 2},
                              {graphics::TextureDimension::Texture3D, 4, 4, 8, 1, 1, 4}};
    for (const Case& value : Cases) {
        Fixture fixture(Allocator_());
        REQUIRE(fixture.Make(value.dimension, graphics::PixelFormat::RGBA8Unorm, value.width,
                             value.height, value.depth, value.slices, value.mips, 2));
        CookedTextureIssue issue = CookedTextureIssue::InvalidFormat;
        auto desc = Check(fixture.source, issue);
        REQUIRE(desc);
        CHECK(issue == CookedTextureIssue::None);
        CHECK(desc.Value().dimension == value.dimension);
        CHECK(desc.Value().width == value.width && desc.Value().height == value.height);
        CHECK(desc.Value().depth == value.depth);
        CHECK(desc.Value().arraySize == value.arraySize);
        CHECK(desc.Value().mipLevels == value.mips);
        CHECK(desc.Value().usage == graphics::TextureUsage::Sampled);
        CHECK(desc.Value().swizzle.IsIdentity());
        CHECK(desc.Value().format == graphics::PixelFormat::RGBA8Unorm);
        // 묶은 byte = 장치 초기 픽셀 크기(꼬리 정렬 패딩 없음).
        auto pixels = PackCookedTexturePixels(Allocator_(), fixture.source);
        REQUIRE(pixels);
        CHECK(pixels.Value().Size() == graphics::TextureDataBytes(desc.Value()));
    }
}

TEST(RenderBase_CookedTexturePackDropsTailPadding) {
    // 3×3 RGBA8 mip 셋(36·4·4 byte): mip 0 스트리밍, mip 1·2 꼬리(오프셋 0·16 — mip 1 뒤 패딩 12).
    Fixture fixture(Allocator_());
    REQUIRE(fixture.Make(graphics::TextureDimension::Texture2D, graphics::PixelFormat::RGBA8Unorm,
                         3, 3, 1, 1, 2, 1));
    REQUIRE(fixture.mips[0].byteSize == 36 && fixture.mips[1].byteSize == 4);
    // 2×2 텍스처의 꼬리 두 mip: 16 byte와 4 byte, 둘째는 16에서 시작한다.
    Fixture tail(Allocator_());
    REQUIRE(tail.Make(graphics::TextureDimension::Texture2D, graphics::PixelFormat::R8Unorm, 4, 4,
                      1, 1, 3, 2));
    REQUIRE(tail.mips[1].tailOffset == 0 && tail.mips[2].tailOffset == 16);
    REQUIRE(tail.tail.Size() == 17);
    CookedTextureIssue issue = CookedTextureIssue::None;
    REQUIRE(Check(tail.source, issue));
    auto pixels = PackCookedTexturePixels(Allocator_(), tail.source);
    REQUIRE(pixels);
    const Span<const uint8> bytes = pixels.Value().View();
    REQUIRE(bytes.Size() == 16 + 4 + 1);
    // mip 0(스트리밍)은 16부터, mip 1은 32부터, mip 2는 48 한 byte다.
    CHECK(bytes[0] == 16 && bytes[15] == 31);
    CHECK(bytes[16] == 32 && bytes[19] == 35);
    CHECK(bytes[20] == 48);
}

TEST(RenderBase_CookedTextureBlockCompressedSizes) {
    // BC1 8×8 mip 넷: 32 + 8 + 8 + 8 byte(블록 4×4, 블록 8 byte, 작은 mip도 블록 하나).
    Fixture bc(Allocator_());
    REQUIRE(bc.Make(graphics::TextureDimension::Texture2D, graphics::PixelFormat::BC1Unorm, 8, 8, 1,
                    1, 4, 4));
    CHECK(bc.mips[0].byteSize == 32 && bc.mips[1].byteSize == 8 && bc.mips[3].byteSize == 8);
    CookedTextureIssue issue = CookedTextureIssue::None;
    CHECK(Check(bc.source, issue));
    // ASTC 6×6 13×13: 블록 수 올림 3 × 3 × 16 byte.
    Fixture astc(Allocator_());
    REQUIRE(astc.Make(graphics::TextureDimension::Texture2D, graphics::PixelFormat::ASTC6x6Unorm,
                      13, 13, 1, 1, 1, 1));
    CHECK(astc.mips[0].byteSize == 144);
    CHECK(Check(astc.source, issue));
    // BC는 최상위가 블록 배수여야 한다(ASTC는 아니다).
    Fixture unaligned(Allocator_());
    REQUIRE(unaligned.Make(graphics::TextureDimension::Texture2D, graphics::PixelFormat::BC1Unorm,
                           6, 8, 1, 1, 1, 1));
    CHECK(
        Rejects(unaligned.source, ErrorCode::InvalidArgument, CookedTextureIssue::BlockAlignment));
}

TEST(RenderBase_CookedTextureSampler) {
    Fixture fixture(Allocator_());
    REQUIRE(fixture.Make(graphics::TextureDimension::Texture2D, graphics::PixelFormat::RGBA8Unorm,
                         4, 4, 1, 1, 1, 1));
    fixture.source.sampler.filter = graphics::Filter::AnisotropicLinear;
    fixture.source.sampler.addressU = graphics::AddressMode::Clamp;
    fixture.source.sampler.addressV = graphics::AddressMode::Border;
    fixture.source.sampler.borderColor = graphics::BorderColor::OpaqueWhite;
    CookedTextureIssue issue = CookedTextureIssue::None;
    auto desc = Check(fixture.source, issue);
    REQUIRE(desc);
    CHECK(desc.Value().sampler.maxAnisotropy == CookedTextureDefaultMaxAnisotropy);
    CHECK(desc.Value().sampler.filter == graphics::Filter::AnisotropicLinear);
    CHECK(desc.Value().sampler.addressU == graphics::AddressMode::Clamp);
    CHECK(desc.Value().sampler.addressV == graphics::AddressMode::Border);
    CHECK(desc.Value().sampler.borderColor == graphics::BorderColor::OpaqueWhite);
    fixture.source.sampler.maxAnisotropy = 64;
    desc = Check(fixture.source, issue);
    REQUIRE(desc);
    CHECK(desc.Value().sampler.maxAnisotropy == 16);
    fixture.source.sampler.filter = static_cast<graphics::Filter>(9);
    CHECK(Rejects(fixture.source, ErrorCode::InvalidArgument, CookedTextureIssue::InvalidSampler));
    fixture.source.sampler.filter = graphics::Filter::Bilinear;
    fixture.source.sampler.comparison = true;
    CHECK(Rejects(fixture.source, ErrorCode::InvalidArgument, CookedTextureIssue::InvalidSampler));
}

TEST(RenderBase_CookedTextureRejectsCorruptData) {
    auto fresh = [](Fixture& fixture) noexcept {
        return fixture.Make(graphics::TextureDimension::Texture2D,
                            graphics::PixelFormat::RGBA8Unorm, 8, 8, 1, 1, 4, 2);
    };
    const ErrorCode Corrupt = ErrorCode::InvalidArgument;
    {
        Fixture fixture(Allocator_());
        REQUIRE(fresh(fixture));
        fixture.source.dimension = static_cast<graphics::TextureDimension>(7);
        CHECK(Rejects(fixture.source, Corrupt, CookedTextureIssue::InvalidDimension));
    }

    {
        Fixture fixture(Allocator_());
        REQUIRE(fresh(fixture));
        fixture.source.format = graphics::PixelFormat::Unknown;
        CHECK(Rejects(fixture.source, Corrupt, CookedTextureIssue::InvalidFormat));
        fixture.source.format = graphics::PixelFormat::D32Float;
        CHECK(Rejects(fixture.source, Corrupt, CookedTextureIssue::NonColorFormat));
        fixture.source.format = graphics::PixelFormat::RGBA8UnormSrgb;
        fixture.source.srgb = false;
        CHECK(Rejects(fixture.source, Corrupt, CookedTextureIssue::SrgbMismatch));
        fixture.source.format = graphics::PixelFormat::RGBA8Unorm;
        fixture.source.srgb = true;
        CHECK(Rejects(fixture.source, Corrupt, CookedTextureIssue::SrgbMismatch));
    }

    {
        Fixture fixture(Allocator_());
        REQUIRE(fresh(fixture));
        fixture.source.slices = 2;
        CHECK(Rejects(fixture.source, Corrupt, CookedTextureIssue::InvalidExtent));
        fixture.source.slices = 1;
        fixture.source.width = 0;
        CHECK(Rejects(fixture.source, Corrupt, CookedTextureIssue::InvalidExtent));
        fixture.source.width = 8;
        fixture.source.dimension = graphics::TextureDimension::TextureCube;
        CHECK(Rejects(fixture.source, Corrupt, CookedTextureIssue::InvalidExtent));
        fixture.source.dimension = graphics::TextureDimension::Texture2D;
        // mip 수: 0, 전체(8×8 → 4) 초과.
        fixture.source.mips = Span<const CookedTextureMip>();
        CHECK(Rejects(fixture.source, Corrupt, CookedTextureIssue::MipCount));
        fixture.source.mips = fixture.mips.View();
        fixture.source.residentTailMips = 0;
        CHECK(Rejects(fixture.source, Corrupt, CookedTextureIssue::ResidentTail));
        fixture.source.residentTailMips = 5;
        CHECK(Rejects(fixture.source, Corrupt, CookedTextureIssue::ResidentTail));
        fixture.source.residentTailMips = 2;
    }

    {
        // mip 크기 사슬·byte 수(크기 불일치).
        Fixture fixture(Allocator_());
        REQUIRE(fresh(fixture));
        fixture.mips[1].width = 3;
        CHECK(Rejects(fixture.source, Corrupt, CookedTextureIssue::MipExtent));
        fixture.mips[1].width = 4;
        fixture.mips[2].byteSize += 4;
        CHECK(Rejects(fixture.source, Corrupt, CookedTextureIssue::MipByteSize));
    }

    {
        // 잘린 스트리밍 bulk.
        Fixture fixture(Allocator_());
        REQUIRE(fresh(fixture));
        fixture.mips[0].bulk =
            Span<const uint8>(fixture.mips[0].bulk.Data(), fixture.mips[0].bulk.Size() - 1);
        CHECK(Rejects(fixture.source, Corrupt, CookedTextureIssue::BulkSize));
        CHECK(!PackCookedTexturePixels(Allocator_(), fixture.source));
    }

    {
        // 꼬리: 정렬 아님, 앞 mip과 겹침, 잘린 덩어리.
        Fixture fixture(Allocator_());
        REQUIRE(fresh(fixture));
        const uint64 offset = fixture.mips[3].tailOffset;
        fixture.mips[3].tailOffset = offset + 4;
        CHECK(Rejects(fixture.source, Corrupt, CookedTextureIssue::TailAlignment));
        fixture.mips[3].tailOffset = 0;
        CHECK(Rejects(fixture.source, Corrupt, CookedTextureIssue::TailRange));
        fixture.mips[3].tailOffset = offset;
        fixture.source.tail = Span<const uint8>(fixture.tail.Data(), fixture.tail.Size() - 1);
        CHECK(Rejects(fixture.source, Corrupt, CookedTextureIssue::TailRange));
        CHECK(!PackCookedTexturePixels(Allocator_(), fixture.source));
        fixture.mips[3].tailOffset = NumericLimits<uint64>::Max() - 15;
        CHECK(Rejects(fixture.source, Corrupt, CookedTextureIssue::TailRange));
    }
}

TEST(RenderBase_CookedTextureRejectsUnsupportedDevice) {
    const ErrorCode Unsupported = ErrorCode::UnsupportedCapability;
    Fixture plane(Allocator_());
    REQUIRE(plane.Make(graphics::TextureDimension::Texture2D, graphics::PixelFormat::RGBA16Float, 8,
                       8, 1, 1, 2, 1));
    CookedTextureIssue issue = CookedTextureIssue::None;
    // 형식 능력: 없음, mip 둘에 Mipmaps 없음.
    auto checked =
        CheckCookedTexture(plane.source, FullCaps(), graphics::PixelFormatCaps::None, &issue);
    CHECK(!checked && checked.Error() == Unsupported &&
          issue == CookedTextureIssue::UnsupportedFormat);
    checked =
        CheckCookedTexture(plane.source, FullCaps(), graphics::PixelFormatCaps::Sampled, &issue);
    CHECK(!checked && issue == CookedTextureIssue::UnsupportedFormat);
    // volume에 Texture3D 없음, cube에 TextureCube 없음.
    Fixture volume(Allocator_());
    REQUIRE(volume.Make(graphics::TextureDimension::Texture3D, graphics::PixelFormat::RGBA8Unorm, 4,
                        4, 4, 1, 1, 1));
    checked = CheckCookedTexture(
        volume.source, FullCaps(),
        graphics::PixelFormatCaps::Sampled | graphics::PixelFormatCaps::TextureCube, &issue);
    CHECK(!checked && issue == CookedTextureIssue::UnsupportedFormat);
    Fixture cube(Allocator_());
    REQUIRE(cube.Make(graphics::TextureDimension::TextureCube, graphics::PixelFormat::RGBA8Unorm, 4,
                      4, 1, 6, 1, 1));
    checked = CheckCookedTexture(
        cube.source, FullCaps(),
        graphics::PixelFormatCaps::Sampled | graphics::PixelFormatCaps::Texture3D, &issue);
    CHECK(!checked && issue == CookedTextureIssue::UnsupportedFormat);
    // cube array 기능 없음.
    Fixture cubes(Allocator_());
    REQUIRE(cubes.Make(graphics::TextureDimension::TextureCubeArray,
                       graphics::PixelFormat::RGBA8Unorm, 4, 4, 1, 12, 1, 1));
    graphics::DeviceCaps noCubeArray = FullCaps();
    noCubeArray.features.textureCubeArray = false;
    checked = CheckCookedTexture(cubes.source, noCubeArray, AllFormatCaps, &issue);
    CHECK(!checked && checked.Error() == Unsupported &&
          issue == CookedTextureIssue::UnsupportedDimension);
    // 장치 한도: 한 변, slice 수.
    graphics::DeviceCaps small = FullCaps();
    small.limits.textureSize2D = 4;
    checked = CheckCookedTexture(plane.source, small, AllFormatCaps, &issue);
    CHECK(!checked && checked.Error() == Unsupported && issue == CookedTextureIssue::DeviceLimit);
    Fixture layers(Allocator_());
    REQUIRE(layers.Make(graphics::TextureDimension::Texture2DArray,
                        graphics::PixelFormat::RGBA8Unorm, 2, 2, 1, 5, 1, 1));
    graphics::DeviceCaps fewLayers = FullCaps();
    fewLayers.limits.textureArrayLayers = 4;
    checked = CheckCookedTexture(layers.source, fewLayers, AllFormatCaps, &issue);
    CHECK(!checked && issue == CookedTextureIssue::DeviceLimit);
    CHECK(StringView(CookedTextureIssueName(CookedTextureIssue::DeviceLimit)) ==
          StringView("DeviceLimit"));
    CHECK(StringView(CookedTextureIssueName(static_cast<CookedTextureIssue>(200))) ==
          StringView("Unknown"));
}

TEST(RenderBase_CookedTextureCreatesOnLane) {
    RecordingDevice device(Allocator_());
    RenderLane lane(Allocator_(), device);
    RenderLaneConfig config;
    config.mode = RenderLaneMode::Inline;
    config.hangTimeoutMs = 0;
    REQUIRE(lane.Start(config));
    Fixture fixture(Allocator_());
    REQUIRE(fixture.Make(graphics::TextureDimension::Texture2DArray,
                         graphics::PixelFormat::RGBA8Unorm, 4, 4, 1, 2, 3, 1));
    auto handle = lane.ReserveHandle(RenderResourceKind::Texture);
    REQUIRE(handle);
    CookedTextureIssue issue = CookedTextureIssue::None;
    auto desc = CheckCookedTexture(fixture.source, FullCaps(), AllFormatCaps, &issue);
    REQUIRE(desc);
    auto pixels = PackCookedTexturePixels(Allocator_(), fixture.source);
    REQUIRE(pixels);
    CookedTextureUpload upload;
    upload.handle = handle.Value();
    upload.desc = desc.Value();
    upload.pixels = Move(pixels.Value());
    REQUIRE(lane.PostResource(Move(upload), &CreateCookedTextureOnLane));
    auto marker = lane.PostMarker();
    REQUIRE(marker);
    REQUIRE(lane.WaitMarker(marker.Value()));
    Array<RenderNotice> notices(Allocator_());
    REQUIRE(lane.Notices().TakeAll(notices));
    bool ready = false;
    for (const RenderNotice& notice : notices) {
        ready = ready ||
                (notice.kind == RenderNoticeKind::ResourceReady && notice.handle == handle.Value());
    }

    CHECK(ready);
    CHECK(lane.Handles().Texture(handle.Value()));
    REQUIRE(lane.Retire(handle.Value()));
    REQUIRE(lane.Shutdown());
    CHECK(lane.Handles().Count() == 0 && device.PendingRetireCount() == 0);
}

} // namespace cooked_texture_tests_file
