#include "ExchangeTestSupport.h"

#include <Base/InitializerList.h>

#include <Core/Memory/SystemAllocator.h>
#include <Platform/Memory.h>
#include <TestSupport/Test.h>
#include <UnrealExchange/ExchangeTextureSource.h>

// 교환 텍스처 원본 변환 시험이다(Docs/Design/UnrealExchange 4.6절 2번, 4.2절): 형식 9종(그대로
// 옮기기, BGRA 교환, BGRE 해제의 UE FromRGBE 식과 지수 0), 감마 이름, 다층·UDIM·위도경도 거부,
// 클래스 slice 규칙, 크기 상한, 형식·블록·mip·blob 배치 위반, 실패하면 빈 결과.
using namespace rocks;
using namespace rocks::exchange_test;
namespace exchange_texture_source_tests_file {

namespace {

// 원본 JSON의 바꿀 칸이다(기본은 4×2 Texture2D G8 한 층·한 블록·한 mip).
struct SourceSpec {
    StringView className = "Texture2D";
    StringView format = "G8";
    StringView layerFormat = {};
    StringView gamma = "sRGB";
    StringView layout = "u8";
    uint32 components = 1;
    uint32 sizeX = 4;
    uint32 sizeY = 2;
    uint32 slices = 1;
    uint32 mipX = 0;
    uint32 numMips = 1;
    uint32 layers = 1;
    uint32 blocks = 1;
    uint32 blockX = 0;
    bool longLat = false;
    // longLat 값의 JSON 원문이다(비면 longLat에서, "omit"이면 키를 뺀다).
    StringView longLatText = {};
    // 0이면 sizeX × sizeY × slices.
    uint64 count = 0;
    // layers·blocks·mips·pixels 배열에 실제로 쓰는 원소 수다(mipEntries 0은 numMips).
    uint32 layerEntries = 1;
    uint32 blockEntries = 1;
    uint32 mipEntries = 0;
    uint32 pixelEntries = 1;
    // 첫 mip의 sizeY·numSlices다(0은 블록 값).
    uint32 mipY = 0;
    uint32 mipSlices = 0;
};

// spec과 pixels로 에셋 JSON을 만들어 읽고 변환한다.
struct Converted {
    explicit Converted(Allocator& allocator) noexcept
        : asset(allocator), diagnostics(allocator), source(allocator) {
    }

