#include <GraphicsTypes/ChannelSwizzle.h>
#include <GraphicsTypes/CompareFunction.h>
#include <GraphicsTypes/Sampler.h>
#include <GraphicsTypes/TextureDimension.h>
#include <Platform/Memory.h>
#include <TestSupport/Test.h>

using namespace rocks;
namespace sampler_tests_file {

using namespace rocks::graphics;

namespace {

StringView Text(const char* text) noexcept {
    return StringView(text, platform::CStringLength(text));
}

} // namespace

TEST(Sampler_FilterNamesFollowUeOrder) {
    // UE ESamplerFilter 순서(SF_Point … SF_AnisotropicLinear)다.
    const char* names[] = {"Point", "Bilinear", "Trilinear", "AnisotropicPoint",
                           "AnisotropicLinear"};
    for (uint32 value = 0; value < 5; ++value) {
        const auto filter = static_cast<Filter>(value);
        CHECK(platform::CStringCompare(FilterName(filter), names[value]) == 0);
        auto parsed = ParseFilter(Text(names[value]));
        REQUIRE(parsed);
        CHECK(parsed.Value() == filter);
    }
    CHECK(ParseFilter(Text("anisotropiclinear")).Value() == Filter::AnisotropicLinear);
    CHECK(ParseFilter(Text("Nearest")).Error() == ErrorCode::InvalidArgument);
    CHECK(ParseFilter(Text("Anisotropic")).Error() == ErrorCode::InvalidArgument);
    CHECK(IsAnisotropicFilter(Filter::AnisotropicPoint));
    CHECK(!IsAnisotropicFilter(Filter::Trilinear));
}

TEST(Sampler_ValidationCoversAddressWAndComparison) {
    SamplerDesc desc;
    CHECK(IsValidSamplerDesc(desc));
    CHECK(desc.addressW == AddressMode::Wrap);
    CHECK(!desc.comparison);
    desc.addressW = static_cast<AddressMode>(4);
    CHECK(!IsValidSamplerDesc(desc));
    desc.addressW = AddressMode::Border;
    desc.comparison = true;
    desc.compare = CompareFunction::GreaterEqual;
    CHECK(IsValidSamplerDesc(desc));
    desc.compare = static_cast<CompareFunction>(8);
    CHECK(!IsValidSamplerDesc(desc));
    desc.compare = CompareFunction::Always;
    desc.filter = static_cast<Filter>(5);
    CHECK(!IsValidSamplerDesc(desc));
    desc.filter = Filter::AnisotropicPoint;
    desc.maxAnisotropy = 17;
    CHECK(!IsValidSamplerDesc(desc));
    desc.maxAnisotropy = 16;
    CHECK(IsValidSamplerDesc(desc));
    // 비교 함수만 다른 두 샘플러는 다르다(장치 샘플러 캐시 키).
    SamplerDesc other = desc;
    other.compare = CompareFunction::Less;
    CHECK(!(other == desc));
}

TEST(CompareFunction_ValuesMatchApiOrderAndNames) {
    // D3D12_COMPARISON_FUNC − 1·VkCompareOp와 같은 순서다.
    const char* names[] = {"Never",   "Less",     "Equal",        "LessEqual",
                           "Greater", "NotEqual", "GreaterEqual", "Always"};
    for (uint32 value = 0; value < 8; ++value) {
        const auto function = static_cast<CompareFunction>(value);
        CHECK(IsValidCompareFunction(function));
        CHECK(platform::CStringCompare(CompareFunctionName(function), names[value]) == 0);
        CHECK(ParseCompareFunction(Text(names[value])).Value() == function);
    }
    CHECK(!IsValidCompareFunction(static_cast<CompareFunction>(8)));
    CHECK(ParseCompareFunction(Text("lessorequal")).Error() == ErrorCode::InvalidArgument);
}

TEST(TextureDimension_UeOrderAndClassification) {
    const char* names[] = {"Texture2D", "Texture2DArray", "Texture3D", "TextureCube",
                           "TextureCubeArray"};
    for (uint32 value = 0; value < 5; ++value) {
        const auto dimension = static_cast<TextureDimension>(value);
        CHECK(IsValidTextureDimension(dimension));
        CHECK(platform::CStringCompare(TextureDimensionName(dimension), names[value]) == 0);
        CHECK(ParseTextureDimension(Text(names[value])).Value() == dimension);
    }
    CHECK(!IsValidTextureDimension(static_cast<TextureDimension>(5)));
    CHECK(IsCubeDimension(TextureDimension::TextureCube));
    CHECK(IsCubeDimension(TextureDimension::TextureCubeArray));
    CHECK(!IsCubeDimension(TextureDimension::Texture2DArray));
    CHECK(IsArrayDimension(TextureDimension::Texture2DArray));
    CHECK(IsArrayDimension(TextureDimension::TextureCubeArray));
    CHECK(!IsArrayDimension(TextureDimension::Texture3D));
    // UE ECubeFace와 같은 면 순서다.
    CHECK(static_cast<uint32>(CubeFace::PositiveX) == 0);
    CHECK(static_cast<uint32>(CubeFace::NegativeZ) == CubeFaceCount - 1);
}

TEST(ChannelSwizzle_IdentityValidationAndComposition) {
    ChannelSwizzle identity;
    CHECK(identity.IsIdentity());
    CHECK(IsValidChannelSwizzle(identity));
    const ChannelSwizzle gray{SwizzleSource::Red, SwizzleSource::Red, SwizzleSource::Red,
                              SwizzleSource::One};
    CHECK(!gray.IsIdentity());
    CHECK(gray.Channel(3) == SwizzleSource::One);
    CHECK(gray.Channel(4) == SwizzleSource::Zero);
    ChannelSwizzle invalid;
    invalid.blue = static_cast<SwizzleSource>(6);
    CHECK(!IsValidChannelSwizzle(invalid));

    // OpenGL ES A8 흉내(R8 + (0, 0, 0, R)) 위에 사용자 swizzle(A, A, A, 1)을 합성한다.
    const ChannelSwizzle alphaEmulation{SwizzleSource::Zero, SwizzleSource::Zero,
                                        SwizzleSource::Zero, SwizzleSource::Red};
    const ChannelSwizzle broadcastAlpha{SwizzleSource::Alpha, SwizzleSource::Alpha,
                                        SwizzleSource::Alpha, SwizzleSource::One};
    const ChannelSwizzle composed = ComposeSwizzle(broadcastAlpha, alphaEmulation);
    CHECK(composed == gray);
    // 항등과의 합성은 그대로다.
    CHECK(ComposeSwizzle(identity, alphaEmulation) == alphaEmulation);
    CHECK(ComposeSwizzle(gray, identity) == gray);

    for (uint32 value = 0; value < 6; ++value) {
        const auto source = static_cast<SwizzleSource>(value);
        CHECK(ParseSwizzleSource(Text(SwizzleSourceName(source))).Value() == source);
    }
    CHECK(ParseSwizzleSource(Text("X")).Error() == ErrorCode::InvalidArgument);
}
} // namespace sampler_tests_file
