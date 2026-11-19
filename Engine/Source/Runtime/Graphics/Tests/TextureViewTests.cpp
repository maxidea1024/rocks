#include <Core/Memory/SystemAllocator.h>
#include <Graphics/DeviceTypes.h>
#include <Graphics/ResourceAccess.h>
#include <Graphics/SubresourceStates.h>
#include <TestSupport/Test.h>

using namespace rocks;
using namespace rocks::graphics;

TEST(TextureView_ResolvesSingleAndSrgbPairFormats) {
    auto same = ResolveTextureViewFormat(PixelFormat::RGBA8Unorm, {});
    REQUIRE(same);
    CHECK(same.Value() == PixelFormat::RGBA8Unorm);
    auto pair =
        ResolveTextureViewFormat(PixelFormat::RGBA8Unorm, {PixelFormat::RGBA8UnormSrgb, 0, {}});
    REQUIRE(pair);
    CHECK(pair.Value() == PixelFormat::RGBA8UnormSrgb);
    auto back = ResolveTextureViewFormat(PixelFormat::BC7UnormSrgb, {PixelFormat::BC7Unorm, 0, {}});
    REQUIRE(back);
    CHECK(back.Value() == PixelFormat::BC7Unorm);
    // 짝이 아닌 형식, plane 범위 밖, 무효 형식은 거부한다.
    CHECK(ResolveTextureViewFormat(PixelFormat::RGBA8Unorm, {PixelFormat::BGRA8Unorm, 0, {}})
              .Error() == ErrorCode::InvalidArgument);
    CHECK(ResolveTextureViewFormat(PixelFormat::R16Float, {PixelFormat::Unknown, 1, {}}).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(ResolveTextureViewFormat(PixelFormat::Unknown, {}).Error() == ErrorCode::InvalidArgument);
}

TEST(TextureView_ResolvesPlanes) {
    auto depth = ResolveTextureViewFormat(PixelFormat::D24UnormS8Uint, {});
    REQUIRE(depth);
    CHECK(depth.Value() == PixelFormat::D24Unorm);
    auto stencil =
        ResolveTextureViewFormat(PixelFormat::D32FloatS8Uint, {PixelFormat::Unknown, 1, {}});
    REQUIRE(stencil);
    CHECK(stencil.Value() == PixelFormat::R8Uint);
    auto chroma = ResolveTextureViewFormat(PixelFormat::P010, {PixelFormat::RG16Unorm, 1, {}});
    REQUIRE(chroma);
    CHECK(chroma.Value() == PixelFormat::RG16Unorm);
    auto luma = ResolveTextureViewFormat(PixelFormat::NV12, {});
    REQUIRE(luma);
    CHECK(luma.Value() == PixelFormat::R8Unorm);
    // plane 형식이 아닌 형식과 plane 2는 거부한다.
    CHECK(ResolveTextureViewFormat(PixelFormat::NV12, {PixelFormat::RG8Unorm, 0, {}}).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(ResolveTextureViewFormat(PixelFormat::NV12, {PixelFormat::Unknown, 2, {}}).Error() ==
          ErrorCode::InvalidArgument);
}

TEST(TextureView_RequiredCapsPerUsageAndDimension) {
    TextureDesc desc;
    desc.usage = TextureUsage::Sampled;
    CHECK(RequiredPixelFormatCaps(desc) == PixelFormatCaps::Sampled);
    desc.mipLevels = 3;
    CHECK(RequiredPixelFormatCaps(desc) == (PixelFormatCaps::Sampled | PixelFormatCaps::Mipmaps));
    desc.mipLevels = 1;
    desc.dimension = TextureDimension::Texture3D;
    CHECK(RequiredPixelFormatCaps(desc) == (PixelFormatCaps::Sampled | PixelFormatCaps::Texture3D));
    desc.dimension = TextureDimension::TextureCubeArray;
    CHECK(RequiredPixelFormatCaps(desc) ==
          (PixelFormatCaps::Sampled | PixelFormatCaps::TextureCube));
    desc.dimension = TextureDimension::Texture2DArray;
    CHECK(RequiredPixelFormatCaps(desc) == PixelFormatCaps::Sampled);
    desc.dimension = TextureDimension::Texture2D;
    desc.usage = TextureUsage::RenderTarget;
    CHECK(RequiredPixelFormatCaps(desc) ==
          (PixelFormatCaps::RenderTarget | PixelFormatCaps::Sampled));
    desc.usage = TextureUsage::DepthStencil;
    CHECK(RequiredPixelFormatCaps(desc) == PixelFormatCaps::DepthStencil);
}

TEST(TextureView_SwizzleMustBeValid) {
    TextureViewDesc view;
    view.swizzle.green = static_cast<SwizzleSource>(9);
    CHECK(ResolveTextureViewFormat(PixelFormat::RGBA8Unorm, view).Error() ==
          ErrorCode::InvalidArgument);
    view.swizzle.green = SwizzleSource::One;
    CHECK(ResolveTextureViewFormat(PixelFormat::RGBA8Unorm, view).Value() ==
          PixelFormat::RGBA8Unorm);
}

TEST(TextureDesc_SliceCountsAndDataLayout) {
    TextureDesc desc;
    desc.width = 8;
    desc.height = 4;
    desc.format = PixelFormat::RGBA8Unorm;
    desc.mipLevels = 2;
    CHECK(TextureSliceCount(desc) == 1);
    CHECK(TextureDataBytes(desc) == 8 * 4 * 4 + 4 * 2 * 4);

    desc.dimension = TextureDimension::Texture2DArray;
    desc.arraySize = 3;
    CHECK(TextureSliceCount(desc) == 3);
    CHECK(TextureMipImageCount(desc, 1) == 3);
    CHECK(TextureMipBytes(desc, 0) == 3 * 8 * 4 * 4);
    CHECK(TextureDataBytes(desc) == 3 * (8 * 4 * 4 + 4 * 2 * 4));

    desc.dimension = TextureDimension::TextureCube;
    desc.arraySize = 1;
    desc.width = 4;
    desc.height = 4;
    CHECK(TextureSliceCount(desc) == CubeFaceCount);
    CHECK(TextureMipBytes(desc, 1) == 6 * 2 * 2 * 4);

    desc.dimension = TextureDimension::TextureCubeArray;
    desc.arraySize = 2;
    CHECK(TextureSliceCount(desc) == 12);

    // 3D는 slice 하나이고 mip마다 깊이가 준다. 전체 mip 수는 가장 긴 축이 정한다.
    desc.dimension = TextureDimension::Texture3D;
    desc.arraySize = 1;
    desc.width = 4;
    desc.height = 2;
    desc.depth = 8;
    desc.mipLevels = 4;
    CHECK(TextureSliceCount(desc) == 1);
    CHECK(TextureFullMipCount(desc) == 4);
    CHECK(TextureMipImageCount(desc, 0) == 8);
    CHECK(TextureMipImageCount(desc, 3) == 1);
    CHECK(TextureDataBytes(desc) == (4 * 2 * 8 + 2 * 1 * 4 + 1 * 1 * 2 + 1 * 1 * 1) * uint64{4});
    CHECK(TextureMipPlaneBytes(desc, 0, 0) == 4 * 2 * 8 * 4);
    CHECK(TextureMipPlaneBytes(desc, 0, 1) == 0);
    desc.mipLevels = 5;
    CHECK(TextureDataBytes(desc) == 0);
}

TEST(TextureDesc_ValidationRules) {
    TextureDesc desc;
    desc.width = 4;
    desc.height = 4;
    desc.format = PixelFormat::RGBA8Unorm;
    CHECK(ValidateTextureDesc(desc).IsOk());
    // 2D에 depth·arraySize는 1이어야 한다.
    desc.depth = 2;
    CHECK(ValidateTextureDesc(desc).Error() == ErrorCode::InvalidArgument);
    desc.depth = 1;
    desc.arraySize = 2;
    CHECK(ValidateTextureDesc(desc).Error() == ErrorCode::InvalidArgument);
    desc.dimension = TextureDimension::Texture2DArray;
    CHECK(ValidateTextureDesc(desc).IsOk());
    desc.arraySize = 0;
    CHECK(ValidateTextureDesc(desc).Error() == ErrorCode::InvalidArgument);
    // cube는 정사각이어야 한다.
    desc.arraySize = 1;
    desc.dimension = TextureDimension::TextureCube;
    desc.height = 2;
    CHECK(ValidateTextureDesc(desc).Error() == ErrorCode::InvalidArgument);
    desc.height = 4;
    CHECK(ValidateTextureDesc(desc).IsOk());
    // mip 수는 전체 mip 수 이하다.
    desc.mipLevels = 4;
    CHECK(ValidateTextureDesc(desc).Error() == ErrorCode::InvalidArgument);
    desc.mipLevels = 3;
    CHECK(ValidateTextureDesc(desc).IsOk());
    // 무효 swizzle·샘플러·차원.
    desc.swizzle.alpha = static_cast<SwizzleSource>(7);
    CHECK(ValidateTextureDesc(desc).Error() == ErrorCode::InvalidArgument);
    desc.swizzle = {};
    desc.sampler.addressW = static_cast<AddressMode>(9);
    CHECK(ValidateTextureDesc(desc).Error() == ErrorCode::InvalidArgument);
    desc.sampler = {};
    desc.dimension = static_cast<TextureDimension>(5);
    CHECK(ValidateTextureDesc(desc).Error() == ErrorCode::InvalidArgument);
    // 2D가 아닌 렌더 타깃·mip 사슬 렌더 타깃은 받는다(텍스처 GX2 C부). 다중 plane 형식의 2D 밖
    // 차원은 계약 밖이다.
    desc.dimension = TextureDimension::TextureCube;
    desc.mipLevels = 3;
    desc.usage = TextureUsage::RenderTarget;
    CHECK(ValidateTextureDesc(desc).IsOk());
    CHECK(HasPixelFormatCaps(RequiredPixelFormatCaps(desc), PixelFormatCaps::Mipmaps));
    desc.mipLevels = 1;
    desc.usage = TextureUsage::Sampled;
    desc.format = PixelFormat::D24UnormS8Uint;
    CHECK(ValidateTextureDesc(desc).Error() == ErrorCode::Unsupported);
    desc.format = PixelFormat::D32Float;
    CHECK(ValidateTextureDesc(desc).IsOk());
    // DepthStencil은 깊이 형식이어야 한다.
    desc.dimension = TextureDimension::Texture2D;
    desc.format = PixelFormat::RGBA8Unorm;
    desc.usage = TextureUsage::DepthStencil;
    CHECK(ValidateTextureDesc(desc).Error() == ErrorCode::InvalidArgument);
}

TEST(TextureDesc_DeviceLimits) {
    DeviceCaps caps;
    caps.limits.textureSize2D = 16;
    caps.limits.textureSizeCube = 8;
    caps.limits.textureSize3D = 4;
    caps.limits.textureArrayLayers = 6;
    TextureDesc desc;
    desc.width = 16;
    desc.height = 16;
    CHECK(CheckTextureLimits(caps, desc).IsOk());
    desc.width = 17;
    CHECK(CheckTextureLimits(caps, desc).Error() == ErrorCode::Unsupported);
    desc.width = 16;
    desc.dimension = TextureDimension::TextureCube;
    CHECK(CheckTextureLimits(caps, desc).Error() == ErrorCode::Unsupported);
    desc.width = 8;
    desc.height = 8;
    CHECK(CheckTextureLimits(caps, desc).IsOk());
    desc.dimension = TextureDimension::TextureCubeArray;
    CHECK(CheckTextureLimits(caps, desc).Error() == ErrorCode::Unsupported);
    caps.features.textureCubeArray = true;
    CHECK(CheckTextureLimits(caps, desc).IsOk());
    desc.arraySize = 2;
    CHECK(CheckTextureLimits(caps, desc).Error() == ErrorCode::Unsupported);
    desc.dimension = TextureDimension::Texture3D;
    desc.arraySize = 1;
    desc.width = 4;
    desc.height = 4;
    desc.depth = 5;
    CHECK(CheckTextureLimits(caps, desc).Error() == ErrorCode::Unsupported);
}

TEST(SubresourceRange_ResolveAndOverlap) {
    SubresourceRange all;
    const ResolvedSubresourceRange whole = ResolveSubresourceRange(all, 3, 6);
    CHECK(whole.firstMip == 0 && whole.mipCount == 3 && whole.firstSlice == 0 &&
          whole.sliceCount == 6);
    CHECK(whole.Contains(2, 5));
    CHECK(!whole.Contains(3, 0));
    const SubresourceRange face{1, 1, 4, 10};
    const ResolvedSubresourceRange clipped = ResolveSubresourceRange(face, 3, 6);
    CHECK(clipped.firstMip == 1 && clipped.mipCount == 1 && clipped.firstSlice == 4 &&
          clipped.sliceCount == 2);
    CHECK(ResolveSubresourceRange({0, 1, 6, 1}, 3, 6).IsEmpty());
    CHECK(ResolveSubresourceRange({3, 1, 0, 1}, 3, 6).IsEmpty());
    CHECK(ResolveSubresourceRange({0, 0, 0, 1}, 3, 6).IsEmpty());

    // 같은 mip이라도 slice가 다르면 겹치지 않는다.
    CHECK(!SubresourceRangesOverlap({0, 1, 0, 1}, {0, 1, 1, 1}));
    CHECK(SubresourceRangesOverlap({0, 1, 0, 2}, {0, 1, 1, 1}));
    CHECK(!SubresourceRangesOverlap({0, 1, 0, 1}, {1, 1, 0, 1}));
    CHECK(SubresourceRangesOverlap({}, {2, 1, 5, 1}));
}

TEST(SubresourceStates_InlineAndHeapStorage) {
    SystemAllocator allocator;
    SubresourceStates<uint32> small;
    REQUIRE(small.Reset(allocator, 4, 1, 7u));
    CHECK(small.Count() == 4);
    CHECK(small.At(3, 0) == 7u);
    SubresourceStates<uint32> large;
    REQUIRE(large.Reset(allocator, 5, 12, 1u));
    CHECK(large.Count() == 60);
    large.At(4, 11) = 9u;
    CHECK(large[SubresourceStates<uint32>::Index(4, 11, 5)] == 9u);
    SubresourceStates<uint32> moved(Move(large));
    CHECK(moved.At(4, 11) == 9u);
    CHECK(large.Count() == 0);
    SubresourceStates<uint32> copy;
    REQUIRE(copy.Reset(allocator, 5, 12, 0u));
    copy.CopyFrom(moved);
    CHECK(copy.At(4, 11) == 9u);
    CHECK(copy.Reset(allocator, 0, 1, 0u).Error() == ErrorCode::InvalidArgument);
}

TEST(TextureDesc_SizesThatOverflowAreZero) {
    TextureDesc desc;
    desc.dimension = TextureDimension::Texture2DArray;
    desc.format = PixelFormat::RGBA32Float;
    desc.width = 65536;
    desc.height = 65536;
    desc.arraySize = NumericLimits<uint32>::Max();
    // 이미지 하나(2^36 byte)와 층 수(2^32 − 1)의 곱은 uint64를 넘친다.
    CHECK(TextureMipBytes(desc, 0) == 0);
    CHECK(TextureMipPlaneBytes(desc, 0, 0) == 0);
    CHECK(TextureDataBytes(desc) == 0);
}

TEST(TextureDesc_ComparisonSamplerNeedsDepth) {
    TextureDesc desc;
    desc.width = 4;
    desc.height = 4;
    desc.format = PixelFormat::RGBA8Unorm;
    desc.sampler.comparison = true;
    CHECK(ValidateTextureDesc(desc).Error() == ErrorCode::InvalidArgument);
    desc.format = PixelFormat::D32Float;
    CHECK(ValidateTextureDesc(desc).IsOk());
    desc.format = PixelFormat::D24UnormS8Uint;
    CHECK(ValidateTextureDesc(desc).IsOk());
    // 비교 샘플러 텍스처의 view는 깊이 plane만 읽는다.
    CHECK(ResolveTextureViewFormat(PixelFormat::D24UnormS8Uint, {}, true).Value() ==
          PixelFormat::D24Unorm);
    CHECK(ResolveTextureViewFormat(PixelFormat::D24UnormS8Uint, {PixelFormat::Unknown, 1, {}}, true)
              .Error() == ErrorCode::InvalidArgument);
    CHECK(
        ResolveTextureViewFormat(PixelFormat::D24UnormS8Uint, {PixelFormat::Unknown, 1, {}}, false)
            .Value() == PixelFormat::R8Uint);
}

TEST(TextureDesc_StorageUsageRules) {
    // Storage는 초기 픽셀 없이 mip 여러 개를 가질 수 있고 Sampled·StorageWrite(·Mipmaps)를
    // 요구한다.
    TextureDesc desc;
    desc.width = 16;
    desc.height = 16;
    desc.format = PixelFormat::R32Float;
    desc.usage = TextureUsage::Storage;
    desc.mipLevels = 5;
    CHECK(ValidateTextureDesc(desc).IsOk());
    CHECK(RequiredPixelFormatCaps(desc) ==
          (PixelFormatCaps::Sampled | PixelFormatCaps::StorageWrite | PixelFormatCaps::Mipmaps));
    desc.mipLevels = 1;
    CHECK(RequiredPixelFormatCaps(desc) ==
          (PixelFormatCaps::Sampled | PixelFormatCaps::StorageWrite));
    desc.dimension = TextureDimension::Texture2DArray;
    desc.arraySize = 3;
    CHECK(ValidateTextureDesc(desc).IsOk());
    desc.dimension = TextureDimension::Texture3D;
    desc.arraySize = 1;
    desc.depth = 4;
    CHECK(ValidateTextureDesc(desc).IsOk());
    // cube·cube 배열 Storage도 받는다(텍스처 GX2 나머지 A부). 깊이 형식은 규칙 위반이다.
    desc.dimension = TextureDimension::TextureCube;
    desc.depth = 1;
    CHECK(ValidateTextureDesc(desc).IsOk());
    desc.dimension = TextureDimension::TextureCubeArray;
    desc.arraySize = 2;
    CHECK(ValidateTextureDesc(desc).IsOk());
    desc.dimension = TextureDimension::Texture2D;
    desc.arraySize = 1;
    desc.format = PixelFormat::D32Float;
    CHECK(ValidateTextureDesc(desc).Error() == ErrorCode::InvalidArgument);
}

// 용도 비트 묶음: 다섯 조합만 받고, 능력은 비트마다의 합이다. MSAA는 Storage와 묶지 않는다.
TEST(TextureDesc_UsageCombinations) {
    TextureDesc desc;
    desc.width = 8;
    desc.height = 8;
    desc.format = PixelFormat::RGBA8Unorm;
    const TextureUsage accepted[] = {TextureUsage::Sampled, TextureUsage::RenderTarget,
                                     TextureUsage::Storage,
                                     TextureUsage::RenderTarget | TextureUsage::Storage};
    for (TextureUsage usage : accepted) {
        desc.usage = usage;
        CHECK(ValidateTextureDesc(desc).IsOk());
    }

    const TextureUsage rejected[] = {TextureUsage::None,
                                     TextureUsage::Sampled | TextureUsage::RenderTarget,
                                     TextureUsage::Sampled | TextureUsage::Storage,
                                     TextureUsage::DepthStencil | TextureUsage::Storage,
                                     TextureUsage::DepthStencil | TextureUsage::RenderTarget};
    for (TextureUsage usage : rejected) {
        desc.usage = usage;
        CHECK(ValidateTextureDesc(desc).Error() == ErrorCode::InvalidArgument);
    }

    desc.usage = TextureUsage::RenderTarget | TextureUsage::Storage;
    desc.mipLevels = 2;
    CHECK(RequiredPixelFormatCaps(desc) ==
          (PixelFormatCaps::RenderTarget | PixelFormatCaps::Sampled |
           PixelFormatCaps::StorageWrite | PixelFormatCaps::Mipmaps));
    CHECK(HasTextureUsage(desc.usage, TextureUsage::RenderTarget));
    CHECK(HasTextureUsage(desc.usage, TextureUsage::Storage));
    CHECK(!HasTextureUsage(desc.usage, TextureUsage::Sampled));
    desc.mipLevels = 1;
    desc.sampleCount = 4;
    CHECK(ValidateTextureDesc(desc).Error() == ErrorCode::InvalidArgument);
    desc.usage = TextureUsage::RenderTarget;
    CHECK(ValidateTextureDesc(desc).IsOk());
}

// cube 계열의 쓰기 view는 면 단위 2D 배열이라 아무 면 범위를 받는다(읽기 view는 cube 정렬).
TEST(TextureView_CubeWritableViewsAreFaceArrays) {
    const TextureSubresources cubeArray{1, 12};
    TextureViewDesc face;
    face.firstSlice = 3;
    face.sliceCount = 1;
    face.access = ViewAccess::ReadWrite;
    auto written = ResolveTextureViewRange(TextureDimension::TextureCubeArray,
                                           TextureUsage::Storage, cubeArray, face);
    REQUIRE(written);
    CHECK(written.Value().firstSlice == 3 && written.Value().sliceCount == 1);
    CHECK(ResolveTextureViewRange(TextureDimension::TextureCube,
                                  TextureUsage::RenderTarget | TextureUsage::Storage,
                                  TextureSubresources{1, 6}, face)
              .IsOk());
    face.access = ViewAccess::Read;
    CHECK(ResolveTextureViewRange(TextureDimension::TextureCubeArray, TextureUsage::Storage,
                                  cubeArray, face)
              .Error() == ErrorCode::InvalidArgument);
    // 렌더 타깃만인 텍스처의 쓰기 view는 규칙 위반이다.
    face.access = ViewAccess::ReadWrite;
    CHECK(ResolveTextureViewRange(TextureDimension::TextureCube, TextureUsage::RenderTarget,
                                  TextureSubresources{1, 6}, face)
              .Error() == ErrorCode::InvalidArgument);
}

TEST(TextureView_RangeRules) {
    const TextureSubresources chain{5, 1};
    TextureViewDesc whole;
    auto all =
        ResolveTextureViewRange(TextureDimension::Texture2D, TextureUsage::Sampled, chain, whole);
    REQUIRE(all);
    CHECK(all.Value().firstMip == 0 && all.Value().mipCount == 5 && all.Value().sliceCount == 1);
    TextureViewDesc tail;
    tail.firstMip = 2;
    auto rest =
        ResolveTextureViewRange(TextureDimension::Texture2D, TextureUsage::Sampled, chain, tail);
    REQUIRE(rest);
    CHECK(rest.Value().firstMip == 2 && rest.Value().mipCount == 3);
    // 범위 밖·끝 넘침은 잘라 받지 않고 거부한다.
    TextureViewDesc outside;
    outside.firstMip = 5;
    CHECK(
        ResolveTextureViewRange(TextureDimension::Texture2D, TextureUsage::Sampled, chain, outside)
            .Error() == ErrorCode::InvalidArgument);
    TextureViewDesc overflow;
    overflow.firstMip = 3;
    overflow.mipCount = 3;
    CHECK(
        ResolveTextureViewRange(TextureDimension::Texture2D, TextureUsage::Sampled, chain, overflow)
            .Error() == ErrorCode::InvalidArgument);
    // 배열 slice 범위, cube는 6면 전체, cube 배열은 6의 배수, 3D는 slice 전체다.
    TextureViewDesc layers;
    layers.firstSlice = 1;
    layers.sliceCount = 2;
    auto arrayRange = ResolveTextureViewRange(TextureDimension::Texture2DArray,
                                              TextureUsage::Sampled, {1, 4}, layers);
    REQUIRE(arrayRange);
    CHECK(arrayRange.Value().firstSlice == 1 && arrayRange.Value().sliceCount == 2);
    CHECK(ResolveTextureViewRange(TextureDimension::TextureCube, TextureUsage::Sampled, {1, 6},
                                  layers)
              .Error() == ErrorCode::InvalidArgument);
    TextureViewDesc secondCube;
    secondCube.firstSlice = 6;
    secondCube.sliceCount = 6;
    CHECK(ResolveTextureViewRange(TextureDimension::TextureCubeArray, TextureUsage::Sampled,
                                  {1, 12}, secondCube)
              .IsOk());
    CHECK(ResolveTextureViewRange(TextureDimension::TextureCubeArray, TextureUsage::Sampled,
                                  {1, 12}, layers)
              .Error() == ErrorCode::InvalidArgument);
    TextureViewDesc firstSliceOnly;
    firstSliceOnly.sliceCount = 1;
    CHECK(ResolveTextureViewRange(TextureDimension::Texture3D, TextureUsage::Sampled, {3, 1},
                                  firstSliceOnly)
              .IsOk());
}

TEST(TextureView_WritableViewRules) {
    // 쓰기 view는 Storage 텍스처의 mip 하나, plane 0, swizzle 항등이다.
    TextureViewDesc write;
    write.access = ViewAccess::ReadWrite;
    write.firstMip = 1;
    write.mipCount = 1;
    CHECK(ResolveTextureViewRange(TextureDimension::Texture2D, TextureUsage::Storage, {4, 1}, write)
              .IsOk());
    CHECK(ResolveTextureViewRange(TextureDimension::Texture2D, TextureUsage::Sampled, {4, 1}, write)
              .Error() == ErrorCode::InvalidArgument);
    TextureViewDesc twoMips = write;
    twoMips.mipCount = 2;
    CHECK(
        ResolveTextureViewRange(TextureDimension::Texture2D, TextureUsage::Storage, {4, 1}, twoMips)
            .Error() == ErrorCode::InvalidArgument);
    TextureViewDesc swizzled = write;
    swizzled.swizzle.red = SwizzleSource::Green;
    CHECK(ResolveTextureViewRange(TextureDimension::Texture2D, TextureUsage::Storage, {4, 1},
                                  swizzled)
              .Error() == ErrorCode::InvalidArgument);
    // 마지막 mip 하나만 남으면 mipCount 0(끝까지)도 mip 하나다.
    TextureViewDesc last;
    last.access = ViewAccess::ReadWrite;
    last.firstMip = 3;
    CHECK(ResolveTextureViewRange(TextureDimension::Texture2D, TextureUsage::Storage, {4, 1}, last)
              .IsOk());
}

TEST(TextureView_BaseMipFollowsViewRangeSupport) {
    TextureViewDesc desc;
    desc.firstMip = 3;
    DeviceFeatures ranged;
    ranged.textureViewRanges = true;
    CHECK(TextureViewBaseMip(ranged, desc) == 0u);
    DeviceFeatures whole;
    CHECK(TextureViewBaseMip(whole, desc) == 3u);
}

TEST(TextureDesc_SampleCountRules) {
    // 표본 수는 1 이상 MaxSampleCount 이하의 2의 거듭제곱이다.
    CHECK(IsValidSampleCount(1) && IsValidSampleCount(4) && IsValidSampleCount(MaxSampleCount));
    CHECK(!IsValidSampleCount(0) && !IsValidSampleCount(3) &&
          !IsValidSampleCount(MaxSampleCount * 2));
    // MSAA는 RenderTarget·DepthStencil의 Texture2D, mip 1이고 Multisample 능력을 요구한다.
    TextureDesc desc;
    desc.width = 8;
    desc.height = 8;
    desc.format = PixelFormat::RGBA8Unorm;
    desc.usage = TextureUsage::RenderTarget;
    desc.sampleCount = 4;
    CHECK(ValidateTextureDesc(desc).IsOk());
    CHECK(HasPixelFormatCaps(RequiredPixelFormatCaps(desc), PixelFormatCaps::Multisample));
    desc.usage = TextureUsage::DepthStencil;
    desc.format = PixelFormat::D32Float;
    CHECK(ValidateTextureDesc(desc).IsOk());
    desc.sampleCount = 3;
    CHECK(ValidateTextureDesc(desc).Error() == ErrorCode::InvalidArgument);
    desc.sampleCount = 4;
    desc.usage = TextureUsage::Sampled;
    desc.format = PixelFormat::RGBA8Unorm;
    CHECK(ValidateTextureDesc(desc).Error() == ErrorCode::InvalidArgument);
    desc.usage = TextureUsage::Storage;
    CHECK(ValidateTextureDesc(desc).Error() == ErrorCode::InvalidArgument);
    desc.usage = TextureUsage::RenderTarget;
    desc.dimension = TextureDimension::Texture2DArray;
    desc.arraySize = 2;
    CHECK(ValidateTextureDesc(desc).Error() == ErrorCode::InvalidArgument);
    // 1표본은 Multisample 능력을 요구하지 않는다.
    desc.dimension = TextureDimension::Texture2D;
    desc.arraySize = 1;
    desc.sampleCount = 1;
    CHECK(!HasPixelFormatCaps(RequiredPixelFormatCaps(desc), PixelFormatCaps::Multisample));
    // 파이프라인 표본 수도 같은 규칙이다.
    PipelineDesc pipeline;
    pipeline.sampleCount = 8;
    CHECK(ValidatePipelineDesc(pipeline).IsOk());
    pipeline.sampleCount = 6;
    CHECK(ValidatePipelineDesc(pipeline).Error() == ErrorCode::InvalidArgument);
}

TEST(TextureDesc_AttachmentDimensionsAndRanges) {
    // 렌더 타깃은 모든 차원, 깊이는 3D를 뺀 차원이다(텍스처 GX2 C부).
    TextureDesc desc;
    desc.width = 8;
    desc.height = 8;
    desc.format = PixelFormat::RGBA8Unorm;
    desc.usage = TextureUsage::RenderTarget;
    desc.dimension = TextureDimension::Texture3D;
    desc.depth = 4;
    desc.mipLevels = 2;
    CHECK(ValidateTextureDesc(desc).IsOk());
    desc.dimension = TextureDimension::Texture2DArray;
    desc.depth = 1;
    desc.arraySize = 3;
    CHECK(ValidateTextureDesc(desc).IsOk());
    desc.usage = TextureUsage::DepthStencil;
    desc.format = PixelFormat::D32Float;
    CHECK(ValidateTextureDesc(desc).IsOk());
    desc.dimension = TextureDimension::Texture3D;
    desc.arraySize = 1;
    desc.depth = 4;
    CHECK(ValidateTextureDesc(desc).Error() == ErrorCode::InvalidArgument);
    desc.dimension = TextureDimension::Texture2DArray;
    desc.depth = 1;
    desc.arraySize = 2;
    desc.format = PixelFormat::D24UnormS8Uint;
    CHECK(ValidateTextureDesc(desc).Error() == ErrorCode::Unsupported);
    // attachment 범위: mip 수·slice 수, 3D는 그 mip의 깊이다.
    CHECK(IsAttachmentInRange(TextureDimension::Texture2DArray, 1, 2, 3, 1, 2));
    CHECK(!IsAttachmentInRange(TextureDimension::Texture2DArray, 1, 2, 3, 2, 0));
    CHECK(!IsAttachmentInRange(TextureDimension::Texture2DArray, 1, 2, 3, 0, 3));
    CHECK(IsAttachmentInRange(TextureDimension::Texture3D, 4, 2, 1, 0, 3));
    CHECK(IsAttachmentInRange(TextureDimension::Texture3D, 4, 2, 1, 1, 1));
    CHECK(!IsAttachmentInRange(TextureDimension::Texture3D, 4, 2, 1, 1, 2));
    // 상태 subresource: 3D는 mip 전체(slice 0), 그 밖은 (mip, slice) 하나다.
    const ResolvedSubresourceRange layer =
        AttachmentSubresource(TextureDimension::TextureCubeArray, 1, 7);
    CHECK(layer.firstMip == 1 && layer.mipCount == 1 && layer.firstSlice == 7 &&
          layer.sliceCount == 1);
    const ResolvedSubresourceRange plane = AttachmentSubresource(TextureDimension::Texture3D, 1, 3);
    CHECK(plane.firstMip == 1 && plane.firstSlice == 0 && plane.sliceCount == 1);
}