    ExchangeAsset asset;
    ExchangeDiagnostics diagnostics;
    ExchangeTextureSource source;
    ExchangeTextureFailure failure;
    Result<void> result = Result<void>::Success();
};

// 배치 이름의 원소 byte 수다(시험이 쓰는 고정 배치만).
uint64 ElementBytes(StringView layout) noexcept {
    return layout == "u8" ? 1 : (layout == "f32" ? 4 : 2);
}

// 문서의 blob 참조는 리더 규칙(bytes = count × components × 원소 크기)을 지키고, pixels는 그와
// 달라도 된다(변환의 byte 수 검사를 시험한다).
void Convert(Allocator& allocator, const SourceSpec& spec, Span<const uint8> pixels,
             Converted& out) noexcept {
    const uint64 count =
        spec.count != 0 ? spec.count : static_cast<uint64>(spec.sizeX) * spec.sizeY * spec.slices;
    const uint64 referenced = count * spec.components * ElementBytes(spec.layout);
    const Sha256Digest digest = Sha256::Hash(pixels);
    char hex[Sha256Digest::HexBytes + 1];
    digest.ToHex(hex);
    StringBuilder<4096> text(allocator);
    const StringView layerFormat = spec.layerFormat.IsEmpty() ? spec.format : spec.layerFormat;
    const StringView longLat = spec.longLatText.IsEmpty()
                                   ? (spec.longLat ? StringView("true") : StringView("false"))
                                   : spec.longLatText;
    REQUIRE(text.TryAppendFormat(
        "{{\"objectPath\": \"/Game/T/A.A\", \"class\": \"{}\", \"ueClass\": \"/Script/Engine.{}\", "
        "\"dependencies\": [], \"diagnostics\": [], \"settings\": {{}}, \"data\": {{\"source\": "
        "{{\"format\": \"{}\", \"compression\": \"None\", \"lossyOrigin\": null, ",
        spec.className, spec.className, spec.format));
    if (longLat != "omit") {
        REQUIRE(text.TryAppendFormat("\"longLat\": {}, ", longLat));
    }

    REQUIRE(text.TryAppendFormat("\"numLayers\": {}, \"numBlocks\": {}, \"layers\": [", spec.layers,
                                 spec.blocks));
    for (uint32 layer = 0; layer < spec.layerEntries; ++layer) {
        REQUIRE(text.TryAppendFormat("{}{{\"format\": \"{}\", \"gammaSpace\": \"{}\"}}",
                                     layer == 0 ? "" : ", ", layerFormat, spec.gamma));
    }

    REQUIRE(text.TryAppend("], \"blocks\": ["));
    const uint32 mipEntries = spec.mipEntries != 0 ? spec.mipEntries : spec.numMips;
    for (uint32 block = 0; block < spec.blockEntries; ++block) {
        REQUIRE(text.TryAppendFormat(
            "{}{{\"blockX\": {}, \"blockY\": 0, \"sizeX\": {}, \"sizeY\": {}, \"numSlices\": {}, "
            "\"numMips\": {}, \"mips\": [",
            block == 0 ? "" : ", ", spec.blockX, spec.sizeX, spec.sizeY, spec.slices,
            spec.numMips));
        // 첫 mip 말고는 쓰지 않는다(같은 blob을 가리킨다).
        for (uint32 mip = 0; mip < mipEntries; ++mip) {
            const uint32 mipX = mip == 0 ? (spec.mipX != 0 ? spec.mipX : spec.sizeX) : 1;
            const uint32 mipY = mip == 0 ? (spec.mipY != 0 ? spec.mipY : spec.sizeY) : 1;
            const uint32 mipSlices =
                mip == 0 ? (spec.mipSlices != 0 ? spec.mipSlices : spec.slices) : 1;
            REQUIRE(text.TryAppendFormat(
                "{}{{\"sizeX\": {}, \"sizeY\": {}, \"numSlices\": {}, \"pixels\": [",
                mip == 0 ? "" : ", ", mipX, mipY, mipSlices));
            for (uint32 pixel = 0; pixel < spec.pixelEntries; ++pixel) {
                REQUIRE(text.TryAppendFormat(
                    "{}{{\"blob\": \"{}\", \"bytes\": {}, \"layout\": \"{}\", \"components\": {}, "
                    "\"count\": {}}}",
                    pixel == 0 ? "" : ", ", StringView(hex, Sha256Digest::HexBytes), referenced,
                    spec.layout, spec.components, count));
            }

            REQUIRE(text.TryAppend("]}"));
        }

        REQUIRE(text.TryAppend("]}"));
    }

    REQUIRE(text.TryAppend("]}}}"));
    const Span<const uint8> bytes(reinterpret_cast<const uint8*>(text.View().Data()),
                                  text.View().ByteCount());
    REQUIRE(ReadExchangeAssetDocument(bytes, allocator, out.asset, out.diagnostics));
    const Span<const uint8> blobs[] = {pixels};
    out.result = ConvertExchangeTextureSource(out.asset, Span<const Span<const uint8>>(blobs),
                                              out.source, out.failure);
}

bool Rejected(const Converted& converted, AssetDiagnosticCode code, ErrorCode error) noexcept {
    return !converted.result && converted.result.Error() == error &&
           converted.failure.code == code && converted.failure.error == error &&
           converted.source.format == TextureSourceFormat::Invalid &&
           converted.source.pixels.IsEmpty() && converted.source.sizeX == 0 &&
           converted.source.gamma == ExchangeGammaSpace::Linear;
}

float ReadFloat(const Array<uint8>& bytes, usize index) noexcept {
    float value = 0.0f;
    platform::MemoryCopy(&value, bytes.Data() + index * sizeof(float), sizeof(float));
    return value;
}

} // namespace

// 감마 번호는 UE EGammaSpace와 같다(GammaMismatch detail).
static_assert(static_cast<uint8>(ExchangeGammaSpace::Linear) == 0 &&
                  static_cast<uint8>(ExchangeGammaSpace::Pow22) == 1 &&
                  static_cast<uint8>(ExchangeGammaSpace::Srgb) == 2,
              "ExchangeGammaSpace must match UE EGammaSpace");

// 그대로 옮기는 형식 7종: byte가 같고 형식·크기·감마가 맞다.
TEST(UnrealExchange_TextureSourceCopyFormats) {
    SystemAllocator allocator;
    {
        struct Case {
            StringView format;
            StringView layout;
            uint32 components;
            uint32 elementBytes;
            TextureSourceFormat expected;
        };
        const Case cases[] = {
            {"G8", "u8", 1, 1, TextureSourceFormat::G8},
            {"RGBA16", "u16", 4, 2, TextureSourceFormat::RGBA16},
            {"RGBA16F", "f16", 4, 2, TextureSourceFormat::RGBA16F},
            {"G16", "u16", 1, 2, TextureSourceFormat::G16},
            {"RGBA32F", "f32", 4, 4, TextureSourceFormat::RGBA32F},
            {"R16F", "f16", 1, 2, TextureSourceFormat::R16F},
            {"R32F", "f32", 1, 4, TextureSourceFormat::R32F},
        };
        for (const Case& item : cases) {
            SourceSpec spec;
            spec.format = item.format;
            spec.layout = item.layout;
            spec.components = item.components;
            spec.gamma = "Linear";
            Array<uint8> pixels(allocator);
            const usize size = 4 * 2 * item.components * item.elementBytes;
            for (usize index = 0; index < size; ++index) {
                REQUIRE(pixels.TryPushBack(static_cast<uint8>(index * 7 + 1)));
            }

            Converted converted(allocator);
            Convert(allocator, spec, pixels.View(), converted);
            REQUIRE(converted.result);
            CHECK(converted.source.format == item.expected && converted.source.sizeX == 4 &&
                  converted.source.sizeY == 2 && converted.source.numSlices == 1 &&
                  converted.source.gamma == ExchangeGammaSpace::Linear);
            CHECK(converted.source.pixels.Size() == size &&
                  platform::MemoryCompare(converted.source.pixels.Data(), pixels.Data(), size) ==
                      0);
        }
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// BGRA8 → RGBA8(B·R 교환), BGRE8 → RGBA32F(UE FromRGBE: (값 + 0.5) · 2^(e − 136), 지수 0은
// 검정·알파 1), 감마 이름 셋.
TEST(UnrealExchange_TextureSourceConversions) {
    SystemAllocator allocator;
    {
        SourceSpec spec;
        spec.format = "BGRA8";
        spec.components = 4;
        spec.sizeX = 2;
        spec.sizeY = 1;
        const uint8 bgra[] = {10, 20, 30, 40, 50, 60, 70, 80};
        Converted swapped(allocator);
        Convert(allocator, spec, Span<const uint8>(bgra), swapped);
        REQUIRE(swapped.result);
        const uint8 rgba[] = {30, 20, 10, 40, 70, 60, 50, 80};
        CHECK(swapped.source.format == TextureSourceFormat::RGBA8 &&
              swapped.source.pixels.Size() == sizeof(rgba) &&
              platform::MemoryCompare(swapped.source.pixels.Data(), rgba, sizeof(rgba)) == 0 &&
              swapped.source.gamma == ExchangeGammaSpace::Srgb);

        spec.format = "BGRE8";
        spec.gamma = "Pow22";
        // texel 0: B 255, G 0, R 128, e 136 → 배율 1. texel 1: e 0 → (0, 0, 0, 1).
        const uint8 bgre[] = {255, 0, 128, 136, 9, 9, 9, 0};
        Converted decoded(allocator);
        Convert(allocator, spec, Span<const uint8>(bgre), decoded);
        REQUIRE(decoded.result);
        CHECK(decoded.source.format == TextureSourceFormat::RGBA32F &&
              decoded.source.pixels.Size() == 2 * 16 &&
              decoded.source.gamma == ExchangeGammaSpace::Pow22);
        CHECK(ReadFloat(decoded.source.pixels, 0) == 128.5f &&
              ReadFloat(decoded.source.pixels, 1) == 0.5f &&
              ReadFloat(decoded.source.pixels, 2) == 255.5f &&
              ReadFloat(decoded.source.pixels, 3) == 1.0f);
        CHECK(ReadFloat(decoded.source.pixels, 4) == 0.0f &&
              ReadFloat(decoded.source.pixels, 5) == 0.0f &&
              ReadFloat(decoded.source.pixels, 6) == 0.0f &&
              ReadFloat(decoded.source.pixels, 7) == 1.0f);

        // 작은 지수(e 1 → 2^-135, float 비정상 범위)와 큰 지수(e 255 → 2^119).
        const uint8 extremes[] = {0, 0, 1, 1, 0, 0, 1, 255};
        Converted edge(allocator);
        Convert(allocator, spec, Span<const uint8>(extremes), edge);
        REQUIRE(edge.result);
        const float tiny = 1.5f * 0x1p-135f;
        const float huge = 1.5f * 0x1p119f;
        CHECK(ReadFloat(edge.source.pixels, 0) == tiny &&
              ReadFloat(edge.source.pixels, 2) == 0.5f * 0x1p-135f);
        CHECK(ReadFloat(edge.source.pixels, 4) == huge);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 클래스 slice 규칙과 둘째 mip 무시: Cube 6, CubeArray 6의 배수, 2DArray·Volume 1 이상, 2D 1.
TEST(UnrealExchange_TextureSourceSlices) {
    SystemAllocator allocator;
    {
        struct Case {
            StringView className;
            uint32 slices;
            bool accepted;
        };
        const Case cases[] = {
            {"Texture2D", 1, true},         {"Texture2D", 2, false},
            {"TextureCube", 6, true},       {"TextureCube", 5, false},
            {"TextureCubeArray", 12, true}, {"TextureCubeArray", 9, false},
            {"Texture2DArray", 3, true},    {"VolumeTexture", 4, true},
        };
        for (const Case& item : cases) {
            SourceSpec spec;
            spec.className = item.className;
            spec.slices = item.slices;
            spec.sizeX = 1;
            spec.sizeY = 1;
            spec.numMips = 2;
            Array<uint8> pixels(allocator);
            REQUIRE(pixels.TryResize(item.slices));
            Converted converted(allocator);
            Convert(allocator, spec, pixels.View(), converted);
            if (item.accepted) {
                CHECK(converted.result && converted.source.numSlices == item.slices);
            } else {
                CHECK(Rejected(converted, AssetDiagnosticCode::InvalidSource,
                               ErrorCode::CorruptData));
            }
        }
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 다층·UDIM·위도경도는 UnsupportedSourceLayout, 그 밖의 형식 위반은 CorruptBundle, 크기 상한은
// InvalidSource / CapacityExceeded.
TEST(UnrealExchange_TextureSourceRejects) {
    SystemAllocator allocator;
    {
        const uint8 pixels[8] = {};
        const Span<const uint8> eight(pixels);
        auto rejects = [&](const SourceSpec& spec, Span<const uint8> bytes,
                           AssetDiagnosticCode code, ErrorCode error) noexcept {
            Converted converted(allocator);
            Convert(allocator, spec, bytes, converted);
            return Rejected(converted, code, error);
        };

        SourceSpec layered;
        layered.layers = 2;
        CHECK(rejects(layered, eight, AssetDiagnosticCode::UnsupportedSourceLayout,
                      ErrorCode::Unsupported));
        SourceSpec udim;
        udim.blocks = 2;
        CHECK(rejects(udim, eight, AssetDiagnosticCode::UnsupportedSourceLayout,
                      ErrorCode::Unsupported));
        SourceSpec longLat;
        longLat.longLat = true;
        CHECK(rejects(longLat, eight, AssetDiagnosticCode::UnsupportedSourceLayout,
                      ErrorCode::Unsupported));

        SourceSpec unknownFormat;
        unknownFormat.format = "BC1";
        SourceSpec layerDiffers;
        layerDiffers.layerFormat = "G16";
        SourceSpec badGamma;
        badGamma.gamma = "Gamma24";
        SourceSpec blockOffset;
        blockOffset.blockX = 1;
        SourceSpec mipDiffers;
        mipDiffers.mipX = 3;
        SourceSpec wrongLayout;
        wrongLayout.layout = "u16";
        SourceSpec wrongComponents;
        wrongComponents.components = 4;
        SourceSpec wrongCount;
        wrongCount.count = 7;
        SourceSpec noMips;
        noMips.numMips = 0;
        SourceSpec fewerMips;
        fewerMips.numMips = 2;
        fewerMips.mipEntries = 1;
        SourceSpec extraLayer;
        extraLayer.layerEntries = 2;
        SourceSpec extraBlock;
        extraBlock.blockEntries = 2;
        SourceSpec extraPixels;
        extraPixels.pixelEntries = 2;
        SourceSpec mipHeight;
        mipHeight.mipY = 3;
        SourceSpec mipSlices;
        mipSlices.mipSlices = 2;
        SourceSpec noLongLat;
        noLongLat.longLatText = "omit";
        SourceSpec textLongLat;
        textLongLat.longLatText = "\"no\"";
        SourceSpec zeroWidth;
        zeroWidth.sizeX = 0;
        zeroWidth.count = 1;
        for (const SourceSpec* spec :
             {&unknownFormat, &layerDiffers, &badGamma, &blockOffset, &mipDiffers, &wrongLayout,
              &wrongComponents, &wrongCount, &noMips, &fewerMips, &extraLayer, &extraBlock,
              &extraPixels, &mipHeight, &mipSlices, &noLongLat, &textLongLat, &zeroWidth}) {
            CHECK(
                rejects(*spec, eight, AssetDiagnosticCode::CorruptBundle, ErrorCode::CorruptData));
        }

        // 크기 상한(ImageDecodeLimits 기본 — 변환 전 판정이라 큰 픽셀을 만들지 않는다).
        SourceSpec wide;
        wide.sizeX = 16385;
        wide.sizeY = 1;
        CHECK(
            rejects(wide, eight, AssetDiagnosticCode::InvalidSource, ErrorCode::CapacityExceeded));
        SourceSpec deep;
        deep.className = "VolumeTexture";
        deep.sizeX = 1;
        deep.sizeY = 1;
        deep.slices = 2049;
        CHECK(
            rejects(deep, eight, AssetDiagnosticCode::InvalidSource, ErrorCode::CapacityExceeded));
        // BGRE8 입력 2 GiB(blob 상한 안)가 RGBA32F 8 GiB로 커진다.
        SourceSpec heavy;
        heavy.format = "BGRE8";
        heavy.layout = "u8";
        heavy.components = 4;
        heavy.sizeX = 16384;
        heavy.sizeY = 16384;
        heavy.className = "Texture2DArray";
        heavy.slices = 2;
        CHECK(
            rejects(heavy, eight, AssetDiagnosticCode::InvalidSource, ErrorCode::CapacityExceeded));

        // 텍스처가 아닌 클래스는 호출 계약 위반이다.
        SourceSpec mesh;
        mesh.className = "StaticMesh";
        CHECK(rejects(mesh, eight, AssetDiagnosticCode::InvalidSource, ErrorCode::InvalidArgument));

        // blob byte 수가 형식과 다름(참조 검사를 마친 byte라고 믿지 않는다).
        Converted shortBlob(allocator);
        Convert(allocator, SourceSpec{}, Span<const uint8>(pixels, 7), shortBlob);
        CHECK(Rejected(shortBlob, AssetDiagnosticCode::CorruptBundle, ErrorCode::CorruptData));

        // source가 없거나 data가 객체가 아니다.
        ExchangeAsset empty(allocator);
        empty.exchangeClass = ExchangeClass::Texture2D;
        ExchangeTextureSource out(allocator);
        ExchangeTextureFailure failure;
        CHECK(ConvertExchangeTextureSource(empty, {}, out, failure).Error() ==
                  ErrorCode::CorruptData &&
              failure.code == AssetDiagnosticCode::CorruptBundle);

        // blob 목록이 비었다(asset.blobs와 수가 다름 — 호출 계약 위반).
        Converted converted(allocator);
        Convert(allocator, SourceSpec{}, eight, converted);
        REQUIRE(converted.result);
        CHECK(ConvertExchangeTextureSource(converted.asset, {}, out, failure).Error() ==
                  ErrorCode::InvalidArgument &&
              failure.code == AssetDiagnosticCode::InvalidSource && out.pixels.IsEmpty());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

} // namespace exchange_texture_source_tests_file
